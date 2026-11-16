#include "netw/api/netw_multiplayer.hpp"

#include <algorithm>

#include "godot/node.hpp"
#include "godot/object.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/entity_record.hpp"
#include "netw/api/simulation.hpp"
#include "netw/colors.hpp"
#include "netw/log.hpp"
#include "netw/profile.hpp"
#include "netw/sim/body.hpp"
#include "netw/sim/row.hpp"
#include "netw/sim/select.hpp"
#include "netw/subsystems.hpp"
#include "netw/sync_authoring.hpp"

using namespace godot;

namespace netw {

namespace {

String order_name(const Ref<NetwEntity> &p_entity) {
    const String named = String(p_entity->get_name());
    if (!named.is_empty()) {
        return named;
    }
    const int64_t route = p_entity->get_route();
    if (route > 0) {
        return String("route:") + String::num_int64(route);
    }
    Node *root = p_entity->get_owner();
    if (root != nullptr) {
        return String("path:") + String(root->get_path());
    }
    return String();
}

bool order_less(const Ref<NetwEntity> &p_a, const Ref<NetwEntity> &p_b) {
    const String a_id = order_name(p_a);
    const String b_id = order_name(p_b);
    if (a_id == b_id) {
        return p_a->get_instance_id() < p_b->get_instance_id();
    }
    return a_id < b_id;
}

bool same_scene(
    const NetwMultiplayer &p_core,
    const Ref<NetwEntity> &p_subject,
    const Ref<NetwEntity> &p_member
) {
    return p_core.scene_of(p_subject->get_rid_handle())
        == p_core.scene_of(p_member->get_rid_handle());
}

bool member_of(
    const NetwMultiplayer &p_core,
    const Ref<NetwEntity> &p_subject,
    const Ref<NetwEntity> &p_member
) {
    if (p_member.is_null() || p_member == p_subject
        || p_member->get_owner() == nullptr || p_member->get_declares_scene()) {
        return false;
    }
    return same_scene(p_core, p_subject, p_member)
        && p_member->get_multiplayer() == p_subject->get_multiplayer();
}

void take(
    const NetwMultiplayer &p_core,
    const Ref<NetwEntity> &p_subject,
    const Ref<NetwEntity> &p_member,
    LocalVector<Ref<NetwEntity>> &r_found
) {
    if (!member_of(p_core, p_subject, p_member)) {
        return;
    }
    for (const Ref<NetwEntity> &held : r_found) {
        if (held == p_member) {
            return;
        }
    }
    r_found.push_back(p_member);
}

sim::Pick pick_of(const sim::Row *p_member, bool p_eligible) {
    const sim::RemoteMode told = p_member != nullptr
        ? p_member->declaration.remote_mode
        : sim::RemoteMode::AUTO;
    if (told == sim::RemoteMode::PROXY) {
        return sim::Pick::EXCLUDED;
    }
    return told == sim::RemoteMode::ACTIVE && p_eligible ? sim::Pick::CHOSEN
                                                         : sim::Pick::AUTOMATIC;
}

} // namespace

void NetwMultiplayer::sim_release_row(const RID &p_entity) {
    if (const sim::Row *row = sim_rows.row_of(p_entity)) {
        for (const sim::AuthoredBody &held : row->bodies.held) {
            const RID *indexed = sim_bodies.getptr(uint64_t(held.node));
            if (indexed != nullptr && *indexed == p_entity) {
                sim_bodies.erase(uint64_t(held.node));
            }
        }
    }
    sim_refresh_members(sim_rows.release(p_entity));
}

uint32_t NetwMultiplayer::sim_selection_count(const RID &p_member) const {
    return sim_rows.selection_count(p_member);
}

RID NetwMultiplayer::sim_entity_of_key(int64_t p_key) const {
    return sim_rows.entity_of_key(p_key);
}

bool NetwMultiplayer::sim_chooses(const RID &p_subject) {
    const sim::Row *row = sim_rows.row_of(p_subject);
    if (row == nullptr) {
        return false;
    }
    if (sim_physics_driven(row->view)) {
        return true;
    }
    sim_rows.joiners(sim_joining);
    for (const RID &joiner : sim_joining) {
        if (joiner != p_subject) {
            return true;
        }
    }
    return false;
}

bool NetwMultiplayer::sim_predicts(const Ref<NetwEntity> &p_subject) const {
    return prediction_engine.slot_of(p_subject) >= 0;
}

bool NetwMultiplayer::sim_selects_unasked(
    const Ref<NetwEntity> &p_subject
) const {
    return sim_predicts(p_subject) && sim_physics_driven(p_subject.ptr());
}

TypedArray<NetwEntity> NetwMultiplayer::sim_joiners(
    const Ref<NetwEntity> &p_subject
) {
    return sim_gather(p_subject, false);
}

TypedArray<NetwEntity> NetwMultiplayer::sim_candidates(
    const Ref<NetwEntity> &p_subject
) {
    return sim_gather(p_subject, sim_selects_unasked(p_subject));
}

TypedArray<NetwEntity> NetwMultiplayer::sim_gather(
    const Ref<NetwEntity> &p_subject,
    bool p_shared
) {
    TypedArray<NetwEntity> out;
    if (p_subject.is_null()
        || sim_rows.row_of(p_subject->get_rid_handle()) == nullptr) {
        return out;
    }
    LocalVector<Ref<NetwEntity>> found;
    sim_rows.joiners(sim_joining);
    for (const RID &joiner : sim_joining) {
        take(*this, p_subject, entity_get_view(joiner), found);
    }
    if (p_shared) {
        const TypedArray<NetwEntity> shared
            = interest_shared_entities(p_subject, StringName());
        for (int at = 0; at < shared.size(); ++at) {
            const Ref<NetwEntity> member = shared[at];
            take(*this, p_subject, member, found);
        }
    }
    std::sort(found.ptr(), found.ptr() + found.size(), order_less);
    for (const Ref<NetwEntity> &member : found) {
        out.push_back(member);
    }
    return out;
}

bool NetwMultiplayer::sim_selectable(const Ref<NetwEntity> &p_member) {
    const RID member = p_member->get_rid_handle();
    if (!member.is_valid()) {
        return false;
    }
    const sim::Row *row = sim_rows.row_of(member);
    if (predict_engine_for(member) != nullptr) {
        return row != nullptr && row->facts.predicted && !sim::leads(row->mode);
    }
    Node *owner = p_member->get_owner();
    if (owner == nullptr || authoring::declares_prediction(owner)
        || predict_implied(p_member.ptr())
        || !sim_simulated(p_member.ptr())) {
        return false;
    }
    const sim::Mode mode = row != nullptr
        ? row->mode
        : sim_rows.resolve(member, sim_body_facts(p_member));
    return !sim::leads(mode);
}

TypedArray<NetwEntity> NetwMultiplayer::sim_select(
    const Ref<NetwEntity> &p_subject,
    const TypedArray<NetwEntity> &p_candidates,
    int64_t p_frontier
) {
    TypedArray<NetwEntity> promoted;
    if (p_subject.is_null()) {
        return promoted;
    }
    const RID subject = p_subject->get_rid_handle();
    const sim::Row *standing = sim_rows.row_of(subject);
    if (standing == nullptr || !sim::leads(standing->mode)) {
        sim_drop_selections(subject);
        return promoted;
    }

    LocalVector<Ref<NetwEntity>> ranked;
    ranked.reserve(uint32_t(p_candidates.size()) + 1);
    for (int at = 0; at < p_candidates.size(); ++at) {
        const Ref<NetwEntity> member = p_candidates[at];
        if (member.is_valid() && member != p_subject) {
            ranked.push_back(member);
        }
    }
    ranked.push_back(p_subject);
    std::sort(ranked.ptr(), ranked.ptr() + ranked.size(), order_less);

    HashMap<int64_t, Ref<NetwEntity>> by_key;
    LocalVector<sim::Candidate> candidates;
    int64_t subject_order = 0;
    for (uint32_t at = 0; at < ranked.size(); ++at) {
        const Ref<NetwEntity> &member = ranked[at];
        if (member == p_subject) {
            subject_order = int64_t(at);
            continue;
        }
        const RID entity = member->get_rid_handle();
        const int64_t key = int64_t(entity.get_id());
        if (!entity.is_valid() || by_key.has(key)) {
            continue;
        }
        sim::Candidate candidate;
        candidate.key = key;
        candidate.order_key = int64_t(at);
        candidate.eligible = sim_selectable(member);
        candidate.pick = pick_of(sim_rows.row_of(entity), candidate.eligible);
        candidates.push_back(candidate);
        by_key.insert(key, member);
    }

    sim::Row &row = sim_rows.ensure(subject);
    row.selection.owner_order_key = subject_order;
    row.selection.all = sim_selects_unasked(p_subject);
    row.selection.commit(candidates, p_frontier);

    LocalVector<int64_t> promoted_keys;
    LocalVector<int64_t> demoted_keys;
    for (const sim::Selected &member : row.selection.members) {
        if (member.promoted) {
            promoted_keys.push_back(member.key);
        } else {
            demoted_keys.push_back(member.key);
        }
    }
    LocalVector<RID> moved;
    for (const int64_t key : promoted_keys) {
        const HashMap<int64_t, Ref<NetwEntity>>::ConstIterator found
            = by_key.find(key);
        if (found == by_key.end()) {
            continue;
        }
        const RID member = found->value->get_rid_handle();
        if (sim_rows.note_selected(member, subject)) {
            moved.push_back(member);
        }
        promoted.push_back(found->value);
    }
    for (const int64_t key : demoted_keys) {
        const HashMap<int64_t, Ref<NetwEntity>>::ConstIterator found
            = by_key.find(key);
        const RID member = found != by_key.end()
            ? found->value->get_rid_handle()
            : sim_rows.entity_of_key(key);
        if (member.is_valid() && sim_rows.erase_selected(member, subject)) {
            moved.push_back(member);
        }
    }
    sim_refresh_members(moved);
    return promoted;
}

void NetwMultiplayer::sim_drop_selections(const RID &p_subject) {
    sim_refresh_members(sim_rows.drop_selections(p_subject));
}

void NetwMultiplayer::sim_note_selected_by(
    const RID &p_member,
    const RID &p_subject,
    bool p_selected
) {
    const bool moved = p_selected
        ? sim_rows.note_selected(p_member, p_subject)
        : sim_rows.erase_selected(p_member, p_subject);
    if (moved) {
        LocalVector<RID> members;
        members.push_back(p_member);
        sim_refresh_members(members);
    }
}

void NetwMultiplayer::sim_release_lingering(
    const RID &p_subject,
    int64_t p_floor
) {
    sim::Row *row = sim_rows.find(p_subject);
    if (row != nullptr) {
        row->selection.release_lingering(p_floor);
    }
}

void NetwMultiplayer::sim_seed(sim::Row &r_row) {
    const Ref<NetwEntity> entity = entity_get_view(r_row.entity);
    if (entity.is_null()) {
        return;
    }
    r_row.view = entity.ptr();
    r_row.steering = sim_steering_of(*entity.ptr());
    const NetwEntityRecord *record = record_of(entity.ptr());
    const sim::Decl *found
        = record != nullptr ? record->find_sim_declaration() : nullptr;
    const sim::Declaration undeclared;
    const sim::Declaration &held
        = found != nullptr ? found->declaration : undeclared;
    if (!r_row.bodies.recorded) {
        r_row.declaration.bodies = held.bodies;
    }
    r_row.declaration.schedule = held.schedule;
    r_row.declaration.remote_mode = held.remote_mode;
    r_row.declaration.restore = held.restore;
    r_row.declaration.max_restore_ticks = held.max_restore_ticks;
    r_row.declaration.step = held.step;
    r_row.declaration.claim_on_contact = held.claim_on_contact;
    r_row.declaration.release_on_rest = held.release_on_rest;
    r_row.facts.remote_mode = held.remote_mode;
}

void NetwMultiplayer::sim_declare(const Ref<NetwEntity> &p_entity) {
    if (p_entity.is_null()) {
        return;
    }
    const RID entity = p_entity->get_rid_handle();
    if (!entity.is_valid() || entity_get_view(entity).is_null()) {
        return;
    }
    sim::Row *standing = sim_rows.find(entity);
    const NetwEntityRecord *record = record_of(p_entity.ptr());
    const sim::Decl *found
        = record != nullptr ? record->find_sim_declaration() : nullptr;
    if (standing == nullptr && (found == nullptr || !found->declared)) {
        return;
    }
    {
        NETW_ZONE_NC("sim seed", colors::PREDICTION);
        sim_seed(standing != nullptr ? *standing : sim_rows.ensure(entity));
    }
    LocalVector<RID> members;
    members.push_back(entity);
    sim_refresh_members(members);
}

void NetwMultiplayer::sim_announce(const RID &p_entity) {
    const sim::Row *row = sim_rows.row_of(p_entity);
    const EntitySlot *slot
        = row != nullptr ? entity_slots.find(p_entity.get_id()) : nullptr;
    if (slot == nullptr || slot->record == nullptr || slot->live.is_null()
        || slot->record->get_announced_mode() == row->mode) {
        return;
    }
#if defined(NETW_TESTS)
    idle_work.announced += 1;
#endif
    sim_tell_mode(slot->record, slot->live, row->mode);
}

void NetwMultiplayer::sim_tell_mode(
    NetwEntityRecord *p_record,
    const Ref<NetwEntity> &p_entity,
    sim::Mode p_mode
) {
    const sim::Mode previous = p_record->get_announced_mode();
    if (previous == p_mode) {
        return;
    }
    p_record->set_announced_mode(p_mode);
    if (p_entity->simulation_if_minted().is_null()) {
        return;
    }
    Announcement row;
    row.kind = AnnouncementKind::MODE_CHANGED;
    row.from = int64_t(previous);
    row.to = int64_t(p_mode);
    entity_announce(p_entity.ptr(), row);
}

void NetwMultiplayer::sim_refresh_members(const LocalVector<RID> &p_members) {
    for (const RID &member : p_members) {
        const sim::Row *row = sim_rows.row_of(member);
        if (row == nullptr) {
            continue;
        }
        if (NetwPredictSlotEngine *engine = predict_engine_for(member)) {
            const bool selected = row->selected_by.count() > 0;
            prediction_engine.follow_relay_subscription(
                engine->native_slot(),
                selected
            );
            if (!selected) {
                prediction_engine.leave_joint(engine->native_slot());
            }
            if (sim::resolve(row->facts) != row->mode) {
                engine->rewire();
            }
            continue;
        }
        const Ref<NetwEntity> entity = entity_get_view(member);
        if (entity.is_null()) {
            continue;
        }
        {
            NETW_ZONE_NC("sim resolve body facts", colors::PREDICTION);
            sim_rows.resolve(member, sim_body_facts(entity));
        }
        {
            NETW_ZONE_NC("sim settle body", colors::PREDICTION);
            sim_settle_body(entity);
        }
        {
            NETW_ZONE_NC("sim mark role dirty", colors::PREDICTION);
            display_mark_role_dirty(member);
        }
        NETW_ZONE_NC("sim announce", colors::PREDICTION);
        sim_announce(member);
    }
}

} // namespace netw
