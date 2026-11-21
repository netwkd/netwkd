#include "netw/predict/engine.hpp"

#include "netw/api/interpolate.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/predict_slot_engine.hpp"
#include "netw/api/predict_stats.hpp"
#include "netw/api/prediction.hpp"
#include "netw/property_set_builder.hpp"
#include "netw/repl/sync_pipeline.hpp"
#include "netw/script/model.hpp"
#include "netw/wire/registry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "godot/callable.hpp"
#include "godot/class_db.hpp"
#include "godot/engine.hpp"
#include "godot/object.hpp"
#include "godot/os.hpp"
#include "godot/spatial_node.hpp"
#include "netw/colliders.hpp"
#include "netw/colors.hpp"
#include "netw/fault_book.hpp"
#include "netw/game_call.hpp"
#include "netw/log.hpp"
#include "netw/object_port.hpp"
#include "netw/predict/axes.hpp"
#include "netw/predict/decl.hpp"
#include "netw/predict/frames.hpp"
#include "netw/predict/tick_work.hpp"
#include "netw/profile.hpp"
#include "netw/project.hpp"
#include "netw/stage.hpp"
#include "netw/store/port.hpp"
#include "src/predict/engine_support.hpp"

using namespace godot;

namespace netw {

using repl::PropertyBindingPtr;
using repl::PropertyColumn;
using repl::PropertySet;

namespace {

bool predicted_agrees(
    predict::Slot &r_row,
    int64_t p_key,
    const predict::StateRow &p_authority,
    double &r_divergence
) {
    predict::StateRow predicted;
    if (!r_row.book.read_row(r_row.wiring.codec, p_key, predicted)
        || !predicted.any()) {
        return false;
    }
    fill_correction_tolerances(
        r_row,
        r_row.config.epsilon,
        r_row.correction_tolerances
    );
    bool diverged = true;
    r_divergence = predict::judge_rows(
        r_row.wiring,
        predicted,
        p_authority,
        r_row.correction_tolerances,
        r_row.judged_errors,
        diverged
    );
    return !diverged;
}

bool roster_index_valid(int p_roster) {
    return p_roster >= 0 && p_roster < 4;
}

bool authors_pass(int p_role) {
    return p_role == int(Role::PREDICT) || p_role == int(Role::CONSUME)
        || p_role == int(Role::HOST_LOCAL);
}

bool steps_in_phase(const predict::Slot &p_row, int p_phase) {
    const int role = p_row.config.role;
    const int schedule = p_row.config.schedule;
    const bool tick = schedule == int(Schedule::TICK);
    const bool frame = schedule == int(Schedule::FRAME);
    const bool stepped = schedule == int(Schedule::STEPPED);
    const bool authoring_fallback
        = role == int(Role::REMOTE) && p_row.quarantine.latched;
    switch (p_phase) {
        case NetwPredictionEngine::PASS_ISLAND_TICK:
            return pass_admits(int(Schedule::TICK), schedule)
                && authors_pass(role);
        case NetwPredictionEngine::PASS_ISLAND_FRAME:
            return pass_admits(int(Schedule::FRAME), schedule)
                && authors_pass(role);
        case NetwPredictionEngine::PASS_JOINT:
            return p_row.config.island == NetwPredictionEngine::ISLAND_JOINT
                && authors_pass(role);
        case NetwPredictionEngine::PASS_TICK:
            return tick || role == int(Role::PREDICT)
                || role == int(Role::HOST_LOCAL) || authoring_fallback;
        case NetwPredictionEngine::PASS_FRAME:
            return frame
                && (authors_pass(role) || authoring_fallback
                    || role == int(Role::SIMULATE));
        case NetwPredictionEngine::PASS_FINALIZE_FRAME:
            return frame && role == int(Role::PREDICT);
        case NetwPredictionEngine::PASS_STEPPED:
            return stepped
                && (authors_pass(role) || authoring_fallback
                    || role == int(Role::SIMULATE));
        default:
            return false;
    }
}

String schedule_label(NetwSimulation::Schedule p_schedule) {
    switch (p_schedule) {
        case NetwSimulation::SCHEDULE_TICK:
            return "SCHEDULE_TICK";
        case NetwSimulation::SCHEDULE_FRAME:
            return "SCHEDULE_FRAME";
        case NetwSimulation::SCHEDULE_STEPPED:
            return "SCHEDULE_STEPPED";
        case NetwSimulation::SCHEDULE_AUTO:
            return "SCHEDULE_AUTO";
        default:
            return String::num_int64(p_schedule);
    }
}

String refusal_row(
    const NetwEntityRecord &p_record,
    Node *p_node,
    const NetwPrediction *p_handle
) {
    const String schedule = p_handle != nullptr
        ? schedule_label(NetwMultiplayer::predict_schedule_of(p_record, p_node))
        : String("no schedule");
    return String("'") + subject_name(p_node) + String("' on ") + schedule;
}

bool same_roster(
    const LocalVector<RID> &p_left,
    const LocalVector<RID> &p_right
) {
    if (p_left.size() != p_right.size()) {
        return false;
    }
    for (uint32_t at = 0; at < p_left.size(); ++at) {
        if (p_left[at] != p_right[at]) {
            return false;
        }
    }
    return true;
}

ObjectID wrapper_id(const NetwMultiplayer *p_session, const RID &p_entity) {
    return p_session != nullptr
        ? gd::instance_id(
              p_session->wrapper_for_id(int64_t(p_entity.get_id())).ptr()
          )
        : ObjectID();
}

const NetwEntity *roster_wrapper(
    const predict::Slot *p_row,
    const RID &p_member
) {
    if (p_row == nullptr) {
        return nullptr;
    }
    for (const predict::EntityRoster &held : p_row->rosters) {
        const int at = held.index_of(p_member);
        if (at >= 0) {
            return Object::cast_to<NetwEntity>(
                gd::object_of(held.wrappers[uint32_t(at)])
            );
        }
    }
    return nullptr;
}

String entity_key(const NetwEntityRecord *p_record, Node *p_root) {
    if (p_record == nullptr) {
        return String();
    }
    const String named = String(p_record->get_entity_id());
    if (!named.is_empty()) {
        return named;
    }
    const int64_t route = p_record->get_route();
    if (route > 0) {
        return String("route:") + String::num_int64(route);
    }
    if (p_root != nullptr) {
        return String("path:") + String(p_root->get_path());
    }
    return String();
}

PackedStringArray sorted_ids(
    const NetwPredictionEngine &p_engine,
    int64_t p_slot,
    const LocalVector<RID> &p_roster
) {
    PackedStringArray ids;
    for (const RID &member : p_roster) {
        ids.push_back(p_engine.member_key(p_slot, member));
    }
    ids.sort();
    return ids;
}

} // namespace

bool NetwPredictionEngine::roster_open() {
    if (depth == 0) {
        return true;
    }
    mutations_refused += 1;
    fault::raise(session, NETW_FAULT_CHANGED_IN_STEP, {});
    return false;
}

int NetwPredictionEngine::pass_depth() const {
    return depth;
}

Dictionary NetwPredictionEngine::open_topology_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? Dictionary() : row->rows.open_topology;
}

void NetwPredictionEngine::set_open_topology(
    int64_t p_slot,
    const Dictionary &p_row
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->rows.open_topology = p_row;
        row->topology_open = false;
    }
}

LocalVector<RID> NetwPredictionEngine::roster_list(
    int64_t p_slot,
    int p_roster
) {
    LocalVector<RID> live;
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr || !roster_index_valid(p_roster)) {
        return live;
    }
    predict::EntityRoster &held = row->rosters[p_roster];
    LocalVector<ObjectID> wrappers;
    for (uint32_t at = 0; at < held.members.size(); ++at) {
        if (gd::object_of(held.wrappers[at]) != nullptr) {
            live.push_back(held.members[at]);
            wrappers.push_back(held.wrappers[at]);
        }
    }
    held.members = live;
    held.wrappers = wrappers;
    return live;
}

bool NetwPredictionEngine::roster_add(
    int64_t p_slot,
    int p_roster,
    const RID &p_entity
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr || !roster_index_valid(p_roster)
        || !p_entity.is_valid()) {
        return false;
    }
    return row->rosters[p_roster].add(p_entity, wrapper_id(session, p_entity));
}

bool NetwPredictionEngine::roster_erase(
    int64_t p_slot,
    int p_roster,
    const RID &p_entity
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr || !roster_index_valid(p_roster)
        || !p_entity.is_valid()) {
        return false;
    }
    return row->rosters[p_roster].erase(p_entity);
}

String NetwPredictionEngine::participant_key(const RID &p_entity) const {
    const NetwEntityRecord *record
        = session != nullptr ? session->entity_record(p_entity) : nullptr;
    return record != nullptr
        ? entity_key(record, session->wrapper_owner(p_entity))
        : String();
}

NetwEntityRecord *NetwPredictionEngine::member_record(
    int64_t p_slot,
    const RID &p_member
) const {
    NetwEntityRecord *record
        = session != nullptr ? session->entity_record(p_member) : nullptr;
    if (record != nullptr) {
        return record;
    }
    const NetwEntity *wrapper = roster_wrapper(row_of(p_slot), p_member);
    return wrapper != nullptr ? wrapper->record_in(session) : nullptr;
}

String NetwPredictionEngine::member_key(
    int64_t p_slot,
    const RID &p_member
) const {
    if (session != nullptr && session->entity_record(p_member) != nullptr) {
        return participant_key(p_member);
    }
    const NetwEntity *wrapper = roster_wrapper(row_of(p_slot), p_member);
    return wrapper != nullptr
        ? entity_key(wrapper->record_in(session), wrapper->get_owner())
        : String();
}

void NetwPredictionEngine::roster_clear(int64_t p_slot, int p_roster) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr && roster_index_valid(p_roster)) {
        row->rosters[p_roster].clear();
    }
}

void NetwPredictionEngine::roster_assign(
    int64_t p_slot,
    int p_roster,
    const LocalVector<RID> &p_members
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr || !roster_index_valid(p_roster)) {
        return;
    }
    predict::EntityRoster &held = row->rosters[p_roster];
    held.clear();
    for (const RID &member : p_members) {
        if (member.is_valid()) {
            held.add(member, wrapper_id(session, member));
        }
    }
}

bool NetwPredictionEngine::joint_refusal_reported_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? false : row->latches.joint_refusal_reported;
}

bool NetwPredictionEngine::island_roster_seeded_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? false : row->latches.island_roster_seeded;
}

void NetwPredictionEngine::set_island_roster_seeded(
    int64_t p_slot,
    bool p_value
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->latches.island_roster_seeded = p_value;
    }
}

int64_t NetwPredictionEngine::joint_basis_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? -1 : row->latches.joint_basis;
}

int64_t NetwPredictionEngine::joint_relay_floor_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? -1 : row->latches.joint_relay_floor;
}

int64_t NetwPredictionEngine::joint_epoch_floor_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? -1 : row->latches.joint_epoch_floor;
}

int64_t NetwPredictionEngine::tenure_begin_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? -1 : row->latches.tenure_begin;
}

void NetwPredictionEngine::set_tenure_begin(int64_t p_slot, int64_t p_value) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->latches.tenure_begin = p_value;
    }
}

int64_t NetwPredictionEngine::tenure_end_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? -1 : row->latches.tenure_end;
}

void NetwPredictionEngine::set_tenure_end(int64_t p_slot, int64_t p_value) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->latches.tenure_end = p_value;
    }
}

int NetwPredictionEngine::simulation_subject_count(int64_t p_slot) const {
    NetwMultiplayer *host = session;
    const NetwEntityRecord *seated = owner_record(p_slot);
    if (host == nullptr || seated == nullptr) {
        return 0;
    }
    return int(host->sim_selection_count(seated->get_handle()));
}

void NetwPredictionEngine::set_order_key(int64_t p_slot, int64_t p_order_key) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->order_key = p_order_key;
    }
}

int64_t NetwPredictionEngine::order_key_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? row->order_key : -1;
}

PackedInt64Array NetwPredictionEngine::pass_slots(int p_phase) const {
    LocalVector<int64_t> held;
    fill_pass_slots(p_phase, held);
    PackedInt64Array out;
    for (const int64_t slot : held) {
        out.push_back(slot);
    }
    return out;
}

void NetwPredictionEngine::fill_pass_slots(
    int p_phase,
    LocalVector<int64_t> &r_slots
) const {
    fill_ordered_slots();
    r_slots.clear();
    for (const int64_t slot : order_slots) {
        const predict::Slot *row = row_of(slot);
        if (row != nullptr && steps_in_phase(*row, p_phase)) {
            r_slots.push_back(slot);
        }
    }
}

PackedInt64Array NetwPredictionEngine::ordered_slots() const {
    fill_ordered_slots();
    PackedInt64Array out;
    out.resize(int(order_slots.size()));
    for (uint32_t at = 0; at < order_slots.size(); ++at) {
        out.set(int(at), order_slots[at]);
    }
    return out;
}

void NetwPredictionEngine::fill_ordered_slots() const {
    NETW_ZONE_NC("Predict ordered slots", colors::PREDICTION);
    LocalVector<int64_t> &keys = order_keys;
    LocalVector<int64_t> &slots = order_slots;
    keys.clear();
    slots.clear();
    for (uint32_t seat = 0; seat < rows.size(); ++seat) {
        const predict::Slot *row = rows[seat];
        if (row == nullptr) {
            continue;
        }
        keys.push_back(row->order_key < 0 ? INT64_MAX : row->order_key);
        slots.push_back(ids.id_at(seat));
    }
    for (uint32_t at = 1; at < slots.size(); ++at) {
        const int64_t key = keys[at];
        const int64_t slot = slots[at];
        uint32_t place = at;
        while (place > 0
               && (keys[place - 1] > key
                   || (keys[place - 1] == key && slots[place - 1] > slot))) {
            keys[place] = keys[place - 1];
            slots[place] = slots[place - 1];
            place -= 1;
        }
        keys[place] = key;
        slots[place] = slot;
    }
}

bool NetwPredictionEngine::island_declared(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr && row->config.island_declared;
}

bool NetwPredictionEngine::island_approximate(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr && row->config.island_approximate;
}

int NetwPredictionEngine::island_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? row->config.island : ISLAND_NONE;
}

int64_t NetwPredictionEngine::sibling_slot(
    int64_t p_slot,
    const RID &p_member
) {
    NetwMultiplayer *host = session;
    if (host == nullptr || !p_member.is_valid()) {
        return -1;
    }
    return host->predict_is_declared(p_member)
        ? slot_of(member_record(p_slot, p_member))
        : -1;
}

bool NetwPredictionEngine::admits_rollback_together(int64_t p_slot) {
    const predict::Decl *decl = decl_of(p_slot);
    const NetwEntityRecord *owner = owner_record(p_slot);
    const bool requested = decl != nullptr
        && NetwMultiplayer::predict_rolls_back_together(
            *owner_record(p_slot),
            owner_node(p_slot)
        );
    if (!requested || role_of(p_slot) == int(Role::REMOTE)) {
        return requested;
    }
    if (joint_refusals(p_slot).is_empty()) {
        set_joint_refusal_reported(p_slot, false);
        return true;
    }
    NetwMultiplayer *host = session;
    if (host != nullptr && owner != nullptr && decl->rollback_together
        && role_of(p_slot) == int(Role::PREDICT)
        && !joint_refusal_reported_of(p_slot)) {
        const int64_t id = int64_t(owner->get_handle().get_id());
        host->session_defer(
            callable_mp(host, &NetwMultiplayer::predict_report_joint_refusal)
                .bind(id),
            StringName(vformat("predict-joint-refusal?%d", id))
        );
    }
    return false;
}

PackedStringArray NetwPredictionEngine::joint_refusals(int64_t p_slot) {
    NetwPrediction *handle = Object::cast_to<NetwPrediction>(handle_of(p_slot));
    PackedStringArray refusals;
    if (!slot_is_steppable(p_slot)) {
        refusals.push_back(
            refusal_row(*owner_record(p_slot), owner_node(p_slot), handle)
        );
    }
    for (const RID &member : roster_list(p_slot, ROSTER_SIMULATED)) {
        const int64_t seat = sibling_slot(p_slot, member);
        if (seat < 0 || slot_is_steppable(seat)) {
            continue;
        }
        refusals.push_back(refusal_row(
            *owner_record(seat),
            owner_node(seat),
            Object::cast_to<NetwPrediction>(handle_of(seat))
        ));
    }
    return refusals;
}

void NetwPredictionEngine::report_joint_refusal(int64_t p_slot) {
    const predict::Decl *decl = decl_of(p_slot);
    if (decl == nullptr || !decl->rollback_together
        || role_of(p_slot) != int(Role::PREDICT)
        || joint_refusal_reported_of(p_slot)) {
        return;
    }
    const PackedStringArray refusals = joint_refusals(p_slot);
    if (refusals.is_empty()) {
        return;
    }
    set_joint_refusal_reported(p_slot, true);
    Node *seated = owner_node(p_slot);
    fault::raise(
        session,
        NETW_FAULT_TOGETHER_REFUSED,
        {subject_name(seated), String(), String(", ").join(refusals)},
        subject_of(seated)
    );
}

void NetwPredictionEngine::follow_relay_subscription(
    int64_t p_slot,
    bool p_selected
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr || row->relay_subscribed == p_selected) {
        return;
    }
    row->relay_subscribed = p_selected;
    NetwMultiplayer *host = session;
    const NetwEntityRecord *seated = owner_record(p_slot);
    if (host == nullptr || seated == nullptr) {
        return;
    }
    host->predict_relay_subscribe(seated->get_handle(), p_selected);
}

void NetwPredictionEngine::resend_relay_subscription(int64_t p_slot) {
    const predict::Slot *row = row_of(p_slot);
    NetwMultiplayer *host = session;
    const NetwEntityRecord *seated = owner_record(p_slot);
    if (row == nullptr || !row->relay_subscribed || host == nullptr
        || seated == nullptr) {
        return;
    }
    host->predict_relay_subscribe(seated->get_handle(), true);
}

void NetwPredictionEngine::admit_rollback_together(int64_t p_slot) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return;
    }
    row->rollback_together_own = admits_rollback_together(p_slot);
    const bool joined = row->rollback_together_own;
    reconfigure_from(
        p_slot,
        Ref<NetwPrediction>(Object::cast_to<NetwPrediction>(handle_of(p_slot)))
    );
    for (const RID &member : roster_list(p_slot, ROSTER_SIMULATED)) {
        const int64_t seat = sibling_slot(p_slot, member);
        predict::Slot *held = mutable_row_of(seat);
        if (held == nullptr || held->rollback_together_joined == joined) {
            continue;
        }
        held->rollback_together_joined = joined;
        reconfigure_from(
            seat,
            Ref<NetwPrediction>(
                Object::cast_to<NetwPrediction>(handle_of(seat))
            )
        );
    }
}

bool NetwPredictionEngine::rolls_back_together(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr
        && (row->rollback_together_joined || row->rollback_together_own);
}

void NetwPredictionEngine::leave_joint(int64_t p_slot) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr || !row->rollback_together_joined) {
        return;
    }
    row->rollback_together_joined = false;
    reconfigure_from(
        p_slot,
        Ref<NetwPrediction>(Object::cast_to<NetwPrediction>(handle_of(p_slot)))
    );
}

void NetwPredictionEngine::note_joint_basis(
    int64_t p_slot,
    int64_t p_basis,
    const predict::StateRow &p_state,
    int p_source
) {
    NetwPrediction *handle = Object::cast_to<NetwPrediction>(handle_of(p_slot));
    if (handle == nullptr) {
        return;
    }
    set_joint_basis(p_slot, MAX(joint_basis_of(p_slot), p_basis));
    if (NetwMultiplayer::predict_schedule_of(
            *owner_record(p_slot),
            owner_node(p_slot)
        )
        == NetwSimulation::SCHEDULE_TICK) {
        if (predict::Slot *row = mutable_row_of(p_slot)) {
            row->record_state_row(p_basis + 1, p_state);
        }
    }
    if (p_basis >= 0 && island_of(p_slot) != ISLAND_NONE) {
        joint_admit_state(p_slot, p_basis, p_state);
        joint_note_basis(p_slot, p_basis, p_source);
    }
}

void NetwPredictionEngine::joint_note_world_tick(
    int64_t p_slot,
    int64_t p_world_tick,
    int64_t p_transition
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr || p_world_tick < 0 || p_transition < 0) {
        return;
    }
    row->joint.world_offset = p_world_tick - (p_transition + 1);
}

int64_t NetwPredictionEngine::joint_world_offset_of(int64_t p_member) const {
    const predict::Slot *member = row_of(p_member);
    if (member == nullptr || member->joint.group < 0) {
        return 0;
    }
    const predict::Slot *group = row_of(member->joint.group);
    return group != nullptr ? group->joint.world_offset : 0;
}

void NetwPredictionEngine::joint_admit_state(
    int64_t p_slot,
    int64_t p_transition,
    const predict::StateRow &p_state
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return;
    }
    const int count = row->wiring.count();
    predict::StateRow arrived;
    if (!row->joint_state(p_transition, arrived)
        || int(arrived.values.size()) != count) {
        arrived.resize(count);
    }
    const int span = std::min(count, int(p_state.values.size()));
    for (int at = 0; at < span; ++at) {
        if (p_state.has(at)) {
            arrived.set(at, p_state.values[uint32_t(at)]);
        }
    }
    if (arrived.any()) {
        row->record_state_row(p_transition + 1, arrived);
    }
}

void NetwPredictionEngine::admit_simulated_state(
    int64_t p_slot,
    const predict::Arrival &p_arrival
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return;
    }
    LocalVector<StringName> &keys = row->recovering.install_keys;
    LocalVector<Variant> &values = row->recovering.install_values;
    keys.clear();
    values.clear();
    const store::Table *table = p_arrival.table;
    if (table != nullptr && p_arrival.row < table->row_count()) {
        const int columns = table->schema.column_count();
        for (int column = 0; column < columns; ++column) {
            keys.push_back(table->schema.at(column)->key);
            values.push_back(
                store::read_value(*table, p_arrival.row, uint32_t(column))
            );
        }
    }
    admit_simulated_rows(p_slot, p_arrival.tick, p_arrival.whole);
}

void NetwPredictionEngine::admit_simulated_rows(
    int64_t p_slot,
    int64_t p_recv_tick,
    bool p_whole
) {
    NetwPrediction *handle = Object::cast_to<NetwPrediction>(handle_of(p_slot));
    predict::Decl *decl = decl_of(p_slot);
    NetwMultiplayer *host = session;
    const NetwEntityRecord *seated = owner_record(p_slot);
    const PropertyBindingPtr binding = state_binding_of(p_slot);
    predict::Slot *row = mutable_row_of(p_slot);
    if (handle == nullptr || decl == nullptr || host == nullptr
        || seated == nullptr
        || binding == nullptr || row == nullptr) {
        return;
    }
    const RID entity = seated->get_handle();
    if (!p_whole && !stream_reconstructed_of(p_slot)) {
        return;
    }
    set_stream_reconstructed(p_slot, true);
    row = mutable_row_of(p_slot);
    if (row == nullptr || row->recovering.install_keys.is_empty()) {
        return;
    }
    const bool joint = rolls_back_together(p_slot);
    const int64_t recv_tick
        = joint ? p_recv_tick - joint_world_offset_of(p_slot) : p_recv_tick;
    if (!joint && host->sim_fenced(entity, recv_tick)) {
        return;
    }
    predict::RecoveryRows &installing = row->recovering;
    const LocalVector<StringName> &keys = installing.install_keys;
    predict::StateRow arrived;
    if (joint) {
        arrived.resize(row->wiring.count());
        for (uint32_t at = 0; at < keys.size(); ++at) {
            const int field = row->wiring.fields.index_of(keys[at]);
            if (field >= 0) {
                arrived.set(field, installing.install_values[at]);
            }
        }
        double agreed = 0.0;
        if (predicted_agrees(*row, recv_tick, arrived, agreed)) {
            announce_evaluation(handle, p_slot, recv_tick, -1, agreed, false);
            return;
        }
    }
    int64_t age = -1;
    host->sim_install_target_row(
        binding,
        recv_tick,
        keys,
        installing.install_values,
        row->config.restore == int(RestoreMode::EXTRAPOLATED)
            ? sim::Restore::EXTRAPOLATED
            : sim::Restore::EXACT,
        int64_t(row->config.max_restore_ticks),
        installing.install_target,
        age
    );
    capture_canonical(p_slot, installing.live);
    row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return;
    }
    const LocalVector<Variant> &target = installing.install_target;
    row->report.divergence.resize(row->wiring.count());
    double divergence = 0.0;
    for (uint32_t at = 0; at < keys.size(); ++at) {
        const int field = row->wiring.fields.index_of(keys[at]);
        double error = std::numeric_limits<double>::infinity();
        if (field >= 0 && installing.live.has(field)) {
            error = predict::value_error(
                installing.live.values[uint32_t(field)],
                target[at],
                row->wiring.angle[uint32_t(field)] != 0
            );
        }
        row->report.divergence.note(field, error);
        divergence = std::max(divergence, error);
    }
    note_reconciling(p_slot, true);
    decl->facts.set_int_fact(
        NetwPredictStats::FACT_INSTALLS,
        decl->facts.get_int_fact(NetwPredictStats::FACT_INSTALLS) + 1
    );
    if (joint) {
        note_joint_basis(p_slot, recv_tick - 1, arrived, JOINT_FLOOR_STATE);
    } else if (
        host->sim_write_install_row(
            entity,
            binding,
            keys,
            target,
            age,
            installing.moved_keys,
            installing.moved
        )
    ) {
        if (predict::Slot *held = mutable_row_of(p_slot)) {
            colliders::teleport(held->port.resolve(sys::PREDICTION));
        }
    }
    note_reconciling(p_slot, false);
    note_verdict_reason(p_slot, predict::VERDICT_REASON_NONE);
    announce_evaluation(handle, p_slot, recv_tick, -1, divergence, true);
}

LocalVector<RID> NetwPredictionEngine::live_participants(int64_t p_slot) {
    LocalVector<RID> seated = roster_list(p_slot, ROSTER_ISLAND_MEMBERS);
    NetwMultiplayer *host = session;
    if (!seated.is_empty() || host == nullptr
        || owner_record(p_slot) == nullptr) {
        return seated;
    }
    return host->sim_joiners(owner_rid(p_slot));
}

PackedStringArray NetwPredictionEngine::live_participant_ids(int64_t p_slot) {
    NETW_ZONE_LEAF(
        "Predict live participant ids",
        colors::PREDICTION,
        profile::SUBSYSTEM_PREDICTION
    );
    const LocalVector<RID> seated = live_participants(p_slot);
    PackedStringArray ids;
    ids.resize(seated.size());
    String *names = ids.ptrw();
    for (uint32_t at = 0; at < seated.size(); ++at) {
        names[at] = member_key(p_slot, seated[at]);
    }
    return ids;
}

void NetwPredictionEngine::open_tenure(
    int64_t p_slot,
    const RID &p_member,
    int64_t p_transition
) {
    roster_erase(p_slot, ROSTER_JOINT_LINGERING, p_member);
    predict::Slot *seat
        = mutable_row_of(slot_of(member_record(p_slot, p_member)));
    if (seat != nullptr) {
        seat->latches.tenure_begin = p_transition;
        seat->latches.tenure_end = -1;
    }
}

void NetwPredictionEngine::close_tenure(
    int64_t p_slot,
    const RID &p_member,
    int64_t p_transition
) {
    predict::Slot *seat
        = mutable_row_of(slot_of(member_record(p_slot, p_member)));
    if (seat == nullptr) {
        return;
    }
    seat->latches.tenure_end = p_transition;
    roster_add(p_slot, ROSTER_JOINT_LINGERING, p_member);
}

void NetwPredictionEngine::publish_island_roster(int64_t p_slot) {
    predict::Decl *decl = decl_of(p_slot);
    if (decl == nullptr) {
        return;
    }
    decl->facts.set_names_fact(
        NetwPredictStats::FACT_ISLAND_MEMBERS,
        sorted_ids(*this, p_slot, roster_list(p_slot, ROSTER_ISLAND_MEMBERS))
    );
    decl->facts.set_names_fact(
        NetwPredictStats::FACT_SIMULATED_MEMBERS,
        sorted_ids(*this, p_slot, roster_list(p_slot, ROSTER_SIMULATED))
    );
}

void NetwPredictionEngine::refresh_selection(int64_t p_slot, bool p_joint) {
    NetwMultiplayer *host = session;
    const RID subject = owner_rid(p_slot);
    if (host == nullptr || !host->sim_chooses(subject)) {
        clear_island_promotions(p_slot);
        roster_clear(p_slot, ROSTER_ISLAND_MEMBERS);
        publish_topology_roster(p_slot);
        if (island_declared(p_slot)) {
            admit_rollback_together(p_slot);
        }
        return;
    }
    const LocalVector<RID> members = host->sim_candidates(subject);
    LocalVector<RID> admitted;
    for (const RID &member : members) {
        const int64_t seat = slot_of(member);
        if (seat < 0 || joint_admits(p_slot, seat)) {
            admitted.push_back(member);
        }
    }
    const LocalVector<RID> promoted = host->sim_select(
        subject,
        admitted,
        drive_frontier(p_slot)
    );
    if (p_joint) {
        const sim::Row *standing = sim_rows->row_of(subject);
        sim::Selection joint;
        if (standing != nullptr) {
            joint.owner_order_key = standing->selection.owner_order_key;
            for (const sim::Selected &member : standing->selection.members) {
                const RID entity = host->sim_entity_of_key(member.key);
                const int64_t seat = host->entity_get_view(entity).is_valid()
                    ? slot_of(entity)
                    : -1;
                if (seat < 0) {
                    continue;
                }
                sim::Selected held = member;
                held.key = seat;
                joint.members.push_back(held);
            }
        }
        joint_adopt(p_slot, joint);
        LocalVector<RID> joined;
        for (const RID &member : promoted) {
            if (slot_of(member) >= 0) {
                joined.push_back(member);
            }
        }
        if (apply_island_promotions(p_slot, joined)) {
            publish_island_roster(p_slot);
        }
    } else {
        clear_joint_roster(p_slot);
    }

    const bool changed
        = !same_roster(members, roster_list(p_slot, ROSTER_ISLAND_MEMBERS));
    if (island_roster_seeded_of(p_slot) && changed) {
        const predict::Decl *decl = decl_of(p_slot);
        open_out_of_domain_window(
            p_slot,
            latest_input_tick_of(p_slot),
            decl != nullptr ? decl->correction_cooldown_ticks : 0
        );
    }
    set_island_roster_seeded(p_slot, true);
    roster_assign(p_slot, ROSTER_ISLAND_MEMBERS, members);
    publish_topology_roster(p_slot);
    admit_rollback_together(p_slot);
    if (changed) {
        publish_island_roster(p_slot);
    }
}

void NetwPredictionEngine::clear_island_promotions(int64_t p_slot) {
    clear_joint_roster(p_slot);
    NetwMultiplayer *host = session;
    const NetwEntityRecord *owner = owner_record(p_slot);
    if (host != nullptr && owner != nullptr) {
        host->sim_drop_selections(owner->get_handle());
    }
}

void NetwPredictionEngine::clear_joint_roster(int64_t p_slot) {
    const int64_t frontier = drive_frontier(p_slot);
    for (const RID &member : roster_list(p_slot, ROSTER_SIMULATED)) {
        close_tenure(p_slot, member, frontier);
    }
    roster_clear(p_slot, ROSTER_SIMULATED);
    if (predict::Slot *row = mutable_row_of(p_slot)) {
        row->joint_roster.members.clear();
    }
}

bool NetwPredictionEngine::apply_island_promotions(
    int64_t p_slot,
    const LocalVector<RID> &p_promoted
) {
    const int64_t frontier = drive_frontier(p_slot);
    const LocalVector<RID> standing = roster_list(p_slot, ROSTER_SIMULATED);
    for (const RID &member : standing) {
        if (!p_promoted.has(member)) {
            close_tenure(p_slot, member, frontier);
        }
    }
    for (const RID &member : p_promoted) {
        if (!standing.has(member)) {
            open_tenure(p_slot, member, frontier + 1);
        }
    }
    bool changed = p_promoted.size() != standing.size();
    for (uint32_t at = 0; !changed && at < p_promoted.size(); ++at) {
        changed = !standing.has(p_promoted[at]);
    }
    roster_assign(p_slot, ROSTER_SIMULATED, p_promoted);
    return changed;
}

int NetwPredictionEngine::release_lingering(
    int64_t p_slot,
    int64_t p_floor_transition
) {
    int held = 0;
    for (const RID &member : roster_list(p_slot, ROSTER_JOINT_LINGERING)) {
        const predict::Slot *seat
            = row_of(slot_of(member_record(p_slot, member)));
        if (seat == nullptr || seat->latches.tenure_end < p_floor_transition) {
            roster_erase(p_slot, ROSTER_JOINT_LINGERING, member);
            continue;
        }
        held += 1;
    }
    return held;
}

void NetwPredictionEngine::publish_topology_roster(int64_t p_slot) {
    set_island_participants(p_slot, live_participant_ids(p_slot));
}

bool NetwPredictionEngine::contact_is_equivalent(
    int64_t p_slot,
    bool p_has_witness
) {
    const LocalVector<RID> seated = live_participants(p_slot);
    if (seated.is_empty()) {
        return false;
    }
    const LocalVector<RID> touched = p_has_witness
        ? roster_list(p_slot, ROSTER_REALIZED_CONTACT)
        : live_participants(p_slot);
    for (const RID &participant : touched) {
        if (!seated.has(participant)) {
            return false;
        }
        const NetwEntityRecord *record = member_record(p_slot, participant);
        const predict::Decl *decl
            = record != nullptr ? record->find_predict_declaration() : nullptr;
        if (decl == nullptr
            || decl->sim_mode == NetwPrediction::SIM_MODE_DISPLAY) {
            return false;
        }
    }
    return true;
}

void NetwPredictionEngine::notify_collision(
    int64_t p_slot,
    bool p_has_witness,
    int p_cooldown_ticks
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr || row->config.role != int(Role::PREDICT)) {
        return;
    }
    const int64_t authored = row->cursor.latest_input_tick;
    set_cooldown_until_tick(p_slot, authored + p_cooldown_ticks);
    if (!row->latches.island_gap_reported
        && live_participants(p_slot).is_empty()) {
        row->latches.island_gap_reported = true;
        Node *seated = owner_node(p_slot);
        fault::raise(
            session,
            NETW_FAULT_COLLISION_UNSIMULATED,
            {subject_name(seated)},
            subject_of(seated)
        );
    }
    if (!contact_is_equivalent(p_slot, p_has_witness)) {
        open_out_of_domain_window(p_slot, authored, p_cooldown_ticks);
    }
}

void NetwPredictionEngine::joint_adopt(
    int64_t p_slot,
    const sim::Selection &p_selection
) {
    predict::Slot *owner = mutable_row_of(p_slot);
    if (owner == nullptr) {
        return;
    }
    owner->joint_roster = p_selection;
    for (const sim::Selected &selected : owner->joint_roster.members) {
        if (predict::Slot *member = mutable_row_of(selected.key)) {
            if (selected.promoted
                && member->joint.tenure.begin != selected.tenure.begin) {
                member->clear_joint_cells();
            }
            member->joint.tenure = selected.tenure;
            member->joint.group = p_slot;
        }
    }
    NETW_TRACE(
        sys::PREDICTION,
        "Group %d took %d members (%d promoted, %d lingering).",
        p_slot,
        owner->joint_roster.present_count(),
        owner->joint_roster.promoted_count(),
        owner->joint_roster.lingering_count()
    );
}

bool NetwPredictionEngine::joint_admits(
    int64_t p_slot,
    int64_t p_member
) const {
    const predict::Slot *owner = row_of(p_slot);
    const predict::Slot *member = row_of(p_member);
    if (owner == nullptr || member == nullptr || p_member == p_slot) {
        return false;
    }
    return owner->config.island != ISLAND_JOINT
        || rerunnable(member->config.schedule);
}

int NetwPredictionEngine::joint_member_count(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? row->joint_roster.present_count() : 0;
}

bool NetwPredictionEngine::joint_promoted(
    int64_t p_slot,
    int64_t p_member
) const {
    const predict::Slot *row = row_of(p_slot);
    const sim::Selected *member
        = row != nullptr ? row->joint_roster.member(p_member) : nullptr;
    return member != nullptr && member->promoted;
}

int64_t NetwPredictionEngine::tenure_begin(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? row->joint.tenure.begin : -1;
}

int64_t NetwPredictionEngine::tenure_end(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? row->joint.tenure.end : -1;
}

void NetwPredictionEngine::joint_record(
    int64_t p_slot,
    int64_t p_transition,
    const predict::StateRow &p_state,
    const predict::StateRow &p_command,
    bool p_authored,
    bool p_relayed,
    bool p_predictor_valid
) {
    NETW_ZONE_LEAF(
        "Predict joint record",
        colors::PREDICTION,
        profile::SUBSYSTEM_PREDICTION
    );
    predict::Slot *row = mutable_row_of(p_slot);
    NETW_BUG_COND(row == nullptr);
    NETW_BUG_COND(p_transition < 0);
    if (p_state.any()) {
        row->record_state_row(p_transition + 1, p_state);
    }
    row->record_joint_command(
        p_transition,
        p_command,
        predict::joint_cell(p_authored, p_relayed, p_predictor_valid)
    );
}

void NetwPredictionEngine::joint_note_basis(
    int64_t p_slot,
    int64_t p_basis,
    int p_source
) {
    predict::Slot *row = mutable_row_of(p_slot);
    NETW_BUG_COND(row == nullptr || row->config.island == ISLAND_NONE);
    NETW_BUG_COND(
        p_basis < 0 || p_source < JOINT_FLOOR_ACK
        || p_source > JOINT_FLOOR_EPOCH
    );
    predict::JointTrack &track = row->joint;
    predict::JointStats &stats = row->joint_stats;
    if (p_source == JOINT_FLOOR_RELAY) {
        if (track.relay_floor < 0 || p_basis < track.relay_floor) {
            track.relay_floor = p_basis;
            stats.floor_relay_moves += 1;
        }
    } else if (p_source == JOINT_FLOOR_EPOCH) {
        if (p_basis > track.epoch_floor) {
            track.epoch_floor = p_basis;
            stats.floor_epoch_moves += 1;
        }
    } else if (p_basis > track.basis) {
        track.basis = p_basis;
        if (p_source == JOINT_FLOOR_ACK) {
            stats.floor_ack_moves += 1;
        } else {
            stats.floor_state_moves += 1;
        }
    }
}

void NetwPredictionEngine::joint_clear(int64_t p_slot) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->clear_joint_cells();
    }
}

int NetwPredictionEngine::joint_provenance_at(
    int64_t p_slot,
    int64_t p_transition
) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? row->inputs.origin_of(p_transition) : -1;
}

predict::ReplayPlan NetwPredictionEngine::joint_pass(
    int64_t p_slot,
    int64_t p_present
) {
    NETW_ZONE_NC("Predict joint group pass", colors::PREDICTION);
    predict::ReplayPlan plan;
    plan.flags = predict::REPLAY_REBASES | predict::REPLAY_SETTLES
        | predict::REPLAY_SAMPLES_EACH_CLOSE | predict::REPLAY_RUNS_THROUGH_SEAT
        | predict::REPLAY_DEMOTES;
    predict::Slot *owner = mutable_row_of(p_slot);
    NETW_BUG_COND_V(
        owner == nullptr || owner->config.island != ISLAND_JOINT
            || !rerunnable(owner->config.schedule),
        plan
    );
    NETW_BUG_COND_V(p_present < 0, plan);

    struct PassMember {
        predict::Slot *row = nullptr;
        int64_t slot = 0;
        int64_t order_key = 0;
    };
    LocalVector<PassMember> members;
    PassMember subject;
    subject.row = owner;
    subject.slot = p_slot;
    subject.order_key = owner->joint_roster.owner_order_key;
    members.push_back(subject);
    for (const sim::Selected &selected : owner->joint_roster.members) {
        if (!selected.promoted && !selected.lingering) {
            continue;
        }
        predict::Slot *member = mutable_row_of(selected.key);
        if (member == nullptr) {
            NETW_DEBUG(
                sys::PREDICTION,
                "Group member %d closed before its group let it go.",
                selected.key
            );
            continue;
        }
        PassMember pass_member;
        pass_member.row = member;
        pass_member.slot = selected.key;
        pass_member.order_key = selected.order_key;
        members.push_back(pass_member);
    }
    struct ByOrder {
        bool operator()(
            const PassMember &p_left,
            const PassMember &p_right
        ) const {
            return p_left.order_key < p_right.order_key;
        }
    };
    members.sort_custom<ByOrder>();

    bool moved = false;
    int64_t epoch_floor = -1;
    int64_t history_floor = p_present - predict::ACK_AGE_MAX;
    LocalVector<int64_t> bases;
    LocalVector<int64_t> relay_floors;
    for (uint32_t at = 0; at < members.size(); ++at) {
        const predict::JointTrack &track = members[at].row->joint;
        moved = moved || track.moved();
        bases.push_back(track.basis);
        relay_floors.push_back(track.relay_floor);
        epoch_floor = std::max(epoch_floor, track.epoch_floor);
        history_floor = std::max(
            history_floor,
            track.history_floor(
                members[at].row->joint_oldest_state(),
                members[at].row->inputs.oldest_key()
            )
        );
    }
    if (!moved) {
        return plan;
    }

    predict::JointFloorDecision decision = predict::joint_floor(
        bases,
        relay_floors,
        epoch_floor,
        history_floor,
        p_present
    );
    const auto release_expired = [&](int64_t p_floor) {
        owner->joint_roster.release_lingering(p_floor);
        plan.floor = p_floor;
        for (uint32_t at = 0; at < members.size();) {
            const bool expired = members[at].slot != p_slot
                && members[at].row->joint.tenure.end >= 0
                && members[at].row->joint.tenure.end < p_floor;
            if (expired) {
                members[at].row->joint.clear_floors();
                members.remove_at(at);
            } else {
                at += 1;
            }
        }
    };
    release_expired(decision.floor);
    predict::StateRow held;
    if (!decision.heal) {
        for (uint32_t at = 0; at < members.size(); ++at) {
            if (!members[at].row->joint_state(decision.floor, held)) {
                decision.floor = p_present;
                decision.heal = true;
                break;
            }
        }
    }
    if (decision.heal) {
        release_expired(decision.floor);
    }

    plan.floor = decision.floor;
    plan.present = p_present;
    plan.heal = decision.heal;
    LocalVector<int> depths;
    depths.resize(members.size());
    for (uint32_t at = 0; at < depths.size(); ++at) {
        depths[at] = 0;
    }
    for (uint32_t at = 0; at < members.size(); ++at) {
        const predict::Slot &member = *members[at].row;
        const int64_t source = decision.heal
            ? member.joint_newest_state(p_present)
            : decision.floor;
        const bool found = source >= 0 && member.joint_state(source, held);
        const predict::StateRow *state = found ? &held : nullptr;
        if (state == nullptr) {
            NETW_DEBUG(
                sys::PREDICTION,
                "Group replay %d stopped, because member %d has no saved "
                "state at that tick.",
                p_slot,
                members[at].slot
            );
            for (uint32_t clear_at = 0; clear_at < members.size(); ++clear_at) {
                members[clear_at].row->joint.clear_floors();
            }
            return plan;
        }
        predict::JointRestore restore;
        restore.slot = members[at].slot;
        restore.state = *state;
        restore.entry = source + 1;
        plan.restores.push_back(restore);
        members[at].row->set_state(*state);
    }
    NETW_ASSERT(plan.restores.size() == members.size());

    predict::JointStats &stats = owner->joint_stats;
    if (!decision.heal) {
        for (int64_t transition = decision.floor + 1; transition <= p_present;
             ++transition) {
            for (uint32_t at = 0; at < members.size(); ++at) {
                const predict::JointTrack &track = members[at].row->joint;
                if (!track.tenure.contains(transition)) {
                    continue;
                }
                predict::Slot &member = *members[at].row;
                const int origin = member.inputs.origin_of(transition);
                predict::ReplayStep step;
                step.slot = members[at].slot;
                step.tick = transition;
                step.entry = transition;
                step.flags = predict::STEP_CLOSES_ENTRY;
                if (origin >= 0) {
                    member.inputs
                        .read_row(member.input_codec, transition, step.command);
                    step.provenance = predict::CellProvenance(origin);
                }
                if (origin == int(predict::CellProvenance::SUBSTITUTED)
                    && !predicts_input(step.slot)
                    && relayed_guess(step.slot, transition, step.command)) {
                    member.inputs.record_row(
                        member.input_codec,
                        transition,
                        transition,
                        true,
                        step.command,
                        uint8_t(predict::CellProvenance::SUBSTITUTED),
                        session
                    );
                }
                plan.steps.push_back(step);
                depths[at] += 1;
                stats.cells_relayed
                    += step.provenance == predict::CellProvenance::RELAYED ? 1
                                                                           : 0;
                stats.cells_substituted
                    += step.provenance == predict::CellProvenance::SUBSTITUTED
                    ? 1
                    : 0;
            }
        }
        stats.passes += 1;
    } else {
        stats.heal_snaps += 1;
    }
    for (uint32_t at = 0; at < depths.size(); ++at) {
        stats.max_depth = std::max(stats.max_depth, depths[at]);
    }
    stats.members = int(plan.restores.size());
    stats.floor = plan.floor;
    stats.present = plan.present;
    plan.valid = true;

    stats.linger_held = owner->joint_roster.lingering_count();
    for (uint32_t at = 0; at < members.size(); ++at) {
        members[at].row->joint.clear_floors();
    }
    NETW_TRACE(
        sys::PREDICTION,
        "Group %d replayed from tick %d with %d of %d members present (heal "
        "%d).",
        p_slot,
        plan.floor,
        plan.present,
        stats.members,
        plan.heal ? 1 : 0
    );
    return plan;
}

PackedInt64Array NetwPredictionEngine::joint_stats(int64_t p_slot) const {
    PackedInt64Array out = gd::zeroed<PackedInt64Array>(STAT_JOINT_COUNT);
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr) {
        return out;
    }
    const predict::JointStats &stats = row->joint_stats;
    int64_t *values = out.ptrw();
    values[STAT_JOINT_PASSES] = stats.passes;
    values[STAT_JOINT_MEMBERS] = stats.members;
    values[STAT_JOINT_CELLS_RELAYED] = stats.cells_relayed;
    values[STAT_JOINT_CELLS_SUBSTITUTED] = stats.cells_substituted;
    values[STAT_JOINT_HEAL_SNAPS] = stats.heal_snaps;
    values[STAT_JOINT_LINGER_HELD] = stats.linger_held;
    values[STAT_JOINT_FLOOR] = stats.floor;
    values[STAT_JOINT_PRESENT] = stats.present;
    values[STAT_JOINT_MAX_DEPTH] = stats.max_depth;
    values[STAT_JOINT_FLOOR_ACK_MOVES] = stats.floor_ack_moves;
    values[STAT_JOINT_FLOOR_STATE_MOVES] = stats.floor_state_moves;
    values[STAT_JOINT_FLOOR_RELAY_MOVES] = stats.floor_relay_moves;
    values[STAT_JOINT_FLOOR_EPOCH_MOVES] = stats.floor_epoch_moves;
    return out;
}

#define NETW_PREDICT_JOURNAL_READ(m_name, m_call, m_type, m_absent) \
    m_type NetwPredictionEngine::m_name(int64_t p_slot, int p_index) const { \
        const predict::Slot *row = row_of(p_slot); \
        return row != nullptr ? m_type(row->journal.m_call(p_index)) \
                              : m_absent; \
    }

NETW_PREDICT_JOURNAL_READ(journal_transition_at, transition_at, int64_t, -1)
NETW_PREDICT_JOURNAL_READ(journal_label_at, label_at, int64_t, -1)
NETW_PREDICT_JOURNAL_READ(journal_kind_at, kind_at, int, 0)
NETW_PREDICT_JOURNAL_READ(journal_c_hash_at, c_hash_at, int64_t, 0)
NETW_PREDICT_JOURNAL_READ(journal_pre_fp_at, pre_fp_at, int64_t, 0)
NETW_PREDICT_JOURNAL_READ(journal_post_fp_at, post_fp_at, int64_t, 0)
NETW_PREDICT_JOURNAL_READ(journal_environment_at, e_digest_at, int64_t, 0)
NETW_PREDICT_JOURNAL_READ(journal_topology_at, topo_fp_at, int64_t, 0)
NETW_PREDICT_JOURNAL_READ(journal_raw_at, raw_fp_at, int64_t, 0)
NETW_PREDICT_JOURNAL_READ(journal_witness_at, witness_fp_at, int64_t, 0)
NETW_PREDICT_JOURNAL_READ(
    journal_witness_class_at,
    witness_class_bits_at,
    int,
    0
)
NETW_PREDICT_JOURNAL_READ(journal_episode_id_at, episode_id_at, int, 0)
NETW_PREDICT_JOURNAL_READ(journal_write_id_at, write_id_at, int, 0)
NETW_PREDICT_JOURNAL_READ(journal_operator_at, operator_at, int, 0)
NETW_PREDICT_JOURNAL_READ(journal_basis_at, basis_at, int64_t, -1)
NETW_PREDICT_JOURNAL_READ(journal_evidence_at, evidence_mask_at, int, 0)
NETW_PREDICT_JOURNAL_READ(journal_flags_at, flags_at, int, 0)
NETW_PREDICT_JOURNAL_READ(journal_domain_at, domain_at, int, 0)
NETW_PREDICT_JOURNAL_READ(journal_attribution_at, attribution_at, int, 0)
NETW_PREDICT_JOURNAL_READ(
    journal_differing_family_at,
    differing_family_at,
    int,
    0
)
NETW_PREDICT_JOURNAL_READ(
    journal_aligned_error_at,
    aligned_error_at,
    double,
    0.0
)

#undef NETW_PREDICT_JOURNAL_READ

bool NetwPredictionEngine::announce_joint_moved(
    int64_t p_slot,
    int64_t p_transition,
    const predict::StateRow &p_before
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr
        || !write_deltas_rows(
            p_slot,
            p_before,
            predict::StateRow(),
            row->recovering.deltas
        )) {
        return false;
    }
    emit_recovered(
        p_slot,
        p_transition,
        int64_t(predict::Attribution::UNKNOWN),
        false
    );
    return true;
}

void NetwPredictionEngine::set_island_participants(
    int64_t p_slot,
    const PackedStringArray &p_names
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return;
    }
    PackedStringArray sorted = p_names.duplicate();
    sorted.sort();
    row->island_participants = sorted;
}

PackedStringArray NetwPredictionEngine::island_participants(
    int64_t p_slot
) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? row->island_participants : PackedStringArray();
}

Dictionary NetwPredictionEngine::topology_facts(
    int64_t p_slot,
    int p_schedule,
    int64_t p_epoch
) const {
    Dictionary out;
    out[StringName("schedule")] = p_schedule;
    out[StringName("epoch")] = p_epoch;
    out[StringName("participants")] = island_participants(p_slot);
    return out;
}

bool NetwPredictionEngine::has_pose_fields(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr) {
        return false;
    }
    for (int at = 0; at < row->wiring.count(); ++at) {
        if (row->wiring.pose[uint32_t(at)] != 0) {
            return true;
        }
    }
    return false;
}

} // namespace netw
