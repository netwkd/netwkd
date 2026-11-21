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

NetwPredictionEngine::~NetwPredictionEngine() {
    for (predict::Slot *row : rows) {
        if (row != nullptr) {
            memdelete(row);
        }
    }
    rows.clear();
}

#if defined(NETW_PROFILING) || defined(DEBUG_ENABLED)
void NetwPredictionEngine::held_bytes(
    int64_t &r_transitions,
    int64_t &r_inputs,
    int64_t &r_journal,
    int64_t &r_slots
) const {
    r_slots += held::vector(rows) + held::vector(order_keys)
        + held::vector(order_slots) + held::map(slot_by_record)
        + held::map(seat_by_slot);
    for (const predict::Slot *row : rows) {
        if (row == nullptr) {
            continue;
        }
        r_transitions += row->book.held_bytes();
        r_inputs += row->inputs.held_bytes();
        r_journal += row->journal.held_bytes() + row->tape.held_bytes();
        r_slots += int64_t(sizeof(predict::Slot))
            + held::vector(row->owner_has_state)
            + held::vector(row->owner_has_input)
            + held::vector(row->state_ports) + held::vector(row->input_ports)
            + held::map(row->sensors) + row->command_scratch.held_bytes()
            + row->fingerprint_scratch.held_bytes()
            + held::vector(row->correction_tolerances)
            + held::vector(row->meter_tolerances)
            + held::vector(row->judged_errors);
        for (const predict::StateRow *state :
             {&row->pulled,
              &row->compared,
              &row->received,
              &row->arrived,
              &row->command,
              &row->frame_command,
              &row->stall,
              &row->last,
              &row->live,
              &row->state}) {
            r_slots
                += held::vector(state->values) + held::vector(state->present);
        }
    }
}
#endif

const predict::Slot *NetwPredictionEngine::row_of(int64_t p_slot) const {
    const int64_t seat = ids.seat(p_slot);
    const predict::Slot *row = seat >= 0 && seat < int64_t(rows.size())
        ? rows[uint32_t(seat)]
        : nullptr;
    return row != nullptr && row->open ? row : nullptr;
}

predict::Slot *NetwPredictionEngine::mutable_row_of(int64_t p_slot) {
    const int64_t seat = ids.seat(p_slot);
    predict::Slot *row = seat >= 0 && seat < int64_t(rows.size())
        ? rows[uint32_t(seat)]
        : nullptr;
    return row != nullptr && row->open ? row : nullptr;
}

int64_t NetwPredictionEngine::open(
    const LocalVector<predict::FieldDecl> &p_declaration
) {
    if (!roster_open()) {
        return -1;
    }
    const int64_t slot = ids.acquire();
    const uint32_t seat = SlotIds::index_of(slot);
    predict::Slot *row = memnew(predict::Slot);
    row->session = session;
    row->open = true;
    const uint32_t depth = predict::book_depth_for(row->config.schedule);
    row->book.set_depth(depth);
    row->journal.set_capacity(int(depth));
    row->journal.arm_diagnostics(journal_readers);
    if (!p_declaration.is_empty()) {
        row->rewire(predict::compile(p_declaration));
    }
    while (rows.size() <= seat) {
        rows.push_back(nullptr);
    }
    rows[seat] = row;
    return slot;
}

namespace {

Object *state_target(
    const predict::Slot &p_row,
    Object *p_owner,
    uint32_t p_field
) {
    if (p_field < p_row.state_nodes.size()
        && p_row.state_nodes[p_field].is_valid()) {
        return gd::object_of(p_row.state_nodes[p_field]);
    }
    return p_owner;
}

const StringName &state_prop(
    const predict::Slot &p_row,
    const predict::FieldCodec &p_codec,
    uint32_t p_field
) {
    return &p_codec == &p_row.wiring.codec && p_field < p_row.state_props.size()
        ? p_row.state_props[p_field]
        : p_codec.keys[p_field];
}

Object *codec_target(
    const predict::Slot &p_row,
    const predict::FieldCodec &p_codec,
    Object *p_owner,
    uint32_t p_field
) {
    return &p_codec == &p_row.wiring.codec
        ? state_target(p_row, p_owner, p_field)
        : p_owner;
}

} // namespace

void NetwPredictionEngine::resolve_reach(
    predict::Slot &p_row,
    const Object *p_owner
) {
    p_row.owner_has_state.clear();
    p_row.owner_has_input.clear();
    const uint32_t fields = uint32_t(p_row.wiring.codec.count());
    p_row.state_nodes.resize(fields);
    p_row.state_props.resize(fields);
    for (uint32_t at = 0; at < fields; ++at) {
        p_row.state_nodes[at] = ObjectID();
        p_row.state_props[at] = p_row.wiring.codec.keys[at];
    }
    for (const predict::StatePart &part : p_row.parts) {
        for (uint32_t at = part.first;
             at < part.first + part.count && at < fields;
             ++at) {
            const String key = p_row.wiring.codec.keys[at];
            p_row.state_nodes[at] = part.node;
            p_row.state_props[at]
                = StringName(key.substr(part.path.length() + 1));
        }
    }
    Object *owner = const_cast<Object *>(p_owner);
    store::resolve_columns(owner, p_row.wiring.codec.keys, p_row.state_ports);
    for (uint32_t at = 0; at < fields; ++at) {
        Object *target = state_target(p_row, owner, at);
        p_row.owner_has_state.push_back(
            gd::has_property(target, p_row.state_props[at]) ? 1 : 0
        );
        if (target != owner && at < p_row.state_ports.size()) {
            LocalVector<StringName> bare;
            bare.push_back(p_row.state_props[at]);
            LocalVector<store::Column> resolved;
            store::resolve_columns(target, bare, resolved);
            p_row.state_ports[at] = resolved[0];
            if (p_row.state_ports[at].port == store::Port::BODY) {
                p_row.state_ports[at].port = store::Port::ENGINE;
                p_row.state_ports[at].body = store::BodyKey::NONE;
            }
        }
    }
    for (int at = 0; at < p_row.input_codec.count(); ++at) {
        p_row.owner_has_input.push_back(
            gd::has_property(p_owner, p_row.input_codec.keys[at]) ? 1 : 0
        );
    }
    store::resolve_columns(owner, p_row.input_codec.keys, p_row.input_ports);
    p_row.pulled.resize(p_row.wiring.codec.count());
}

bool NetwPredictionEngine::bind_owner(int64_t p_slot, Object *p_owner) {
    if (!roster_open()) {
        return false;
    }
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr || p_owner == nullptr) {
        return false;
    }
    row->port.bind(p_owner);
    row->owner_solves
        = p_owner->is_class("RigidBody2D") || p_owner->is_class("RigidBody3D");
    resolve_reach(*row, p_owner);
    NETW_TRACE(
        sys::PREDICTION,
        "Predicted entity %d is bound to its node.",
        p_slot
    );
    return row->port.bound();
}

void NetwPredictionEngine::unbind_owner(int64_t p_slot) {
    if (!roster_open()) {
        return;
    }
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->port.unbind();
        row->owner_solves = false;
        row->owner_has_state.clear();
        row->owner_has_input.clear();
        row->state_ports.clear();
        row->input_ports.clear();
    }
}

bool NetwPredictionEngine::owner_bound(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr && row->port.bound();
}

bool NetwPredictionEngine::owner_solves(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr && row->port.bound() && row->owner_solves;
}

Object *NetwPredictionEngine::resolve_owner(predict::Slot &p_row) {
    return p_row.port.resolve(sys::PREDICTION);
}

namespace {

bool reaches(const LocalVector<uint8_t> &p_reach, uint32_t p_field) {
    return p_field < p_reach.size() && p_reach[p_field] != 0;
}

void read_binding(
    const PropertyBindingPtr &p_binding,
    const predict::FieldCodec &p_codec,
    predict::StateRow &r_row
) {
    const int count = p_codec.count();
    r_row.resize(count);
    for (int at = 0; at < count; ++at) {
        r_row.values[uint32_t(at)] = Variant();
    }
    Node *bound = p_binding != nullptr ? p_binding->node() : nullptr;
    if (bound == nullptr) {
        return;
    }
    Variant value;
    for (int at = 0; at < count; ++at) {
        const StringName &key = p_codec.keys[uint32_t(at)];
        if (p_binding->field_of(key) != nullptr
            && p_binding->read_field(bound, key, value)) {
            r_row.set(at, value);
        }
    }
}

void project_row(
    const predict::Wiring &p_wiring,
    double p_age,
    predict::StateRow &r_row
) {
    if (p_age <= 0.0) {
        return;
    }
    const predict::StateRow source = r_row;
    const int count = std::min(p_wiring.count(), int(source.values.size()));
    for (int at = 0; at < count; ++at) {
        const int channel = p_wiring.projection[uint32_t(at)];
        if (channel < 0 || channel >= count || !source.has(at)
            || !source.has(channel)) {
            continue;
        }
        const Variant &value = source.values[uint32_t(at)];
        if (project::supports(int(value.get_type()))) {
            r_row.set(
                at,
                project::forward(value, source.values[uint32_t(channel)], p_age)
            );
        }
    }
}

bool pull_fields(
    Object *p_owner,
    const predict::FieldCodec &p_codec,
    const LocalVector<store::Column> &p_ports,
    const LocalVector<uint8_t> &p_reach,
    predict::StateRow &r_row,
    const predict::Slot *p_slot = nullptr
) {
    const uint32_t count = uint32_t(p_codec.count());
    if (r_row.values.size() != count) {
        r_row.resize(int(count));
    }
    store::BodyState body;
    bool probed = false;
    for (uint32_t at = 0; at < count; ++at) {
        r_row.present[at] = 0;
        if (!reaches(p_reach, at)) {
            continue;
        }
        if (p_slot != nullptr) {
            Object *target = state_target(*p_slot, p_owner, at);
            if (target != p_owner) {
                if (target != nullptr) {
                    r_row.set(
                        int(at),
                        store::object_value(target, p_slot->state_props[at])
                    );
                }
                continue;
            }
        }
        const StringName &key = p_codec.keys[at];
        const bool through_body
            = at < p_ports.size() && p_ports[at].port == store::Port::BODY;
        if (through_body && !probed) {
            store::body_probe(p_owner, body);
            store::body_read(body);
            probed = true;
        }
        const store::BodyKey part
            = through_body ? p_ports[at].body : store::BodyKey::NONE;
        r_row.set(
            int(at),
            through_body ? store::body_value(body, part, p_owner, key)
                         : store::object_value(p_owner, key)
        );
    }
    return true;
}

} // namespace

bool NetwPredictionEngine::pull_state(predict::Slot &p_row) {
    NETW_ZONE_LEAF(
        "Predict pull state",
        colors::PREDICTION,
        profile::SUBSYSTEM_PREDICTION
    );
    Object *owner = resolve_owner(p_row);
    if (owner == nullptr) {
        return false;
    }
    return pull_fields(
        owner,
        p_row.wiring.codec,
        p_row.state_ports,
        p_row.owner_has_state,
        p_row.pulled,
        &p_row
    );
}

bool NetwPredictionEngine::push_state(predict::Slot &p_row) {
    NETW_ZONE_LEAF(
        "Predict push state",
        colors::PREDICTION,
        profile::SUBSYSTEM_PREDICTION
    );
    Object *owner = resolve_owner(p_row);
    if (owner == nullptr) {
        return false;
    }
    const predict::StateRow &values = p_row.pulled;
    const uint32_t count
        = std::min(uint32_t(p_row.wiring.codec.count()), values.values.size());
    for (int rank = 0; rank < store::PUSH_RANKS; ++rank) {
        for (uint32_t at = 0; at < count; ++at) {
            const int held = at < p_row.state_ports.size()
                ? store::push_rank(p_row.state_ports[at])
                : 0;
            if (held == rank && reaches(p_row.owner_has_state, at)
                && values.has(int(at))) {
                port_set(
                    state_target(p_row, owner, at),
                    state_prop(p_row, p_row.wiring.codec, at),
                    values.values[at]
                );
            }
        }
    }
    return true;
}

bool NetwPredictionEngine::push_row(
    predict::Slot &p_row,
    const predict::FieldCodec &p_codec,
    const LocalVector<uint8_t> &p_reach,
    const predict::StateRow &p_values
) {
    NETW_ZONE_LEAF(
        "Predict push row",
        colors::PREDICTION,
        profile::SUBSYSTEM_PREDICTION
    );
    Object *owner = resolve_owner(p_row);
    if (owner == nullptr) {
        return false;
    }
    const int count = std::min(p_codec.count(), int(p_values.values.size()));
    for (int at = 0; at < count; ++at) {
        if (reaches(p_reach, uint32_t(at)) && p_values.has(at)) {
            port_set(
                codec_target(p_row, p_codec, owner, uint32_t(at)),
                state_prop(p_row, p_codec, uint32_t(at)),
                p_values.values[uint32_t(at)]
            );
        }
    }
    return true;
}

bool NetwPredictionEngine::capture_canonical(
    int64_t p_slot,
    predict::StateRow &r_row
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return false;
    }
    if (row->port.bound()) {
        if (!pull_state(*row)) {
            r_row.resize(row->wiring.count());
            return false;
        }
        r_row = row->pulled;
    } else if (state_binding_in(*row) != nullptr) {
        read_binding(state_binding_in(*row), row->wiring.codec, r_row);
    } else {
        r_row.resize(row->wiring.count());
        return false;
    }
    predict::canonicalize_row(
        row->wiring.codec,
        r_row,
        row->recovering.scratch,
        row->recovering.codes,
        session
    );
    return true;
}

bool NetwPredictionEngine::capture_predicted(int64_t p_slot, int64_t p_key) {
    NETW_ZONE_LEAF(
        "Predict capture into book",
        colors::PREDICTION,
        profile::SUBSYSTEM_PREDICTION
    );
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return false;
    }
    if (!row->port.bound()) {
        capture_canonical(p_slot, row->pulled);
        return row->record_state_row(p_key, row->pulled);
    }
    return pull_state(*row) && row->record_state_row(p_key, row->pulled);
}

void NetwPredictionEngine::capture_into_book(int64_t p_slot, int64_t p_key) {
    capture_predicted(p_slot, p_key);
}

bool NetwPredictionEngine::read_predicted(
    int64_t p_slot,
    int64_t p_key,
    predict::StateRow &r_state
) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr
        && row->book.read_row(row->wiring.codec, p_key, r_state);
}

bool NetwPredictionEngine::restore_predicted(int64_t p_slot, int64_t p_key) {
    NETW_ZONE_LEAF(
        "Predict restore from book",
        colors::PREDICTION,
        profile::SUBSYSTEM_PREDICTION
    );
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr || !row->port.bound()
        || !row->book.read_row(row->wiring.codec, p_key, row->pulled)
        || !push_state(*row)) {
        return false;
    }
    colliders::teleport(row->port.resolve(sys::PREDICTION));
    return true;
}

bool NetwPredictionEngine::seat_restore_row(
    int64_t p_slot,
    int64_t p_key,
    const predict::StateRow &p_restore
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr || !row->port.bound() || p_key < 0) {
        return false;
    }
    predict::StateRow &seed = row->pulled;
    const int count = row->wiring.codec.count();
    if (!row->book.read_row(row->wiring.codec, p_key, seed)
        || int(seed.values.size()) != count) {
        seed.resize(count);
    }
    const int given = std::min(count, int(p_restore.values.size()));
    for (int at = 0; at < given; ++at) {
        if (p_restore.has(at)) {
            seed.set(at, p_restore.values[uint32_t(at)]);
        }
    }
    return seed.any() && row->record_state_row(p_key, seed);
}

Dictionary NetwPredictionEngine::capture_state(int64_t p_slot) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr || !pull_state(*row)) {
        return Dictionary();
    }
    return payload_of(row->wiring.codec, row->pulled);
}

bool NetwPredictionEngine::hold_live_input(int64_t p_slot) {
    predict::Slot *row = mutable_row_of(p_slot);
    return row != nullptr && pull_command(*row, row->live);
}

bool NetwPredictionEngine::restore_live_input(int64_t p_slot) {
    predict::Slot *row = mutable_row_of(p_slot);
    return row != nullptr && push_command(*row, row->live);
}

bool NetwPredictionEngine::capture_command(int64_t p_slot) {
    predict::Slot *row = mutable_row_of(p_slot);
    return row != nullptr && pull_command(*row, row->command);
}

bool NetwPredictionEngine::apply_command(
    int64_t p_slot,
    const predict::StateRow &p_command
) {
    predict::Slot *row = mutable_row_of(p_slot);
    return row != nullptr && push_command(*row, p_command);
}

predict::StateRow NetwPredictionEngine::bound_input(int64_t p_slot) {
    predict::StateRow out;
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr && input_binding_in(*row) != nullptr
        && input_binding_in(*row)->node() != nullptr) {
        pull_command(*row, out);
    }
    return out;
}

bool NetwPredictionEngine::pull_command(
    predict::Slot &p_row,
    predict::StateRow &r_command
) {
    const int count = p_row.input_codec.count();
    if (int(r_command.values.size()) != count) {
        r_command.resize(count);
    }
    for (int at = 0; at < count; ++at) {
        r_command.values[uint32_t(at)] = Variant();
        r_command.present[uint32_t(at)] = 0;
    }
    repl::PropertyBinding *const binding = input_binding_in(p_row);
    if (binding != nullptr && binding->node() != nullptr) {
        read_binding(binding, p_row.input_codec, r_command);
        return true;
    }
    Object *owner = resolve_owner(p_row);
    if (owner == nullptr) {
        return false;
    }
    return pull_fields(
        owner,
        p_row.input_codec,
        p_row.input_ports,
        p_row.owner_has_input,
        r_command
    );
}

int32_t NetwPredictionEngine::command_hash_of(
    predict::Slot &p_row,
    const predict::StateRow &p_command
) {
    bool any = false;
    const int32_t hashed = predict::fingerprint_row(
        p_row.input_codec,
        p_command,
        ~uint64_t(0),
        p_row.command_scratch,
        any,
        p_row.session
    );
    return any ? hashed : predict::fnv1a(nullptr, 0);
}

int64_t NetwPredictionEngine::command_hash_row(
    int64_t p_slot,
    const predict::StateRow &p_command
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return predict::fnv1a(nullptr, 0);
    }
    return command_hash_of(*row, p_command);
}

bool NetwPredictionEngine::author_command(int64_t p_slot, int64_t p_tick) {
    NETW_PREDICT_TICK_SCOPE();
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr || !pull_command(*row, row->command)) {
        return false;
    }
    predict::canonicalize_row(
        row->input_codec,
        row->command,
        row->command_scratch,
        row->command_codes,
        session
    );
    if (row->lane_open && row->config.schedule != int(Schedule::FRAME)) {
        row->record_authored_row(p_tick, p_tick, true, row->command);
    }
    if (!predict::same_command(row->command, row->standing_input)) {
        row->standing_input = row->command;
        row->lane_cursor.input_changed_at = p_tick;
    }
    row->cursor.latest_input_tick = p_tick;
    if (input_binding_in(*row) != nullptr) {
        input_binding_in(*row)->set_authored_tick(p_tick);
    }
    record_input(p_slot, p_tick, command_hash_of(*row, row->command));
    return true;
}

const predict::StateRow *NetwPredictionEngine::command_of(
    int64_t p_slot
) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? &row->command : nullptr;
}

bool NetwPredictionEngine::read_command_at(
    int64_t p_slot,
    int64_t p_transition,
    predict::StateRow &r_command
) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr
        && row->inputs.read_row(row->input_codec, p_transition, r_command);
}

bool NetwPredictionEngine::authored_command(
    int64_t p_slot,
    int64_t p_key,
    predict::StateRow &r_command
) const {
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr) {
        return false;
    }
    row->authored_row(p_key, r_command);
    return true;
}

void NetwPredictionEngine::call_step(
    const Callable &p_step,
    double p_delta,
    int64_t p_tick,
    bool p_fresh
) {
    Array arguments;
    arguments.push_back(p_delta);
    arguments.push_back(p_tick);
    arguments.push_back(p_fresh);
    const int stale = p_fresh ? 0 : 1;
    replay_steps += stale;
    game_call(p_step, arguments, game_calls::SIMULATION_STEP);
    replay_steps -= stale;
}

void NetwPredictionEngine::set_simulate(
    int64_t p_slot,
    const Callable &p_callable
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->simulate = p_callable;
    }
}

void NetwPredictionEngine::set_corridor(
    int64_t p_slot,
    const Callable &p_callable
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->corridor = p_callable;
    }
}

void NetwPredictionEngine::set_sensor(
    int64_t p_slot,
    const StringName &p_name,
    const Callable &p_callable
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return;
    }
    if (p_callable.is_valid()) {
        row->sensors[p_name] = p_callable;
    } else {
        row->sensors.erase(p_name);
    }
}

const predict::StateRow *NetwPredictionEngine::frame_command_of(
    int64_t p_slot
) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? &row->frame_command : nullptr;
}

void NetwPredictionEngine::hold_frame_command(int64_t p_slot) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->frame_command = row->command;
    }
}

const predict::StateRow *NetwPredictionEngine::stall_row_of(
    int64_t p_slot
) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? &row->stall : nullptr;
}

const predict::StateRow *NetwPredictionEngine::last_row_of(
    int64_t p_slot
) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? &row->last : nullptr;
}

void NetwPredictionEngine::forget_last_input(int64_t p_slot) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->last.resize(0);
    }
}

void NetwPredictionEngine::forget_commands(int64_t p_slot) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->forget_commands();
    }
}

bool NetwPredictionEngine::registered_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? false : row->latches.registered;
}

void NetwPredictionEngine::set_registered(int64_t p_slot, bool p_value) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->latches.registered = p_value;
    }
}

bool NetwPredictionEngine::previous_witness_sleeping_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? false : row->latches.previous_witness_sleeping;
}

void NetwPredictionEngine::set_previous_witness_sleeping(
    int64_t p_slot,
    bool p_value
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->latches.previous_witness_sleeping = p_value;
    }
}

bool NetwPredictionEngine::has_previous_witness_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? false : row->latches.has_previous_witness;
}

void NetwPredictionEngine::set_has_previous_witness(
    int64_t p_slot,
    bool p_value
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->latches.has_previous_witness = p_value;
    }
}

Dictionary NetwPredictionEngine::witness_sample(
    int64_t p_slot,
    const predict::Decl *p_decl
) {
    NETW_ZONE_LEAF(
        "Predict witness sample",
        colors::PREDICTION,
        profile::SUBSYSTEM_PREDICTION
    );
    const NetwEntityRecord *seated = owner_record(p_slot);
    if (p_decl == nullptr || seated == nullptr || !samples_contacts(p_slot)) {
        return Dictionary();
    }
    LocalVector<ObjectID> bodies;
    const sim::Row *row
        = session != nullptr ? sim_rows->row_of(seated->get_handle()) : nullptr;
    if (row != nullptr && row->bodies.recorded) {
        for (const sim::AuthoredBody &held : row->bodies.held) {
            bodies.push_back(held.node);
        }
    } else {
        const sim::Decl *declared = seated->find_sim_declaration();
        sim::collect_bodies(
            owner_node(p_slot),
            declared != nullptr ? declared->declaration.bodies
                                : Vector<NodePath>(),
            bodies
        );
    }
    return sim::sample_contacts(bodies);
}

bool NetwPredictionEngine::samples_contacts(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr && row->config.witness;
}

bool NetwPredictionEngine::predict_command(int64_t p_slot, int64_t p_tick) {
    NETW_ZONE_LEAF(
        "Predict predicted command",
        colors::PREDICTION,
        profile::SUBSYSTEM_PREDICTION
    );
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return false;
    }
    if (session != nullptr) {
        if (!relayed_guess(p_slot, p_tick, row->command)) {
            coast_command(p_slot, row->command);
        }
    } else {
        row->command.resize(row->input_codec.count());
        NETW_TRACE(
            sys::PREDICTION,
            "Predicted entity %d has no session, so it starts from empty "
            "state.",
            p_slot
        );
    }
    const predict::Decl *decl = decl_of(p_slot);
    const Callable predictor = decl != nullptr ? decl->input : Callable();
    if (!predictor.is_valid()) {
        return seal_predicted_command(p_slot);
    }
    const NetwPrediction *part
        = Object::cast_to<NetwPrediction>(handle_of(p_slot));
    const Ref<NetwEntity> subject
        = part != nullptr ? part->get_entity() : Ref<NetwEntity>();
    Array arguments;
    arguments.push_back(subject);
    arguments.push_back(p_tick);
    bool called = false;
    const Variant predicted = game_call(
        predictor,
        arguments,
        game_calls::PREDICT_INPUT_CALL,
        &called
    );
    row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return false;
    }
    if (predicted.get_type() == Variant::DICTIONARY) {
        const Dictionary overlay = predicted;
        for (int at = 0; at < row->input_codec.count(); ++at) {
            const StringName &key = row->input_codec.keys[uint32_t(at)];
            if (overlay.has(key)) {
                row->command.set(at, overlay[key]);
            }
        }
        return seal_predicted_command(p_slot);
    }
    if (called) {
        fault::raise(
            session,
            NETW_FAULT_BAD_RETURN,
            {String("{NetwPrediction.predict_input}"),
             String("something other than a Dictionary"),
             String("a Dictionary of input property names to values")},
            uint64_t(gd::instance_id(owner_node(p_slot)))
        );
    }
    NETW_TRACE(
        sys::PREDICTION,
        "Predicted entity %d declared an input predictor that cannot be used.",
        p_slot
    );
    return seal_predicted_command(p_slot);
}

bool NetwPredictionEngine::seal_predicted_command(int64_t p_slot) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return false;
    }
    predict::canonicalize_row(
        row->input_codec,
        row->command,
        row->command_scratch,
        row->command_codes,
        session
    );
    return true;
}

void NetwPredictionEngine::set_joint_refusal_reported(
    int64_t p_slot,
    bool p_value
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->latches.joint_refusal_reported = p_value;
    }
}

void NetwPredictionEngine::set_joint_basis(int64_t p_slot, int64_t p_value) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->latches.joint_basis = p_value;
    }
}

void NetwPredictionEngine::set_joint_relay_floor(
    int64_t p_slot,
    int64_t p_value
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->latches.joint_relay_floor = p_value;
    }
}

void NetwPredictionEngine::set_joint_epoch_floor(
    int64_t p_slot,
    int64_t p_value
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->latches.joint_epoch_floor = p_value;
    }
}

int64_t NetwPredictionEngine::next_tape_entry_index_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? 0 : row->next_tape_entry_index;
}

void NetwPredictionEngine::set_next_tape_entry_index(
    int64_t p_slot,
    int64_t p_value
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->next_tape_entry_index = p_value;
    }
}

int64_t NetwPredictionEngine::last_driven_entry_index_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? -1 : row->last_driven_entry_index;
}

void NetwPredictionEngine::set_last_driven_entry_index(
    int64_t p_slot,
    int64_t p_value
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->last_driven_entry_index = p_value;
    }
}

int64_t NetwPredictionEngine::last_recorded_entry_index_of(
    int64_t p_slot
) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? -1 : row->lane_cursor.last_recorded_entry_index;
}

void NetwPredictionEngine::set_last_recorded_entry_index(
    int64_t p_slot,
    int64_t p_value
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->lane_cursor.last_recorded_entry_index = p_value;
    }
}

int64_t NetwPredictionEngine::replay_cursor_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? -1 : row->lane_cursor.replay_cursor;
}

void NetwPredictionEngine::set_replay_cursor(int64_t p_slot, int64_t p_value) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->lane_cursor.replay_cursor = p_value;
    }
}

int64_t NetwPredictionEngine::last_replayed_label_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? -1 : row->lane_cursor.last_replayed_label;
}

void NetwPredictionEngine::set_last_replayed_label(
    int64_t p_slot,
    int64_t p_value
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->lane_cursor.last_replayed_label = p_value;
    }
}

bool NetwPredictionEngine::last_replayed_fresh_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? false : row->lane_cursor.last_replayed_fresh;
}

void NetwPredictionEngine::set_last_replayed_fresh(
    int64_t p_slot,
    bool p_value
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->lane_cursor.last_replayed_fresh = p_value;
    }
}

int64_t NetwPredictionEngine::next_input_tick_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? -1 : row->lane_cursor.next_input_tick;
}

void NetwPredictionEngine::set_next_input_tick(
    int64_t p_slot,
    int64_t p_value
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->lane_cursor.next_input_tick = p_value;
    }
}

int64_t NetwPredictionEngine::ack_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? -1 : row->lane_cursor.ack;
}

void NetwPredictionEngine::set_ack(int64_t p_slot, int64_t p_value) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->lane_cursor.ack = p_value;
    }
}

bool NetwPredictionEngine::ack_advanced_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? false : row->lane_cursor.ack_advanced;
}

void NetwPredictionEngine::set_ack_advanced(int64_t p_slot, bool p_value) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->lane_cursor.ack_advanced = p_value;
    }
}

int64_t NetwPredictionEngine::ack_of_acks_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? -1 : row->lane_cursor.ack_of_acks;
}

void NetwPredictionEngine::set_ack_of_acks(int64_t p_slot, int64_t p_value) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->lane_cursor.ack_of_acks = p_value;
        if (p_value < 0) {
            row->lane_cursor.ack_mismatched = -1;
        }
    }
}

bool NetwPredictionEngine::ack_domain_confirmed_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? true : row->lane_cursor.ack_domain_confirmed;
}

void NetwPredictionEngine::set_ack_domain_confirmed(
    int64_t p_slot,
    bool p_value
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->lane_cursor.ack_domain_confirmed = p_value;
    }
}

int64_t NetwPredictionEngine::owner_ack_floor_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? -1 : row->lane_cursor.owner_ack_floor;
}

void NetwPredictionEngine::set_owner_ack_floor(
    int64_t p_slot,
    int64_t p_value
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->lane_cursor.owner_ack_floor = p_value;
        if (p_value < 0) {
            for (int64_t &sent : row->lane_cursor.ack_detailed) {
                sent = -1;
            }
        }
    }
}

int64_t NetwPredictionEngine::newest_matrix_transition_of(
    int64_t p_slot
) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? -1 : row->lane_cursor.newest_matrix_transition;
}

void NetwPredictionEngine::set_newest_matrix_transition(
    int64_t p_slot,
    int64_t p_value
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->lane_cursor.newest_matrix_transition = p_value;
    }
}

int NetwPredictionEngine::arrivals_this_frame_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? 0 : row->lane_cursor.arrivals_this_frame;
}

void NetwPredictionEngine::set_arrivals_this_frame(
    int64_t p_slot,
    int p_value
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->lane_cursor.arrivals_this_frame = p_value;
    }
}

double NetwPredictionEngine::tick_delta_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? 1.0 / 60.0 : row->tick_delta;
}

void NetwPredictionEngine::set_tick_delta(int64_t p_slot, double p_value) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->tick_delta = p_value;
    }
}

int64_t NetwPredictionEngine::frame_index_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? 0 : row->frame_index;
}

void NetwPredictionEngine::set_frame_index(int64_t p_slot, int64_t p_value) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->frame_index = p_value;
    }
}

int64_t NetwPredictionEngine::latest_input_tick_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? -1 : row->cursor.latest_input_tick;
}

void NetwPredictionEngine::set_latest_input_tick(
    int64_t p_slot,
    int64_t p_value
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->cursor.latest_input_tick = p_value;
    }
}

int64_t NetwPredictionEngine::last_driven_input_tick_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? -1 : row->cursor.last_driven_input_tick;
}

void NetwPredictionEngine::set_last_driven_input_tick(
    int64_t p_slot,
    int64_t p_value
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->cursor.last_driven_input_tick = p_value;
    }
}

int64_t NetwPredictionEngine::last_frame_transition_tick_of(
    int64_t p_slot
) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? -1 : row->cursor.last_frame_transition_tick;
}

void NetwPredictionEngine::set_last_frame_transition_tick(
    int64_t p_slot,
    int64_t p_value
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->cursor.last_frame_transition_tick = p_value;
    }
}

int64_t NetwPredictionEngine::last_recorded_input_tick_of(
    int64_t p_slot
) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? -1 : row->cursor.last_recorded_input_tick;
}

void NetwPredictionEngine::set_last_recorded_input_tick(
    int64_t p_slot,
    int64_t p_value
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->cursor.last_recorded_input_tick = p_value;
    }
}

bool NetwPredictionEngine::has_simulate(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr && row->simulate.is_valid();
}

bool NetwPredictionEngine::push_command(
    predict::Slot &p_row,
    const predict::StateRow &p_command
) {
    const uint32_t count = std::min(
        uint32_t(p_row.input_codec.count()),
        p_command.values.size()
    );
    repl::PropertyBinding *const binding = input_binding_in(p_row);
    Node *bound = binding != nullptr ? binding->node() : nullptr;
    if (bound != nullptr) {
        for (uint32_t at = 0; at < count; ++at) {
            if (p_command.has(int(at))) {
                binding->write_field(
                    bound,
                    p_row.input_codec.keys[at],
                    p_command.values[at]
                );
            }
        }
        return true;
    }
    Object *owner = resolve_owner(p_row);
    if (owner == nullptr) {
        return false;
    }
    for (uint32_t at = 0; at < count; ++at) {
        if (reaches(p_row.owner_has_input, at) && p_command.has(int(at))) {
            port_set(owner, p_row.input_codec.keys[at], p_command.values[at]);
        }
    }
    return true;
}

bool NetwPredictionEngine::run_command(
    int64_t p_slot,
    const predict::StateRow &p_command,
    double p_delta,
    int64_t p_tick,
    bool p_fresh
) {
    NETW_ZONE_LEAF(
        "Predict run step",
        colors::PREDICTION,
        profile::SUBSYSTEM_PREDICTION
    );
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return false;
    }
    {
        NETW_PREDICT_TICK_SCOPE();
        push_command(*row, p_command);
    }
    const Callable step = row->simulate;
    depth += 1;
    if (step.is_valid()) {
        NETW_ZONE_LEAF(
            "Predict game step",
            colors::PREDICTION,
            profile::SUBSYSTEM_PREDICTION
        );
        call_step(step, p_delta, p_tick, p_fresh);
    }
    depth -= 1;
    return mutable_row_of(p_slot) != nullptr;
}

Dictionary NetwPredictionEngine::canonicalize_state(
    int64_t p_slot,
    const Dictionary &p_payload
) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr
        ? predict::canonicalize(row->wiring.codec, p_payload, session)
        : p_payload.duplicate();
}

bool NetwPredictionEngine::coast_command(
    int64_t p_slot,
    predict::StateRow &r_command
) const {
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr) {
        r_command.resize(0);
        return false;
    }
    predict::zero_state_row(row->input_codec, r_command);
    return true;
}

bool NetwPredictionEngine::predicts_input(int64_t p_slot) const {
    const predict::Decl *decl = decl_of(p_slot);
    return decl != nullptr && decl->input.is_valid();
}

bool NetwPredictionEngine::relayed_guess(
    int64_t p_slot,
    int64_t p_transition,
    predict::StateRow &r_command
) const {
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr || relayed_epoch_of(p_slot) < 0) {
        return false;
    }
    const int64_t oldest = row->inputs.oldest_key();
    if (oldest < 0) {
        return false;
    }
    const int relayed = int(predict::CellProvenance::RELAYED);
    for (int64_t key = std::min(p_transition, row->inputs.newest_key());
         key >= oldest;
         --key) {
        if (row->inputs.origin_of(key) == relayed) {
            return row->inputs.read_row(row->input_codec, key, r_command);
        }
    }
    return false;
}

int64_t NetwPredictionEngine::sample_environment(
    int64_t p_slot,
    int64_t p_epoch
) {
    NETW_ZONE_LEAF(
        "Predict sample environment",
        colors::PREDICTION,
        profile::SUBSYSTEM_PREDICTION
    );
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return 0;
    }
    if (p_epoch == -1 && row->sensors.is_empty()) {
        row->last_samples = Dictionary();
        return 0;
    }
    Dictionary samples;
    depth += 1;
    for (const KeyValue<StringName, Callable> &sensor : row->sensors) {
        if (sensor.value.is_valid()) {
            samples[sensor.key]
                = game_call(sensor.value, Array(), game_calls::SENSOR_CALL);
        }
    }
    depth -= 1;
    row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return 0;
    }
    row->last_samples = samples;
    return prediction_core::environment_digest(p_epoch, samples);
}

Dictionary NetwPredictionEngine::sensor_samples(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? row->last_samples : Dictionary();
}

void NetwPredictionEngine::close(int64_t p_slot) {
    if (roster_open()) {
        predict::Slot *row = mutable_row_of(p_slot);
        if (row != nullptr) {
            unbind_store(p_slot);
            release_pins(*row);
            rows[SlotIds::index_of(p_slot)] = nullptr;
            ids.release(p_slot);
            memdelete(row);
        }
        unseat(p_slot);
    }
}

void NetwPredictionEngine::unseat(int64_t p_slot) {
    const HashMap<int64_t, Seat>::ConstIterator seated
        = seat_by_slot.find(p_slot);
    if (seated == seat_by_slot.end()) {
        return;
    }
    slot_by_record.erase(seated->value.record);
    seat_by_slot.erase(p_slot);
}

int64_t NetwPredictionEngine::slot_register(
    NetwEntityRecord &p_record,
    Node *p_node,
    Object *p_part
) {
    const int64_t standing = slot_of(&p_record);
    if (standing >= 0) {
        return standing;
    }
    const int64_t slot = open();
    if (slot < 0) {
        return slot;
    }
    Seat seat;
    seat.record = &p_record;
    seat.node = gd::instance_id(p_node);
    seat.part = gd::instance_id(p_part);
    slot_by_record.insert(&p_record, slot);
    seat_by_slot.insert(slot, seat);
    NETW_TRACE(
        sys::PREDICTION,
        "Predicted entity %d was added, %d in all.",
        slot,
        int64_t(slot_by_record.size())
    );
    return slot;
}

int64_t NetwPredictionEngine::slot_of(const NetwEntityRecord *p_record) const {
    const HashMap<const NetwEntityRecord *, int64_t>::ConstIterator seated
        = slot_by_record.find(p_record);
    return seated != slot_by_record.end() ? seated->value : -1;
}

int64_t NetwPredictionEngine::slot_of(const Ref<RefCounted> &p_entity) const {
    const NetwEntity *entity = Object::cast_to<NetwEntity>(p_entity.ptr());
    return entity != nullptr ? slot_of(entity->get_record()) : -1;
}

int64_t NetwPredictionEngine::slot_of(const RID &p_entity) const {
    return session != nullptr && p_entity.is_valid()
        ? slot_of(session->entity_record(p_entity))
        : -1;
}

void NetwPredictionEngine::rekey_member(const RID &p_from, const RID &p_to) {
    for (predict::Slot *row : rows) {
        if (row == nullptr) {
            continue;
        }
        for (predict::EntityRoster &roster : row->rosters) {
            const int at = roster.index_of(p_from);
            if (at >= 0) {
                roster.members[uint32_t(at)] = p_to;
            }
        }
    }
}

void NetwPredictionEngine::slot_unregister(const NetwEntityRecord *p_record) {
    const int64_t slot = slot_of(p_record);
    if (slot >= 0) {
        close(slot);
        unseat(slot);
    }
}

int64_t NetwPredictionEngine::slot_registered() const {
    return int64_t(slot_by_record.size());
}

bool NetwPredictionEngine::slot_bind_owner(
    const NetwEntityRecord *p_record,
    Object *p_owner
) {
    const int64_t slot = slot_of(p_record);
    return slot >= 0 && bind_owner(slot, p_owner);
}

void NetwPredictionEngine::slot_unbind_owner(const NetwEntityRecord *p_record) {
    unbind_owner(slot_of(p_record));
}

bool NetwPredictionEngine::is_open(int64_t p_slot) const {
    return row_of(p_slot) != nullptr;
}

int NetwPredictionEngine::open_count() const {
    int count = 0;
    for (const predict::Slot *row : rows) {
        count += row != nullptr && row->open ? 1 : 0;
    }
    return count;
}

void NetwPredictionEngine::bind_store(
    int64_t p_slot,
    const store::EntityRef &p_ref
) {
    predict::Slot *row = mutable_row_of(p_slot);
    NetwMultiplayer *host = session;
    if (row == nullptr || host == nullptr) {
        return;
    }
    store::Store &world = *store_world;
    if (row->store != p_ref) {
        unbind_store(p_slot);
    }
    if (!world.holds(p_ref)) {
        return;
    }
    row->store = p_ref;
    world.bind_predict(p_ref.slot, p_slot);
}

void NetwPredictionEngine::unbind_store(int64_t p_slot) {
    predict::Slot *row = mutable_row_of(p_slot);
    NetwMultiplayer *host = session;
    if (row == nullptr) {
        return;
    }
    const store::EntityRef held = row->store;
    row->store = store::EntityRef();
    if (host == nullptr) {
        return;
    }
    store::Store &world = *store_world;
    if (world.holds(held) && world.predict_of(held.slot) == p_slot) {
        world.bind_predict(held.slot, -1);
    }
}

store::EntityRef NetwPredictionEngine::store_ref_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? row->store : store::EntityRef();
}

void NetwPredictionEngine::seat_arrivals(
    int64_t p_slot,
    const predict::Feed &p_feed,
    NetwPredictSlotEngine *p_seat
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return;
    }
    row->state_listener = p_feed.state_listener;
    row->input_listener = p_feed.input_listener;
    row->arrival_seat = p_seat;
}

bool NetwPredictionEngine::takes_arrival(
    int64_t p_slot,
    const store::EntityRef &p_ref
) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr && p_ref.bound() && row->store == p_ref;
}

predict::Listener NetwPredictionEngine::listener_for(
    int64_t p_slot,
    const store::EntityRef &p_ref,
    const repl::PropertyBinding *p_binding
) const {
    const predict::Slot *row = row_of(p_slot);
    if (p_binding == nullptr || !takes_arrival(p_slot, p_ref)) {
        return predict::Listener::NONE;
    }
    if (p_binding->id == row->state_binding_id) {
        return row->state_listener;
    }
    if (p_binding->id == row->input_binding_id) {
        return row->input_listener;
    }
    return predict::Listener::NONE;
}

void NetwPredictionEngine::note_state_arrival(
    int64_t p_slot,
    const store::EntityRef &p_ref,
    const repl::PropertyBinding *p_binding,
    bool p_whole
) {
    const predict::Slot *row = row_of(p_slot);
    if (!p_whole || p_binding == nullptr || !takes_arrival(p_slot, p_ref)
        || p_binding->id != row->state_binding_id) {
        return;
    }
    set_stream_reconstructed(p_slot, true);
}

void NetwPredictionEngine::deliver(
    int64_t p_slot,
    predict::Listener p_listener,
    const predict::Arrival &p_arrival
) {
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr || p_listener == predict::Listener::NONE) {
        return;
    }
    NetwPredictSlotEngine *seat = row->arrival_seat;
    if (seat == nullptr) {
        return;
    }
    switch (p_listener) {
        case predict::Listener::STATE:
            seat->on_state_frame(p_arrival);
            break;
        case predict::Listener::INPUT:
            seat->on_input_frame(p_arrival);
            break;
        case predict::Listener::SIMULATED:
            seat->on_simulated_state_frame(p_arrival);
            break;
        case predict::Listener::QUARANTINE:
            seat->on_quarantine_state_frame(p_arrival);
            break;
        default:
            break;
    }
}

bool NetwPredictionEngine::supports(
    int p_schedule,
    int p_role,
    int p_correction,
    int p_restore,
    int p_island
) const {
    const bool scheduled = p_schedule == int(Schedule::TICK)
        || p_schedule == int(Schedule::FRAME)
        || p_schedule == int(Schedule::STEPPED);
    const bool island = p_island >= 0 && p_island <= 2;
    const bool declared
        = p_role >= int(Role::PREDICT) && p_role <= int(Role::SIMULATE);
    const bool role
        = declared && (p_role != int(Role::SIMULATE) || p_island > 0);
    const bool joint = p_island != 2 || rerunnable(p_schedule);
    const bool corrected = p_correction == int(CorrectionMode::SNAP)
        || p_correction == int(CorrectionMode::REPLAY);
    const bool restored = p_restore == int(RestoreMode::EXACT)
        || p_restore == int(RestoreMode::EXTRAPOLATED);
    return scheduled && island && role && joint && corrected && restored;
}

predict::KindDefaults NetwPredictionEngine::slot_kind(int64_t p_slot) const {
    return predict::kind_defaults(
        decl_of(p_slot) != nullptr
        && NetwMultiplayer::sim_physics_driven(
            *owner_record(p_slot),
            owner_node(p_slot)
        )
    );
}

void NetwPredictionEngine::record_authority_witness_class(
    int64_t p_slot,
    int64_t p_transition,
    int p_witness_class
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->authority_witness_classes
            .record(p_transition, p_witness_class, predict::TAPE_HISTORY_LIMIT);
    }
}

int NetwPredictionEngine::authority_witness_class(
    int64_t p_slot,
    int64_t p_transition
) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? row->authority_witness_classes.at(p_transition)
                          : -1;
}

void NetwPredictionEngine::clear_authority_witness_classes(int64_t p_slot) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->authority_witness_classes.clear();
    }
}

predict::ConsumeInputPlan NetwPredictionEngine::plan_consume_input(
    int p_schedule,
    bool p_has_input,
    bool p_has_later_input,
    bool p_has_last_input,
    bool p_repeat_missing_input
) const {
    NETW_BUG_COND_V(
        p_schedule != int(Schedule::TICK) && p_schedule != int(Schedule::FRAME),
        predict::ConsumeInputPlan()
    );
    return predict::plan_consume_input(
        Schedule(p_schedule),
        p_has_input,
        p_has_later_input,
        p_has_last_input,
        p_repeat_missing_input ? MissingInput::REPEAT_LAST : MissingInput::STALL
    );
}

NetwPredictTiming NetwPredictTiming::of(
    int64_t p_tick,
    double p_delta,
    double p_ticktime,
    int64_t p_frame,
    int p_quantum,
    bool p_simulating
) {
    NetwPredictTiming out;
    out.carried.tick = p_tick;
    out.carried.delta = p_delta;
    out.carried.ticktime = p_ticktime;
    out.carried.frame = p_frame;
    out.carried.quantum = p_quantum;
    out.carried.simulating = p_simulating;
    return out;
}

predict::EvidenceRow evidence_row(
    int64_t p_pre_fp,
    int64_t p_c_hash,
    int64_t p_e_digest,
    int64_t p_post_fp,
    int64_t p_pre_pose_fp,
    int64_t p_pre_momentum_fp,
    int64_t p_pre_controller_fp,
    int64_t p_post_pose_fp,
    int64_t p_post_momentum_fp,
    int64_t p_post_controller_fp,
    int64_t p_topo_fp,
    int64_t p_witness_fp,
    int64_t p_raw_fp,
    int p_evidence_mask,
    bool p_complete
) {
    predict::EvidenceRow row;
    row.pre_fp = int32_t(p_pre_fp);
    row.c_hash = int32_t(p_c_hash);
    row.e_digest = int32_t(p_e_digest);
    row.post_fp = int32_t(p_post_fp);
    row.pre_families.pose = int32_t(p_pre_pose_fp);
    row.pre_families.momentum = int32_t(p_pre_momentum_fp);
    row.pre_families.controller = int32_t(p_pre_controller_fp);
    row.post_families.pose = int32_t(p_post_pose_fp);
    row.post_families.momentum = int32_t(p_post_momentum_fp);
    row.post_families.controller = int32_t(p_post_controller_fp);
    row.topo_fp = int32_t(p_topo_fp);
    row.witness_fp = int32_t(p_witness_fp);
    row.raw_fp = int32_t(p_raw_fp);
    row.evidence_mask = uint8_t(p_evidence_mask);
    row.complete = p_complete;
    return row;
}

void NetwPredictionEngine::record_input(
    int64_t p_slot,
    int64_t p_tick,
    int64_t p_c_hash
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->record_input(p_tick, int32_t(p_c_hash));
    }
}

predict::DriveRecord NetwPredictionEngine::open_drive(
    int64_t p_slot,
    const Dictionary &p_topology,
    int64_t p_tick,
    int64_t p_frame,
    double p_ticktime,
    int p_quantum,
    bool p_simulating,
    int64_t p_pre_fp,
    int64_t p_pre_pose_fp,
    int64_t p_pre_momentum_fp,
    int64_t p_pre_controller_fp,
    int64_t p_raw_fp,
    int p_evidence_mask
) {
    predict::DriveRecord out;
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return out;
    }
    predict::Timing timing;
    timing.tick = p_tick;
    timing.frame = p_frame;
    timing.ticktime = p_ticktime;
    timing.quantum = p_quantum;
    timing.simulating = p_simulating;

    predict::StateStamp pre;
    pre.fp = int32_t(p_pre_fp);
    pre.families.pose = int32_t(p_pre_pose_fp);
    pre.families.momentum = int32_t(p_pre_momentum_fp);
    pre.families.controller = int32_t(p_pre_controller_fp);
    pre.raw_fp = int32_t(p_raw_fp);
    pre.evidence_mask = uint8_t(p_evidence_mask);

    return row->open_drive(timing, p_topology, pre);
}

predict::DriveRecord NetwPredictionEngine::replay_drive(
    int64_t p_slot,
    const Dictionary &p_topology,
    int64_t p_transition,
    int64_t p_label,
    int p_kind,
    int64_t p_tick,
    int64_t p_frame,
    double p_ticktime,
    int p_quantum,
    int64_t p_pre_fp,
    int64_t p_pre_pose_fp,
    int64_t p_pre_momentum_fp,
    int64_t p_pre_controller_fp,
    int64_t p_raw_fp,
    int p_evidence_mask,
    bool p_authoring
) {
    predict::DriveRecord out;
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return out;
    }
    predict::Timing timing;
    timing.tick = p_tick;
    timing.frame = p_frame;
    timing.ticktime = p_ticktime;
    timing.quantum = p_quantum;

    predict::StateStamp pre;
    pre.fp = int32_t(p_pre_fp);
    pre.families.pose = int32_t(p_pre_pose_fp);
    pre.families.momentum = int32_t(p_pre_momentum_fp);
    pre.families.controller = int32_t(p_pre_controller_fp);
    pre.raw_fp = int32_t(p_raw_fp);
    pre.evidence_mask = uint8_t(p_evidence_mask);

    return row->replay_drive(
        timing,
        p_topology,
        p_transition,
        p_label,
        DriveKind(p_kind),
        pre,
        p_authoring
    );
}

bool NetwPredictionEngine::record_evidence(
    int64_t p_slot,
    int64_t p_transition,
    int64_t p_environment_epoch,
    const Dictionary &p_environment,
    const Dictionary &p_topology,
    const PackedStringArray &p_contact_ids,
    const PackedInt32Array &p_witness_classes,
    const PackedInt32Array &p_realizations,
    const PackedByteArray &p_outside_boundary,
    bool p_sleeping
) {
    predict::Slot *row = mutable_row_of(p_slot);
    NETW_BUG_COND_V(row == nullptr, false);
    const int count = p_contact_ids.size();
    NETW_BUG_COND_V(
        p_witness_classes.size() != count || p_realizations.size() != count
            || p_outside_boundary.size() != count,
        false
    );
    LocalVector<predict::WitnessContact> contacts;
    for (int at = 0; at < count; ++at) {
        predict::WitnessContact contact;
        contact.identity = p_contact_ids[at];
        contact.witness_class = p_witness_classes[at];
        contact.realization = p_realizations[at];
        contact.outside_boundary = p_outside_boundary[at] != 0;
        contacts.push_back(contact);
    }
    int rejected_contact = -1;
    const bool recorded = row->record_evidence(
        p_transition,
        p_environment_epoch,
        p_environment,
        p_topology,
        contacts,
        p_sleeping,
        &rejected_contact
    );
    NETW_BUG_COND_V(rejected_contact >= 0, false);
    return recorded;
}

bool NetwPredictionEngine::recovery_replays(int64_t p_slot) const {
    const predict::Decl *decl = decl_of(p_slot);
    if (decl == nullptr) {
        return true;
    }
    const int declared = predict::correction_for_mode(decl->mode);
    return resolve_correction(p_slot, declared) == int(CorrectionMode::REPLAY);
}

bool NetwPredictionEngine::demote_for_breach(
    int64_t p_slot,
    int64_t p_transition,
    bool p_breached,
    int p_witness_fingerprint,
    NetwPredictSlotEngine &p_engine
) {
    NETW_ZONE_NC("Predict demote for breach", colors::PREDICTION);
    const predict::Decl *decl = decl_of(p_slot);
    if (!p_breached || decl == nullptr
        || role_of(p_slot) != NetwPrediction::ROLE_PREDICT
        || fallback_latched_of(p_slot) || !decl->pause_on_unpredicted_contact
        || !samples_contacts(p_slot)) {
        return false;
    }
    if (journal_has(p_slot, p_transition)) {
        mark_domain(p_slot, p_transition, int(predict::Domain::OUT_OF_DOMAIN));
    }
    const bool opened
        = episode_state(p_slot) != NetwPrediction::EPISODE_STATE_OPEN;
    record_breach(p_slot, p_transition);
    if (opened) {
        sync_episode(p_slot);
        announce_episode(p_slot, EventPlane::EPISODE_OPEN);
    }
    record_episode_write(
        p_slot,
        int(predict::Operator::DEMOTE),
        p_transition,
        p_witness_fingerprint,
        owner_name(p_slot),
        decl->ack_age_ticks,
        trigger_shape(p_slot, decl->correction_threshold),
        true,
        false
    );
    sync_episode(p_slot);
    send_command_frame(p_slot);
    p_engine
        .enter_fallback(p_transition, int(predict::Attribution::CONTACT), true);
    return true;
}

void NetwPredictionEngine::record_breach(int64_t p_slot, int64_t p_transition) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return;
    }
    predict::Episode &episode = row->episode;
    if (!episode.active || episode.state != predict::EpisodeState::OPEN) {
        episode.open(row->journal, p_transition, predict::Attribution::CONTACT);
    } else {
        episode.repin_generator(
            row->journal,
            p_transition,
            predict::Attribution::CONTACT
        );
    }
    episode.record_breach(p_transition);
}

bool NetwPredictionEngine::witness_row_clean(
    int64_t p_slot,
    int64_t p_transition,
    bool p_require_peer
) const {
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr) {
        return false;
    }
    const int index = row->journal.index_of(p_transition);
    if (index < 0) {
        return false;
    }
    const uint8_t evidence = row->journal.evidence_mask_at(index);
    if ((evidence & predict::EVIDENCE_WITNESS) == 0) {
        return false;
    }
    const uint8_t flags = row->journal.flags_at(index);
    if (p_require_peer && (flags & predict::ROW_WITNESS_MATCHED) == 0) {
        return false;
    }
    const uint8_t state = row->journal.witness_state_at(index);
    if ((state & (predict::WITNESS_SLEEPING | predict::WITNESS_WOKE)) != 0) {
        return false;
    }
    const uint8_t realized = row->journal.witness_realization_bits_at(index);
    if (realized == 0) {
        return false;
    }
    constexpr uint8_t REPRODUCIBLE
        = uint8_t(1u << int(predict::ContactClass::NONE))
        | uint8_t(1u << int(predict::ContactClass::DECLARED_SUPPORT))
        | uint8_t(1u << int(predict::ContactClass::OTHER_STATIC));
    return (realized & uint8_t(~REPRODUCIBLE)) == 0;
}

bool NetwPredictionEngine::transport_admissible(
    int64_t p_slot,
    bool p_candidate,
    bool p_basis_witness_clean,
    bool p_recent_witness_clean,
    bool p_non_pose_agrees,
    bool p_below_teleport,
    bool p_escalated,
    bool p_observing
) const {
    const predict::Slot *row = row_of(p_slot);
    NETW_BUG_COND_V(row == nullptr, false);
    predict::TransportEvidence evidence;
    evidence.candidate = p_candidate;
    evidence.basis_witness_clean = p_basis_witness_clean;
    evidence.recent_witness_clean = p_recent_witness_clean;
    evidence.non_pose_agrees = p_non_pose_agrees;
    evidence.below_teleport = p_below_teleport;
    evidence.escalated = p_escalated;
    evidence.snap_correction
        = row->config.correction == int(CorrectionMode::SNAP);
    evidence.observing = p_observing;
    return predict::transport_admissible(evidence);
}

bool NetwPredictionEngine::dissipate_admissible(
    int64_t p_slot,
    bool p_momentum_active,
    bool p_other_active,
    bool p_basis_witness_clean,
    bool p_recent_witness_clean,
    bool p_escalated,
    bool p_observing,
    int p_meter
) const {
    const predict::Slot *row = row_of(p_slot);
    NETW_BUG_COND_V(row == nullptr, false);
    predict::DissipateEvidence evidence;
    evidence.momentum_active = p_momentum_active;
    evidence.other_active = p_other_active;
    evidence.basis_witness_clean = p_basis_witness_clean;
    evidence.recent_witness_clean = p_recent_witness_clean;
    evidence.escalated = p_escalated;
    evidence.snap_correction
        = row->config.correction == int(CorrectionMode::SNAP);
    evidence.observing = p_observing;
    evidence.meter = p_meter;
    return predict::dissipate_admissible(evidence);
}

int NetwPredictionEngine::try_dissipate(
    int64_t p_slot,
    int64_t p_basis,
    int p_meter,
    int p_domain,
    bool p_escalated,
    double p_correction_threshold,
    bool p_observing,
    const StringName &p_target,
    int p_ack_age_ticks
) {
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr) {
        return DISSIPATE_UNDECIDED;
    }
    const PackedInt64Array stats = episode_stats(p_slot);
    if (stats[STAT_EPISODE_ACTIVE] == 0
        || stats[STAT_EPISODE_DISSIPATE_DECIDED] != 0
        || !dissipate_declared(p_slot)) {
        return DISSIPATE_UNDECIDED;
    }
    const Dictionary judged = dissipate_fields(
        p_slot,
        divergence_report(p_slot),
        p_domain,
        p_correction_threshold
    );
    const bool momentum_active = judged[StringName("momentum_active")];
    const bool other_active = judged[StringName("other_active")];
    const bool basis_clean = witness_row_clean(p_slot, p_basis, true);
    const bool recent_clean = recent_witness_clean(p_slot, p_basis);
    const bool eligible = dissipate_admissible(
        p_slot,
        momentum_active,
        other_active,
        basis_clean,
        recent_clean,
        p_escalated,
        p_observing,
        p_meter
    );
    Dictionary eligibility;
    eligibility[StringName("eligible")] = eligible;
    eligibility[StringName("momentum_active")] = momentum_active;
    eligibility[StringName("other_active")] = other_active;
    eligibility[StringName("basis_witness_clean")] = basis_clean;
    eligibility[StringName("recent_witness_clean")] = recent_clean;
    eligibility[StringName("mechanism")]
        = !p_observing && row->config.correction == int(CorrectionMode::SNAP);
    eligibility[StringName("fields")] = judged[StringName("fields")];
    record_episode_decision(
        p_slot,
        int(predict::Operator::DISSIPATE),
        p_basis,
        eligible,
        eligible,
        eligibility
    );
    if (eligible) {
        record_episode_write(
            p_slot,
            int(predict::Operator::DISSIPATE),
            p_basis,
            0,
            p_target,
            p_ack_age_ticks,
            trigger_shape(p_slot, p_correction_threshold),
            false,
            true
        );
    }
    return eligible ? DISSIPATE_APPLIED : DISSIPATE_DECLINED;
}

bool NetwPredictionEngine::dissipate_declared(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr) {
        return false;
    }
    for (uint32_t at = 0; at < row->wiring.withheld.size(); ++at) {
        if (row->wiring.withheld[at] != 0) {
            return true;
        }
    }
    return false;
}

bool NetwPredictionEngine::static_geometry(Object *p_collider) const {
    return predict::static_geometry(p_collider);
}

int NetwPredictionEngine::witness_class(
    Object *p_collider,
    bool p_declared_support
) const {
    return predict::witness_class(p_collider, p_declared_support);
}

RID NetwPredictionEngine::contact_entity(Object *p_collider) const {
    Node *node = Object::cast_to<Node>(p_collider);
    if (node == nullptr) {
        return RID();
    }
    const Ref<NetwEntity> entity = NetwEntity::of(node);
    if (entity.is_null() || entity->get_declares_scene()) {
        return RID();
    }
    return entity->get_rid_handle();
}

String NetwPredictionEngine::collider_identity(Object *p_collider) const {
    Node *node = Object::cast_to<Node>(p_collider);
    if (static_geometry(p_collider)) {
        return String("path:") + String(node->get_path());
    }
    const RID entity = contact_entity(p_collider);
    if (entity.is_valid()) {
        return String("entity:") + participant_key(entity);
    }
    if (node != nullptr) {
        return String("path:") + String(node->get_path());
    }
    return String("object:")
        + (p_collider != nullptr ? p_collider->get_class() : String());
}

int NetwPredictionEngine::classify_collider(
    Object *p_collider,
    const String &p_support
) const {
    if (p_collider == nullptr) {
        return int(NetwPrediction::CONTACT_CLASS_UNPREDICTED_DYNAMIC);
    }
    if (!p_support.is_empty() && collider_identity(p_collider) == p_support) {
        return int(NetwPrediction::CONTACT_CLASS_DECLARED_SUPPORT);
    }
    if (p_collider->is_class("AnimatableBody2D")
        || p_collider->is_class("AnimatableBody3D")
        || p_collider->is_class("CharacterBody2D")
        || p_collider->is_class("CharacterBody3D")) {
        return int(NetwPrediction::CONTACT_CLASS_KINEMATIC_PROXY);
    }
    if (static_geometry(p_collider)) {
        return int(NetwPrediction::CONTACT_CLASS_OTHER_STATIC);
    }
    const bool rigid = p_collider->is_class("RigidBody2D")
        || p_collider->is_class("RigidBody3D");
    if (rigid && bool(p_collider->get(StringName("freeze")))) {
        return int(NetwPrediction::CONTACT_CLASS_KINEMATIC_PROXY);
    }
    if (slot_of(contact_entity(p_collider)) >= 0) {
        return int(NetwPrediction::CONTACT_CLASS_PREDICTED_DYNAMIC);
    }
    return int(NetwPrediction::CONTACT_CLASS_UNPREDICTED_DYNAMIC);
}

void NetwPredictionEngine::relay_ran_input(
    int64_t p_slot,
    int64_t p_world_tick,
    const predict::StateRow &p_command
) {
    predict::Slot *row = mutable_row_of(p_slot);
    NetwMultiplayer *host = session;
    if (row == nullptr || host == nullptr || !host->is_host()
        || p_world_tick < 0) {
        return;
    }
    row->relay_window.pace(uint32_t(clock_engine->get_send_interval()));
    row->relay_window.record(p_world_tick, p_command);
    const NetwEntityRecord *seated = owner_record(p_slot);
    if (seated == nullptr || !clock_engine->is_send_tick()) {
        return;
    }
    const int64_t relayed = int64_t(seated->get_handle().get_id());
    if (host->relay_peer_count(relayed) == 0) {
        return;
    }
    row->relay_window.follow(host->relay_generation(relayed));
    const int refresh = CLAMP(
        clock_engine->get_tickrate(),
        1,
        predict::RELAY_REFRESH_TICKS_MAX
    );
    if (!row->relay_window.due(uint32_t(refresh))) {
        return;
    }
    const PackedByteArray frame = build_relay_frame(p_slot);
    if (frame.is_empty()) {
        return;
    }
    host->predict_relay_command_frame(
        seated->get_handle(),
        frame,
        seated->get_control()->resolve(),
        row->relay_window.refreshes(uint32_t(refresh)) ? wire::Delivery::WAIT
                                                       : wire::Delivery::TICK
    );
}

int NetwPredictionEngine::command_window(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr || input_binding_in(*row) == nullptr
        || input_binding_in(*row)->get_set() == nullptr) {
        return 1;
    }
    return int(
        MAX(int64_t(1), input_binding_in(*row)->get_set()->get_window() + 2)
        * int64_t(clock_engine->get_send_interval())
    );
}

bool NetwPredictionEngine::sent_last_tick() const {
    return clock_engine->sent_last_tick();
}

int NetwPredictionEngine::input_buffer(int p_buffer_ticks) const {
    return std::max(0, p_buffer_ticks) + clock_engine->get_send_interval() - 1;
}

int64_t NetwPredictionEngine::queued_span(int64_t p_slot) const {
    if (!input_lane_open(p_slot)) {
        return 0;
    }
    return newest_authored_input(p_slot) - next_input_tick_of(p_slot) + 1;
}

void NetwPredictionEngine::resync_input_if_stranded(
    int64_t p_slot,
    int64_t p_ceiling,
    int64_t p_buffer
) {
    if (p_ceiling <= 0 || queued_span(p_slot) <= p_ceiling) {
        return;
    }
    if (!input_lane_open(p_slot)) {
        return;
    }
    const int64_t standing = next_input_tick_of(p_slot);
    const int64_t target = newest_authored_input(p_slot) - p_buffer;
    if (target <= standing) {
        return;
    }
    if (session != nullptr) {
        for (int64_t transition = MAX(standing, target - ACK_WINDOW_MAX);
             transition < target;
             ++transition) {
            declare_skipped(p_slot, transition, transition);
        }
    }
    add_held_fact(
        p_slot,
        NetwPredictStats::FACT_SKIPPED,
        MAX(int64_t(0), target - standing)
    );
    add_held_fact(p_slot, NetwPredictStats::FACT_RESYNC, 1);
    set_next_input_tick(p_slot, target);
}

void NetwPredictionEngine::resync_tape_if_stranded(
    int64_t p_slot,
    int64_t p_depth,
    int64_t p_buffer,
    int64_t p_ceiling
) {
    if (p_ceiling <= 0 || p_depth <= p_buffer + p_ceiling) {
        return;
    }
    const int64_t standing = replay_cursor_of(p_slot);
    const int64_t target = standing + p_depth - p_buffer - 1;
    if (target <= standing) {
        return;
    }
    declare_skipped_run(p_slot, standing, target);
    add_held_fact(p_slot, NetwPredictStats::FACT_SKIPPED, target - standing);
    add_held_fact(p_slot, NetwPredictStats::FACT_RESYNC, 1);
    set_replay_cursor(p_slot, target);
}

namespace {

int64_t missing_label(const predict::Slot &p_row, int64_t p_hole) {
    int64_t after = p_hole + 1;
    while (!p_row.holds_command(after)) {
        after += 1;
    }
    const int64_t next = p_row.inputs.label_of(after);
    if (!p_row.holds_command(p_hole - 1)) {
        return next;
    }
    return std::min(p_row.inputs.label_of(p_hole - 1) + 1, next);
}

void stall_command(const predict::Slot &p_row, predict::StateRow &r_command) {
    predict::zero_state_row(p_row.input_codec, r_command);
    const int declared = std::min(
        int(p_row.stall.values.size()),
        int(r_command.values.size())
    );
    for (int field = 0; field < declared; ++field) {
        if (p_row.stall.has(field)) {
            r_command.set(field, p_row.stall.values[uint32_t(field)]);
        }
    }
}

} // namespace

void NetwPredictionEngine::admit_missing_commands(
    int64_t p_slot,
    int p_buffer
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return;
    }
    const int64_t cursor = row->lane_cursor.replay_cursor;
    const int64_t newest = row->newest_authored();
    if (cursor < 0 || newest <= cursor) {
        return;
    }
    const predict::StateRow missing;
    int64_t hole = cursor + row->command_depth_from(cursor);
    while (hole < newest && hole - cursor <= p_buffer) {
        if (!row->admit_command(
                hole,
                missing_label(*row, hole),
                true,
                missing
            )) {
            return;
        }
        hole += row->command_depth_from(hole);
    }
}

Dictionary NetwPredictionEngine::capture_state_raw(int64_t p_slot) {
    if (session != nullptr && owner_bound(p_slot)) {
        return capture_state(p_slot);
    }
    const PropertyBindingPtr binding = state_binding_of(p_slot);
    return binding != nullptr ? binding->snapshot_payload() : Dictionary();
}

Dictionary NetwPredictionEngine::canonical_state(
    int64_t p_slot,
    const Dictionary &p_payload
) {
    if (session != nullptr) {
        return p_slot >= 0 ? canonicalize_state(p_slot, p_payload)
                           : p_payload.duplicate();
    }
    const PropertyBindingPtr binding = state_binding_of(p_slot);
    return binding != nullptr ? binding->canonicalize_payload(p_payload)
                              : Dictionary();
}

Dictionary NetwPredictionEngine::capture_canonical_state(int64_t p_slot) {
    NETW_ZONE_LEAF(
        "Predict capture canonical state",
        colors::PREDICTION,
        profile::SUBSYSTEM_PREDICTION
    );
    return canonical_state(p_slot, capture_state_raw(p_slot));
}

void NetwPredictionEngine::record_command_input(
    int64_t p_slot,
    int64_t p_tick,
    const predict::StateRow &p_command
) {
    if (session == nullptr) {
        return;
    }
    record_input(p_slot, p_tick, command_hash_row(p_slot, p_command));
}

void NetwPredictionEngine::run_replay_step(
    int64_t p_slot,
    const predict::StateRow &p_command,
    double p_delta,
    int64_t p_tick,
    bool p_fresh
) {
    if (session != nullptr && owner_bound(p_slot)) {
        run_command(p_slot, p_command, p_delta, p_tick, p_fresh);
        return;
    }
    apply_command(p_slot, p_command);
    const NetwEntityRecord *record = owner_record(p_slot);
    if (record == nullptr) {
        return;
    }
    const Callable step
        = NetwMultiplayer::sim_step_of(*record, owner_node(p_slot));
    if (step.is_valid()) {
        call_step(step, p_delta, p_tick, p_fresh);
    }
}

bool NetwPredictionEngine::replay_tape_entry(
    int64_t p_slot,
    double p_delta,
    int64_t p_tick
) {
    const int64_t cursor = replay_cursor_of(p_slot);
    if (session == nullptr || !command_has(p_slot, cursor)) {
        return false;
    }
    const int64_t label = command_label_of(p_slot, cursor);
    const bool fresh = command_is_fresh(p_slot, cursor);
    predict::Slot *row = mutable_row_of(p_slot);
    int kind = int(DriveKind::REPEAT);
    bool applied_fresh = false;
    if (fresh) {
        if (row->inputs.read_row(row->input_codec, cursor, row->command)
            && row->command.any()) {
            row->last = row->command;
            kind = int(DriveKind::FRESH);
            applied_fresh = true;
        } else {
            add_held_fact(p_slot, NetwPredictStats::FACT_MISSING, 1);
            kind = int(DriveKind::MISSING);
        }
    }
    if (kind == int(DriveKind::MISSING)) {
        const predict::Decl *decl = decl_of(p_slot);
        const predict::ConsumeInputPlan plan = predict::plan_consume_input(
            Schedule(row->config.schedule),
            false,
            true,
            row->last.any(),
            decl != nullptr && decl->repeat_missing_input
                ? MissingInput::REPEAT_LAST
                : MissingInput::STALL
        );
        if (plan.use_last) {
            row->command = row->last;
        } else {
            stall_command(*row, row->command);
        }
    } else if (!applied_fresh) {
        row->command = row->last;
    }
    if (!row->command.any()) {
        row->command = row->stall;
    }
    record_drive_hashed(
        p_slot,
        cursor,
        label,
        kind,
        command_hash_row(p_slot, row->command),
        p_tick,
        true
    );
    row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return false;
    }
    run_replay_step(p_slot, row->command, p_delta, label, applied_fresh);
    set_ack(p_slot, cursor);
    set_replay_cursor(p_slot, cursor + 1);
    set_last_replayed_label(p_slot, label);
    set_last_replayed_fresh(p_slot, fresh);
    add_held_fact(p_slot, NetwPredictStats::FACT_CONSUMED, 1);
    if (const predict::Slot *ran = row_of(p_slot)) {
        relay_ran_input(p_slot, p_tick, ran->command);
    }
    return true;
}

void NetwPredictionEngine::prepare_tick_tape(int64_t p_slot, int64_t p_tick) {
    if (session == nullptr) {
        return;
    }
    const PackedInt64Array span = tape_span(p_slot);
    const int64_t newest = span.size() > 1 ? span[1] : -1;
    tape_prepare_tick(p_slot, p_tick);
    if (newest >= 0 && newest != p_tick - 1) {
        clear_witness_details(p_slot);
        drop_deferred_operator(p_slot);
        set_ack_of_acks(p_slot, -1);
        set_ack_domain_confirmed(p_slot, false);
    }
}

void NetwPredictionEngine::refresh_simulation_gate(
    int64_t p_slot,
    bool p_force_release
) {
    NetwMultiplayer *host = session;
    const NetwEntityRecord *seated = owner_record(p_slot);
    if (host == nullptr || seated == nullptr) {
        return;
    }
    const RID entity = seated->get_handle();
    const sim::Row *row = sim_rows->row_of(entity);
    const bool leads = row != nullptr && sim::leads(row->mode);
    host->simulation_gate_set(
        entity,
        !p_force_release && leads && slot_kind(p_slot).gates_physics
    );
}

void NetwPredictionEngine::host_local_step(
    int64_t p_slot,
    double p_delta,
    int64_t p_tick
) {
    if (row_of(p_slot) == nullptr) {
        return;
    }
    capture_command(p_slot);
    record_authored_command(p_slot, p_tick, p_tick, true, *command_of(p_slot));
    record_drive_hashed(
        p_slot,
        p_tick,
        p_tick,
        int(DriveKind::FRESH),
        command_hash_row(p_slot, *command_of(p_slot)),
        p_tick,
        false
    );
    if (row_of(p_slot) == nullptr) {
        return;
    }
    run_replay_step(p_slot, *command_of(p_slot), p_delta, p_tick, true);
    const PropertyBindingPtr binding = state_binding_of(p_slot);
    if (binding != nullptr) {
        binding->set_authored_tick(p_tick);
        binding->set_reconcile_ack(p_tick);
    }
    if (const predict::StateRow *ran = command_of(p_slot)) {
        relay_ran_input(p_slot, p_tick, *ran);
    }
}

bool NetwPredictionEngine::slot_has_stepper(int64_t p_slot) const {
    NetwMultiplayer *host = session;
    if (host == nullptr || owner_record(p_slot) == nullptr) {
        return false;
    }
    const NetwMultiplayer::EntitySpace held
        = host->entity_space_of(owner_node(p_slot));
    return host->predict_space_can_step(held.space, held.dimension);
}

bool NetwPredictionEngine::slot_steps(int64_t p_slot) const {
    NetwMultiplayer *host = session;
    return host != nullptr && owner_record(p_slot) != nullptr
        && host->predict_entity_can_step(owner_node(p_slot));
}

predict::ReplayPlan NetwPredictionEngine::solo_plan(
    const Dictionary &p_sample,
    const PackedStringArray &p_participants,
    NetwPredictSlotEngine *p_demote
) {
    predict::ReplayPlan plan;
    plan.sample = p_sample;
    plan.participants = p_participants;
    plan.flags = p_demote != nullptr ? predict::REPLAY_DEMOTES : 0;
    return plan;
}

void NetwPredictionEngine::replay_ticks(
    int64_t p_slot,
    const LocalVector<int64_t> &p_ticks,
    uint8_t p_step_flags,
    const Dictionary &p_sample,
    const PackedStringArray &p_participants,
    NetwPredictSlotEngine *p_demote
) {
    predict::ReplayPlan plan = solo_plan(p_sample, p_participants, p_demote);
    plan.floor = p_ticks.is_empty() ? -1 : p_ticks[0] - 1;
    for (const int64_t tick : p_ticks) {
        predict::ReplayStep step;
        step.slot = p_slot;
        step.tick = tick;
        step.entry = tick;
        step.input_key = tick;
        step.flags = p_step_flags;
        plan.steps.push_back(step);
    }
    run_solo_replay(p_slot, plan);
}

void NetwPredictionEngine::run_solo_replay(
    int64_t p_slot,
    const predict::ReplayPlan &p_plan
) {
    NetwPredictSlotEngine *seat = session != nullptr
        ? session->predict_engine_for(owner_rid(p_slot))
        : nullptr;
    NETW_BUG_COND(seat == nullptr || seat->native_slot() != p_slot);
    LocalVector<NetwPredictSlotEngine *> members;
    members.push_back(seat);
    seat->run_replay(p_plan, members);
}

int NetwPredictionEngine::resolved_schedule(int64_t p_slot, int p_declared) {
    if (p_declared != int(NetwSimulation::SCHEDULE_STEPPED)) {
        return p_declared;
    }
    return slot_steps(p_slot) ? p_declared
                              : int(NetwSimulation::SCHEDULE_FRAME);
}

void NetwPredictionEngine::report_stepper_absence(int64_t p_slot) {
    NetwMultiplayer *host = session;
    Node *owner = owner_node(p_slot);
    if (host == nullptr || owner == nullptr) {
        return;
    }
    if (slot_has_stepper(p_slot)) {
        Node *kinematic = colliders::first_kinematic_body(owner);
        if (kinematic == nullptr
            || !colliders::first_report(
                kinematic,
                colliders::Fault::STEPPED_KINEMATIC
            )) {
            return;
        }
        fault::raise(
            session,
            NETW_FAULT_ENGINE_LACKS,
            {String(kinematic->get_name()),
             String(),
             String(kinematic->get_class()),
             0,
             NETW_LACKS_REPLAY_RESTORE},
            gd::instance_id(kinematic)
        );
        return;
    }
    if (!colliders::first_report(owner, colliders::Fault::STEPPED_NO_STEP)) {
        return;
    }
    fault::raise(
        session,
        NETW_FAULT_CANNOT_STEP_PHYSICS,
        {String(owner->get_name())},
        gd::instance_id(owner)
    );
}

bool NetwPredictionEngine::slot_is_steppable(int64_t p_slot) const {
    const NetwEntityRecord *record = owner_record(p_slot);
    Node *owner = owner_node(p_slot);
    const predict::Decl *decl = decl_of(p_slot);
    if (record == nullptr || decl == nullptr
        || decl->sim_mode == NetwPrediction::SIM_MODE_DISPLAY
        || !NetwMultiplayer::sim_step_of(*record, owner).is_valid()) {
        return false;
    }
    const int schedule = NetwMultiplayer::predict_schedule_of(*record, owner);
    if (schedule == NetwSimulation::SCHEDULE_TICK) {
        return true;
    }
    return schedule == NetwSimulation::SCHEDULE_STEPPED && slot_steps(p_slot);
}

void NetwPredictionEngine::refresh_tape_diagnostics(int64_t p_slot) {
    const bool live = session != nullptr;
    const PackedInt64Array held
        = live ? command_transitions(p_slot) : PackedInt64Array();
    set_held_fact(
        p_slot,
        NetwPredictStats::FACT_TAPE_EPOCH,
        command_epoch_of(p_slot)
    );
    set_held_fact(
        p_slot,
        NetwPredictStats::FACT_TAPE_QUEUE_DEPTH,
        live ? command_depth_from(p_slot, replay_cursor_of(p_slot)) : 0
    );
    set_held_fact(
        p_slot,
        NetwPredictStats::FACT_TAPE_INDEX,
        held.is_empty() ? -1 : held[held.size() - 1]
    );
}

int64_t NetwPredictionEngine::refresh_owner_ack_age(int64_t p_slot) {
    if (session == nullptr || p_slot < 0) {
        return -1;
    }
    refresh_ack_age(p_slot);
    const int64_t age = drive_stat(p_slot, STAT_ACK_AGE_TICKS);
    predict::Decl *decl = decl_of(p_slot);
    if (age >= 0 && decl != nullptr) {
        decl->ack_age_ticks = int(age);
    }
    return age;
}

bool NetwPredictionEngine::speculation_horizon_full(int64_t p_slot) {
    refresh_owner_ack_age(p_slot);
    const predict::Decl *decl = decl_of(p_slot);
    return decl != nullptr && decl->ack_age_ticks >= predict::ACK_AGE_MAX;
}

namespace {

int64_t input_hold_ticks(int p_tickrate) {
    return CLAMP(
        int64_t(p_tickrate) * predict::HOLD_SECONDS,
        int64_t(1),
        predict::COMMAND_HOLD_MAX
    );
}

} // namespace

int64_t NetwPredictionEngine::quiet_hold(
    int64_t p_slot,
    int64_t p_newest,
    bool p_asks
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return 0;
    }
    predict::LaneCursor &lane = row->lane_cursor;
    lane.command_rides = false;
    if (lane.quiet_epoch != row->tape_epoch) {
        lane.quiet_epoch = row->tape_epoch;
        lane.input_changed_at = p_newest;
        lane.input_confirmed = -1;
        lane.quiet_since = -1;
        lane.hold_sent_at = -1;
        lane.tolerated_at = -1;
    }
    const predict::Journal &journal = row->journal;
    const int64_t closed = journal.last_closed();
    const bool stands = row->config.role == int(Role::PREDICT)
        && row->config.schedule == int(Schedule::TICK) && row->lane_open
        && !p_asks && lane.ack_domain_confirmed
        && lane.input_changed_at < p_newest
        && lane.input_confirmed >= lane.input_changed_at
        && row->stats.ack_age_ticks < predict::ACK_AGE_MAX * 3 / 4
        && closed >= p_newest - 1 && journal.slot_of(closed - 1) >= 0
        && journal.post_fp_of(closed) == journal.post_fp_of(closed - 1);
    if (!stands) {
        lane.quiet_since = -1;
        return 0;
    }
    if (lane.quiet_since < 0) {
        lane.quiet_since = p_newest;
    }
    const int64_t hold = input_hold_ticks(clock_engine->get_tickrate());
    const bool hands_off = p_newest - lane.quiet_since
        < predict::HOLD_HANDOFF_SENDS * clock_engine->get_send_interval();
    const int64_t renew = CLAMP(
        hold / predict::HOLD_SECONDS,
        int64_t(1),
        int64_t(predict::HOLD_RENEW_TICKS_MAX)
    );
    const NetwMultiplayer *host = session;
    const bool joins_ping = host != nullptr && host->clock_ping_waits();
    if (!hands_off && p_newest - lane.hold_sent_at < renew && !joins_ping) {
        return -1;
    }
    lane.command_rides = !hands_off;
    return hold;
}

void NetwPredictionEngine::hear_input(
    int64_t p_slot,
    int64_t p_newest,
    int64_t p_hold
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr || p_newest <= row->lane_cursor.input_heard) {
        return;
    }
    predict::LaneCursor &lane = row->lane_cursor;
    lane.input_heard = p_newest;
    lane.input_hold_through = p_newest
        + std::min(p_hold, input_hold_ticks(clock_engine->get_tickrate()));
    if (p_hold > 0) {
        lane.input_stood_through = p_newest;
    }
    if (p_newest >= lane.next_input_tick || !row->last.any()
        || !row->holds_command(p_newest)) {
        return;
    }
    predict::StateRow late;
    if (!row->inputs.read_row(row->input_codec, p_newest, late)
        || predict::same_command(late, row->last)) {
        return;
    }
    row->last = late;
    lane.input_hold_through = lane.next_input_tick - 1;
}

void NetwPredictionEngine::judge_held_guess(
    int64_t p_slot,
    int64_t p_transition
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return;
    }
    const predict::Journal &journal = row->journal;
    const int at = journal.index_of(p_transition);
    predict::StateRow late;
    if (at < 0
        || (journal.witness_state_at(at) & predict::WITNESS_INPUT_HELD) == 0
        || (journal.flags_at(at) & predict::ROW_SUPERSEDED) != 0
        || !row->inputs.read_row(row->input_codec, p_transition, late)
        || int32_t(command_hash_row(p_slot, late)) == journal.c_hash_at(at)) {
        return;
    }
    row = mutable_row_of(p_slot);
    row->journal.mark_superseded(p_transition);
    predict::LaneCursor &lane = row->lane_cursor;
    lane.guess_wrong_at = lane.guess_sends > 0
        ? std::min(lane.guess_wrong_at, p_transition)
        : p_transition;
    lane.guess_sends = predict::ACK_NEWS_SENDS;
}

bool NetwPredictionEngine::holds_input(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr || !row->lane_open || !row->last.any()) {
        return false;
    }
    const int64_t next = row->lane_cursor.next_input_tick;
    return next >= 0 && next <= row->lane_cursor.input_hold_through
        && row->newest_authored() < next;
}

int64_t NetwPredictionEngine::drive_frontier(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr) {
        return -1;
    }
    return row->config.schedule == int(Schedule::FRAME)
        ? row->last_driven_entry_index
        : row->cursor.latest_input_tick;
}

namespace {

const StringName &signal_corrected() {
    static const StringName name("corrected");
    return name;
}

void blank_row(predict::StateRow &r_row, int p_width) {
    r_row.resize(p_width);
    for (uint32_t at = 0; at < r_row.values.size(); ++at) {
        r_row.values[at] = Variant();
    }
}

bool delta_negligible(const Variant &p_delta) {
    switch (p_delta.get_type()) {
        case Variant::FLOAT:
            return std::abs(double(p_delta)) < 0.000001;
        case Variant::VECTOR2:
            return Vector2(p_delta).length() < 0.000001;
        case Variant::VECTOR3:
            return Vector3(p_delta).length() < 0.000001;
        default:
            return true;
    }
}

} // namespace

int NetwPredictionEngine::replay_reseed_horizon(
    int64_t p_slot,
    int64_t p_ack,
    const Dictionary &p_sample,
    const PackedStringArray &p_participants,
    NetwPredictSlotEngine *p_demote
) {
    NETW_STAGE(ROLLBACK, "Predict replay horizon", colors::PREDICTION);
    NetwPrediction *handle = Object::cast_to<NetwPrediction>(handle_of(p_slot));
    if (handle == nullptr) {
        return 0;
    }
    if (NetwMultiplayer::predict_schedule_of(
            *owner_record(p_slot),
            owner_node(p_slot)
        )
        == NetwSimulation::SCHEDULE_FRAME) {
        return replay_authored_entries(
            p_slot,
            p_ack,
            p_sample,
            p_participants,
            p_demote
        );
    }
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr || !row->lane_open
        || input_binding_of(p_slot) == nullptr) {
        return 0;
    }
    LocalVector<int64_t> ticks;
    row->authored_ticks(p_ack + 1, latest_input_tick_of(p_slot), ticks);
    replay_ticks(
        p_slot,
        ticks,
        predict::STEP_READS_INPUT,
        p_sample,
        p_participants,
        p_demote
    );
    return int(ticks.size());
}

void NetwPredictionEngine::finish_reseed_alignment(
    int64_t p_slot,
    int64_t p_recv_tick,
    int64_t p_ack,
    const predict::StateRow &p_payload,
    const Dictionary &p_sample,
    const PackedStringArray &p_participants,
    NetwPredictSlotEngine *p_demote
) {
    predict::Slot *row = mutable_row_of(p_slot);
    NetwPrediction *handle = Object::cast_to<NetwPrediction>(handle_of(p_slot));
    const predict::Decl *decl = decl_of(p_slot);
    if (row == nullptr || handle == nullptr || decl == nullptr) {
        return;
    }
    const NetwEntityRecord &record = *owner_record(p_slot);
    const bool frame_scheduled
        = NetwMultiplayer::predict_schedule_of(record, owner_node(p_slot))
        == NetwSimulation::SCHEDULE_FRAME;
    predict::StateRow seed;
    const bool projected = advanced_seed(
        p_slot,
        p_payload,
        p_ack,
        NetwMultiplayer::sim_snap_restore_of(record),
        NetwMultiplayer::sim_max_restore_ticks_of(record),
        decl->teleport_threshold,
        decl->correction_threshold,
        seed
    );
    carry_reseed(p_slot, p_ack, p_payload, seed);
    row = mutable_row_of(p_slot);
    const bool seated_basis = !projected && row != nullptr
        && row->config.correction == int(CorrectionMode::REPLAY)
        && seat_restore_row(p_slot, p_ack + 1, seed);
    if (apply_restore_row(
            p_slot,
            seed,
            int(predict::Operator::RESEED_ALIGN),
            p_ack,
            p_slot,
            record.get_entity_id(),
            decl->ack_age_ticks,
            decl->correction_threshold,
            true,
            false,
            seated_basis ? p_ack + 1 : -1
        )) {
        sync_episode(p_slot);
    }
    if (!seated_basis && !frame_scheduled) {
        if (predict::Slot *held = mutable_row_of(p_slot)) {
            held->record_state_row(p_ack + 1, p_payload);
        }
    }
    row = mutable_row_of(p_slot);
    if (row != nullptr
        && row->config.correction == int(CorrectionMode::REPLAY)) {
        note_replay_depth(
            p_slot,
            replay_reseed_horizon(
                p_slot,
                p_ack,
                p_sample,
                p_participants,
                p_demote
            )
        );
    }
    adopt_alignment(
        p_slot,
        p_ack,
        frame_scheduled ? last_driven_entry_index_of(p_slot)
                        : latest_input_tick_of(p_slot)
    );
    sync_episode(p_slot);
    announce_evaluation(handle, p_slot, p_recv_tick, p_ack, 0.0, false);
}

bool NetwPredictionEngine::carry_reseed(
    int64_t p_slot,
    int64_t p_ack,
    const predict::StateRow &p_payload,
    predict::StateRow &r_seed
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr
        || row->config.correction == int(CorrectionMode::REPLAY)) {
        return false;
    }
    predict::StateRow predicted;
    if (!row->book.read_row(row->wiring.codec, p_ack + 1, predicted)) {
        return false;
    }
    predict::StateRow current;
    if (!capture_canonical(p_slot, current)) {
        return false;
    }
    row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return false;
    }
    predict::StateRow carried = p_payload;
    if (!predict::carry_difference(
            row->wiring,
            p_payload,
            predicted,
            current,
            carried
        )) {
        return false;
    }
    r_seed = carried;
    return true;
}

void NetwPredictionEngine::run_input(
    int64_t p_slot,
    const predict::StateRow &p_command,
    double p_delta,
    int64_t p_tick,
    bool p_fresh
) {
    if (owner_bound(p_slot)) {
        run_command(p_slot, p_command, p_delta, p_tick, p_fresh);
        return;
    }
    apply_command(p_slot, p_command);
    const Callable step = NetwMultiplayer::sim_step_of(
        *owner_record(p_slot),
        owner_node(p_slot)
    );
    if (step.is_valid()) {
        call_step(step, p_delta, p_tick, p_fresh);
    }
}

void NetwPredictionEngine::consume_one(
    int64_t p_slot,
    double p_delta,
    bool p_holds
) {
    predict::Decl *decl = decl_of(p_slot);
    if (decl == nullptr || !input_lane_open(p_slot)) {
        return;
    }
    const int64_t next = next_input_tick_of(p_slot);
    const bool has_input = has_authored_input(p_slot, next);
    predict::Slot *row = mutable_row_of(p_slot);
    const predict::ConsumeInputPlan plan = plan_consume_input(
        int(Schedule::TICK),
        has_input,
        newest_authored_input(p_slot) > next,
        row->last.any(),
        decl->repeat_missing_input
    );
    const bool stood = plan.missing && row->last.any()
        && next <= row->lane_cursor.input_stood_through;
    const bool held = stood || (!plan.eligible && p_holds);
    if (!plan.eligible && !held) {
        return;
    }
    if (has_input) {
        row->authored_row(next, row->command);
    } else {
        row->command = held || plan.use_last ? row->last : row->stall;
    }
    if (!plan.missing) {
        row->last = row->command;
    }
    row->input_held_transition = held ? next : -1;
    record_drive_hashed(
        p_slot,
        next,
        next,
        int(held ? DriveKind::FRESH : plan.kind),
        command_hash_row(p_slot, row->command),
        next,
        false,
        false,
        false
    );
    row = mutable_row_of(p_slot);
    if ((held || plan.run) && row != nullptr) {
        run_input(p_slot, row->command, p_delta, next, true);
    }
    const int counted = plan.missing && !held ? NetwPredictStats::FACT_MISSING
                                              : NetwPredictStats::FACT_CONSUMED;
    decl->facts.set_int_fact(counted, decl->facts.get_int_fact(counted) + 1);
    set_ack(p_slot, next);
    set_next_input_tick(p_slot, next + 1);
}

int NetwPredictionEngine::replay_tick_window(
    int64_t p_slot,
    int64_t p_from,
    int64_t p_to,
    const Dictionary &p_sample,
    const PackedStringArray &p_participants,
    NetwPredictSlotEngine *p_demote
) {
    NETW_STAGE(ROLLBACK, "Predict replay window", colors::PREDICTION);
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr || !row->lane_open) {
        return 0;
    }
    LocalVector<int64_t> ticks;
    row->authored_ticks(p_from, p_to, ticks);
    note_replay_depth(p_slot, int(ticks.size()));
    replay_ticks(
        p_slot,
        ticks,
        predict::STEP_READS_INPUT | predict::STEP_SEALS,
        p_sample,
        p_participants,
        p_demote
    );
    return int(ticks.size());
}

bool NetwPredictionEngine::run_recovery_ladder(
    int64_t p_slot,
    int64_t p_ack,
    int64_t p_ack_label,
    const predict::StateRow &p_predicted,
    const predict::StateRow &p_payload,
    int p_meter,
    int p_domain,
    int p_attribution,
    const Dictionary &p_sample,
    const PackedStringArray &p_participants,
    NetwPredictSlotEngine *p_demote
) {
    const predict::Decl *decl = decl_of(p_slot);
    if (decl == nullptr) {
        return false;
    }
    const int window = open_recovery_rows(
        p_slot,
        p_ack,
        p_predicted,
        p_payload,
        p_meter,
        p_domain
    );
    if (window == RECOVERY_DISSIPATED) {
        return false;
    }
    bool skip = false;
    bool seated = false;
    if (window != RECOVERY_TRANSPORTED) {
        const predict::WritePlan *planned = plan_recovery_rows(
            p_slot,
            p_ack,
            p_ack_label,
            p_predicted,
            p_payload,
            escalation_pending(p_slot),
            p_domain,
            p_attribution
        );
        if (mutable_row_of(p_slot) == nullptr || planned == nullptr) {
            return false;
        }
        if (planned->escalated) {
            set_cooldown_until_tick(
                p_slot,
                latest_input_tick_of(p_slot) + decl->correction_cooldown_ticks
            );
        }
        skip = planned->skip;
        seated = apply_plan_rows(p_slot, *planned, p_ack);
    }
    predict::Slot *written = mutable_row_of(p_slot);
    if (!skip && written != nullptr
        && written->config.correction != int(CorrectionMode::REPLAY)) {
        written->written_through
            = written->lane_cursor.last_recorded_entry_index;
    }
    close_recovery_rows(
        p_slot,
        p_ack,
        p_payload,
        skip,
        p_attribution,
        p_sample,
        p_participants,
        p_demote,
        seated
    );
    return true;
}

void NetwPredictionEngine::close_recovery_rows(
    int64_t p_slot,
    int64_t p_ack,
    const predict::StateRow &p_payload,
    bool p_skip,
    int64_t p_attribution,
    const Dictionary &p_sample,
    const PackedStringArray &p_participants,
    NetwPredictSlotEngine *p_demote,
    bool p_seated
) {
    NetwPrediction *handle = Object::cast_to<NetwPrediction>(handle_of(p_slot));
    if (handle == nullptr) {
        return;
    }
    const int schedule = NetwMultiplayer::predict_schedule_of(
        *owner_record(p_slot),
        owner_node(p_slot)
    );
    const bool frame_scheduled = schedule == NetwSimulation::SCHEDULE_FRAME;
    if (schedule == NetwSimulation::SCHEDULE_TICK) {
        predict::Slot *row = mutable_row_of(p_slot);
        if (!p_seated && row != nullptr) {
            row->record_state_row(p_ack + 1, p_payload);
        }
    }
    const bool replays
        = !p_skip && correction_of(p_slot) == int(CorrectionMode::REPLAY);
    if (replays && frame_scheduled) {
        replay_authored_entries(
            p_slot,
            p_ack,
            p_sample,
            p_participants,
            p_demote
        );
    } else if (replays) {
        replay_tick_window(
            p_slot,
            p_ack + 1,
            latest_input_tick_of(p_slot),
            p_sample,
            p_participants,
            p_demote
        );
    }
    announce_recovered_rows(p_slot, p_ack, p_attribution);
    note_reconciling(p_slot, false);
}

void NetwPredictionEngine::note_replay_depth(int64_t p_slot, int p_depth) {
#if defined(DEBUG_ENABLED)
    replayed_ticks += p_depth;
#endif
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return;
    }
    row->stats.max_replay_depth = MAX(row->stats.max_replay_depth, p_depth);
}

int NetwPredictionEngine::replay_authored_entries(
    int64_t p_slot,
    int64_t p_ack,
    const Dictionary &p_sample,
    const PackedStringArray &p_participants,
    NetwPredictSlotEngine *p_demote
) {
    NETW_STAGE(ROLLBACK, "Predict replay entries", colors::PREDICTION);
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr) {
        return 0;
    }
    const LocalVector<predict::ReplayEntry> entries
        = replay_entries(p_slot, p_ack);
    note_replay_depth(p_slot, int(entries.size()));
    predict::ReplayPlan plan = solo_plan(p_sample, p_participants, p_demote);
    for (const predict::ReplayEntry &entry : entries) {
        predict::ReplayStep step;
        step.slot = p_slot;
        step.tick = entry.label;
        step.entry = entry.index;
        step.input_key = entry.input_key;
        step.flags = predict::STEP_READS_INPUT | predict::STEP_CLOSES_ENTRY;
        plan.steps.push_back(step);
    }
    run_solo_replay(p_slot, plan);
    return int(entries.size());
}

Dictionary NetwPredictionEngine::close_replayed_entry(
    int64_t p_slot,
    int64_t p_index,
    const Dictionary &p_sample,
    const PackedStringArray &p_participants
) {
    if (row_of(p_slot) == nullptr) {
        return Dictionary();
    }
    if (owner_bound(p_slot)) {
        capture_into_book(p_slot, p_index + 1);
    } else {
        record_predicted(p_slot, p_index + 1, predict::StateRow());
    }
    return seal_transition(p_slot, p_index, p_sample, p_participants);
}

Dictionary NetwPredictionEngine::seal_transition(
    int64_t p_slot,
    int64_t p_transition,
    const Dictionary &p_sample,
    const PackedStringArray &p_participants
) {
    NETW_ZONE_LEAF(
        "Predict seal transition",
        colors::PREDICTION,
        profile::SUBSYSTEM_PREDICTION
    );
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr) {
        return Dictionary();
    }
    if (p_transition >= 0 && joint_provenance_at(p_slot, p_transition) < 0) {
        predict::StateRow coast;
        predict::zero_state_row(row->input_codec, coast);
        joint_record(
            p_slot,
            p_transition,
            predict::StateRow(),
            coast,
            false,
            false,
            false
        );
    }
    const int seat = journal_slot_of(p_slot, p_transition);
    if (seat < 0
        || (journal_flags_at(p_slot, seat) & predict::ROW_CLOSED) != 0) {
        return Dictionary();
    }
    const int32_t post_fp = book_fingerprint(p_slot, p_transition + 1);
    const predict::FamilyFingerprints families
        = book_families(p_slot, p_transition + 1);
    if (p_sample.is_empty()) {
        roster_clear(p_slot, ROSTER_REALIZED_CONTACT);
        record_evidence(
            p_slot,
            p_transition,
            declared_epoch_of(p_slot),
            sensor_samples(p_slot),
            open_topology_of(p_slot),
            PackedStringArray(),
            PackedInt32Array(),
            PackedInt32Array(),
            PackedByteArray(),
            false
        );
        close_drive(
            p_slot,
            p_transition,
            post_fp,
            families.pose,
            families.momentum,
            families.controller
        );
        predict::Slot *held = mutable_row_of(p_slot);
        if (held != nullptr) {
            held->witness_details.forget(
                p_transition,
                predict::TAPE_HISTORY_LIMIT
            );
        }
        return Dictionary();
    }
    const Dictionary solve = solve_evidence(
        p_slot,
        open_topology_of(p_slot),
        p_transition,
        p_sample,
        p_participants
    );
    const Array contacts = solve.get(StringName("contacts"), Array());
    PackedStringArray identities;
    PackedInt32Array witness_classes;
    PackedInt32Array realizations;
    PackedByteArray outside;
    for (int at = 0; at < contacts.size(); ++at) {
        const Dictionary contact = contacts[at];
        identities.append(contact[StringName("identity")]);
        witness_classes.append(contact[StringName("witness_class")]);
        realizations.append(contact[StringName("realization")]);
        outside.append(bool(contact[StringName("outside_boundary")]) ? 1 : 0);
    }
    record_evidence(
        p_slot,
        p_transition,
        declared_epoch_of(p_slot),
        sensor_samples(p_slot),
        solve.get(StringName("topology_facts"), Dictionary()),
        identities,
        witness_classes,
        realizations,
        outside,
        bool(solve.get(StringName("sleeping"), false))
    );
    close_drive(
        p_slot,
        p_transition,
        post_fp,
        families.pose,
        families.momentum,
        families.controller
    );
    record_witness_detail(
        p_slot,
        p_transition,
        solve.get(StringName("detail"), Dictionary())
    );
    return solve;
}

bool NetwPredictionEngine::apply_restore_row(
    int64_t p_slot,
    const predict::StateRow &p_restore,
    int p_operator,
    int64_t p_basis,
    int64_t p_provenance_slot,
    const StringName &p_target,
    int p_ack_age_ticks,
    double p_correction_threshold,
    bool p_evidence_free,
    bool p_pool_planned,
    int64_t p_entry
) {
    if (p_basis < -1 || p_operator < int(predict::Operator::NONE)
        || p_operator > int(predict::Operator::JOINT_REBASE)) {
        return false;
    }
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return false;
    }
    if (p_entry < 0 || !restore_predicted(p_slot, p_entry)) {
        row = mutable_row_of(p_slot);
        if (row != nullptr
            && !push_row(
                *row,
                row->wiring.codec,
                row->owner_has_state,
                p_restore
            )) {
            const PropertyBindingPtr binding = state_binding_of(p_slot);
            if (binding != nullptr) {
                binding->apply_payload(
                    predict::payload_of(row->wiring, p_restore)
                );
            }
        }
        if (predict::Slot *held = mutable_row_of(p_slot)) {
            colliders::teleport(held->port.resolve(sys::PREDICTION));
        }
    }
    record_restore_row(
        p_slot,
        p_restore,
        p_operator,
        p_basis,
        p_provenance_slot,
        p_target,
        p_ack_age_ticks,
        p_correction_threshold,
        p_evidence_free,
        p_pool_planned
    );
    return true;
}

bool NetwPredictionEngine::write_deltas_rows(
    int64_t p_slot,
    const predict::StateRow &p_before,
    const predict::StateRow &p_staged,
    predict::StateRow &r_deltas
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return false;
    }
    const int count = row->wiring.count();
    predict::StateRow &after = row->recovering.live;
    if (!pull_state(*row)) {
        after.resize(count);
    } else {
        after = row->pulled;
    }
    const int staged = std::min(count, int(p_staged.values.size()));
    for (int at = 0; at < staged; ++at) {
        if (p_staged.has(at)) {
            after.set(at, p_staged.values[uint32_t(at)]);
        }
    }
    r_deltas.resize(count);
    bool any = false;
    const int before = std::min(count, int(p_before.values.size()));
    for (int at = 0; at < count; ++at) {
        r_deltas.values[uint32_t(at)] = Variant();
        if (at >= before || !after.has(at) || !p_before.has(at)) {
            continue;
        }
        const Variant delta = prediction_core::pose_delta(
            after.values[uint32_t(at)],
            p_before.values[uint32_t(at)],
            row->wiring.angle[uint32_t(at)] != 0
        );
        if (delta.get_type() == Variant::NIL || delta_negligible(delta)) {
            continue;
        }
        r_deltas.set(at, delta);
        any = true;
    }
    return any;
}

bool NetwPredictionEngine::recent_witness_clean(
    int64_t p_slot,
    int64_t p_basis
) const {
    int64_t expected = p_basis;
    const PackedInt64Array held = journal_transitions(p_slot);
    for (int at = 0; at < held.size(); ++at) {
        const int64_t transition = held[at];
        if (transition < p_basis) {
            continue;
        }
        if (transition != expected) {
            return false;
        }
        const int index = journal_slot_of(p_slot, transition);
        if (index < 0
            || (journal_flags_at(p_slot, index) & predict::ROW_CLOSED) == 0) {
            return expected > p_basis;
        }
        if (!witness_row_clean(p_slot, transition, false)) {
            return false;
        }
        expected += 1;
    }
    return expected > p_basis;
}

Dictionary NetwPredictionEngine::try_transport(
    int64_t p_slot,
    const Dictionary &p_predicted,
    const Dictionary &p_authority,
    const Dictionary &p_current,
    int64_t p_basis,
    bool p_escalated,
    const Callable &p_corridor,
    double p_teleport_default,
    double p_correction_threshold,
    int p_mode
) {
    const predict::Slot *row = row_of(p_slot);
    const PackedInt64Array stats = episode_stats(p_slot);
    if (row == nullptr || !p_corridor.is_valid()
        || stats[STAT_EPISODE_ACTIVE] == 0
        || stats[STAT_EPISODE_TRANSPORT_DECIDED] != 0) {
        return Dictionary();
    }
    const Dictionary candidate
        = transport_of(p_slot, p_predicted, p_authority, p_current);
    const bool basis_clean = witness_row_clean(p_slot, p_basis, true);
    const bool recent_clean = recent_witness_clean(p_slot, p_basis);
    const Dictionary non_pose = non_pose_eligibility(
        p_slot,
        p_predicted,
        p_authority,
        p_correction_threshold
    );
    const Dictionary deltas = transport_deltas(
        p_slot,
        p_current,
        candidate.get(StringName("restore"), Dictionary())
    );
    const bool below_teleport = transport_moved(deltas)
        && !teleport_reached_of(p_slot, deltas, p_teleport_default);
    const bool observing = p_mode == int(NetwPrediction::MODE_OBSERVE);
    const bool valid = candidate.get(StringName("valid"), false);
    const bool prior_ok = transport_admissible(
        p_slot,
        valid,
        basis_clean,
        recent_clean,
        bool(non_pose[StringName("agrees")]),
        below_teleport,
        p_escalated,
        observing
    );
    bool corridor_clear = false;
    if (prior_ok) {
        Dictionary proposed = p_current.duplicate();
        proposed.merge(candidate[StringName("restore")], true);
        Array arguments;
        arguments.push_back(p_current);
        arguments.push_back(proposed);
        corridor_clear = bool(
            game_call(p_corridor, arguments, game_calls::CORRECTION_PATH_CALL)
        );
    }
    const bool eligible = prior_ok && corridor_clear;

    Dictionary eligibility;
    eligibility[StringName("candidate")] = valid;
    eligibility[StringName("basis_witness_clean")] = basis_clean;
    eligibility[StringName("recent_witness_clean")] = recent_clean;
    eligibility[StringName("non_pose")] = non_pose;
    eligibility[StringName("delta")] = deltas;
    eligibility[StringName("below_teleport")] = below_teleport;
    eligibility[StringName("policy")] = !p_escalated && !observing
        && row->config.correction == int(CorrectionMode::SNAP);
    eligibility[StringName("corridor_clear")] = corridor_clear;
    record_episode_decision(
        p_slot,
        int(predict::Operator::TRANSPORT_DELTA),
        p_basis,
        eligible,
        eligible,
        eligibility
    );
    return eligible ? candidate : Dictionary();
}

Dictionary NetwPredictionEngine::solve_evidence(
    int64_t p_slot,
    const Dictionary &p_base_facts,
    int64_t p_transition,
    const Dictionary &p_sample,
    const PackedStringArray &p_participants
) {
    NETW_ZONE_LEAF(
        "Predict solve evidence",
        colors::PREDICTION,
        profile::SUBSYSTEM_PREDICTION
    );
    roster_clear(p_slot, ROSTER_REALIZED_CONTACT);

    Dictionary out;
    out[StringName("topology_facts")] = p_base_facts;
    out[StringName("contacts")] = TypedArray<Dictionary>();
    out[StringName("sleeping")] = false;
    out[StringName("detail")] = Dictionary();
    if (p_sample.is_empty()) {
        return out;
    }

    const Dictionary ground
        = sensor_samples(p_slot).get(StringName("ground"), Dictionary());
    const Variant named = ground.get(StringName("collider"), Variant());
    const String support = named.get_type() == Variant::STRING
            || named.get_type() == Variant::STRING_NAME
        ? String(named)
        : String();

    Array contact_classes;
    PackedStringArray collider_classes;
    PackedStringArray collider_ids;
    PackedStringArray compared_contacts;
    PackedStringArray outside_boundary_ids;
    int witness_class_bits = 0;
    TypedArray<Dictionary> contacts;

    const Array colliders = p_sample[StringName("colliders")];
    for (int at = 0; at < colliders.size(); ++at) {
        Object *collider = colliders[at];
        const RID touched = contact_entity(collider);
        if (touched.is_valid() && slot_of(touched) != p_slot) {
            roster_add(p_slot, ROSTER_REALIZED_CONTACT, touched);
        }
        const int realization = classify_collider(collider, support);
        if (!contact_classes.has(realization)) {
            contact_classes.append(realization);
        }
        const String kind
            = collider != nullptr ? collider->get_class() : String();
        if (!collider_classes.has(kind)) {
            collider_classes.append(kind);
        }
        const String identity = collider_identity(collider);
        if (!collider_ids.has(identity)) {
            collider_ids.append(identity);
        }
        if (contact_breaches_boundary(p_slot, collider, support, p_participants)
            && !outside_boundary_ids.has(identity)) {
            outside_boundary_ids.append(identity);
        }
        const int witness = witness_class_of(collider, support);
        witness_class_bits |= witness;
        const String compared = identity + String(":") + itos(witness);
        if (!compared_contacts.has(compared)) {
            compared_contacts.append(compared);
        }
        Dictionary contact;
        contact[StringName("identity")] = identity;
        contact[StringName("witness_class")] = witness;
        contact[StringName("realization")] = realization;
        contact[StringName("outside_boundary")]
            = outside_boundary_ids.has(identity);
        contacts.append(contact);
    }
    if (contact_classes.is_empty()) {
        contact_classes.append(int(NetwPrediction::CONTACT_CLASS_NONE));
    }
    contact_classes.sort();
    collider_classes.sort();
    collider_ids.sort();
    compared_contacts.sort();
    outside_boundary_ids.sort();

    const bool sleeping = p_sample[StringName("sleeping")];
    const bool woke = has_previous_witness_of(p_slot)
        && previous_witness_sleeping_of(p_slot) && !sleeping;
    set_previous_witness_sleeping(p_slot, sleeping);
    set_has_previous_witness(p_slot, true);

    const Dictionary continuous
        = p_sample.get(StringName("continuous"), Dictionary());
    Dictionary detail;
    detail[StringName("contact_classes")] = contact_classes;
    detail[StringName("collider_classes")] = collider_classes;
    detail[StringName("collider_ids")] = collider_ids;
    detail[StringName("contact_bucket")]
        = prediction_core::contact_count_bucket(colliders.size());
    detail[StringName("sleeping")] = sleeping;
    detail[StringName("woke")] = woke;
    detail[StringName("solve_ordinal")] = p_transition;
    detail[StringName("witness_class_bits")] = witness_class_bits;
    detail[StringName("outside_boundary_ids")] = outside_boundary_ids;
    detail[StringName("breach")] = !outside_boundary_ids.is_empty();
    detail[StringName("continuous")] = continuous.duplicate(true);

    Dictionary facts = p_base_facts.duplicate();
    const StringName SAMPLED[] = {
        StringName("body_mode"),
        StringName("collision_layer"),
        StringName("collision_mask"),
    };
    for (const StringName &key : SAMPLED) {
        if (p_sample.has(key)) {
            facts[key] = p_sample[key];
        }
    }

    out[StringName("topology_facts")] = facts;
    out[StringName("breach")] = !outside_boundary_ids.is_empty();
    out[StringName("contacts")] = contacts;
    out[StringName("sleeping")] = sleeping;
    out[StringName("detail")] = detail;
    return out;
}

int NetwPredictionEngine::witness_class_of(
    Object *p_collider,
    const String &p_support
) const {
    return witness_class(
        p_collider,
        !p_support.is_empty() && collider_identity(p_collider) == p_support
    );
}

bool NetwPredictionEngine::contact_breaches_boundary(
    int64_t p_slot,
    Object *p_collider,
    const String &p_support,
    const PackedStringArray &p_participants
) const {
    if (!p_support.is_empty() && collider_identity(p_collider) == p_support) {
        return false;
    }
    if (static_geometry(p_collider)) {
        return false;
    }
    const RID entity = contact_entity(p_collider);
    if (!entity.is_valid()) {
        return true;
    }
    const NetwEntityRecord *record = member_record(p_slot, entity);
    if (slot_of(record) == p_slot) {
        return false;
    }
    if (!p_participants.has(member_key(p_slot, entity))) {
        return true;
    }
    const predict::Decl *declared
        = record != nullptr ? record->find_predict_declaration() : nullptr;
    return declared == nullptr
        || declared->sim_mode == int(NetwPrediction::SIM_MODE_DISPLAY);
}

int32_t NetwPredictionEngine::book_fingerprint(
    int64_t p_slot,
    int64_t p_key
) const {
    NETW_ZONE_LEAF(
        "Predict book fingerprint",
        colors::PREDICTION,
        profile::SUBSYSTEM_PREDICTION
    );
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr) {
        return 0;
    }
    bool any = false;
    return row->book.fingerprint(
        row->wiring.codec,
        p_key,
        predict::causal_scope(row->wiring),
        any
    );
}

predict::FamilyFingerprints NetwPredictionEngine::book_families(
    int64_t p_slot,
    int64_t p_key
) const {
    predict::FamilyFingerprints out;
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr) {
        return out;
    }
    const predict::StateFamily families[] = {
        predict::StateFamily::POSE,
        predict::StateFamily::MOMENTUM,
        predict::StateFamily::CONTROLLER,
    };
    int32_t *into[] = {&out.pose, &out.momentum, &out.controller};
    for (int at = 0; at < 3; ++at) {
        bool any = false;
        const int32_t hash = row->book.fingerprint(
            row->wiring.codec,
            p_key,
            predict::family_scope(row->wiring, families[at]),
            any
        );
        *into[at] = any ? hash : 0;
    }
    return out;
}

int32_t NetwPredictionEngine::live_fingerprint(
    int64_t p_slot,
    predict::FamilyFingerprints *r_families
) {
    NETW_ZONE_LEAF(
        "Predict live fingerprint",
        colors::PREDICTION,
        profile::SUBSYSTEM_PREDICTION
    );
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return 0;
    }
    static const predict::StateRow NONE;
    const predict::StateRow &live = pull_state(*row) ? row->pulled : NONE;
    bool any = false;
    const int32_t out = predict::fingerprint_row(
        row->wiring.codec,
        live,
        predict::causal_scope(row->wiring),
        row->fingerprint_scratch,
        any,
        session
    );
    if (r_families == nullptr) {
        return out;
    }
    const predict::StateFamily families[] = {
        predict::StateFamily::POSE,
        predict::StateFamily::MOMENTUM,
        predict::StateFamily::CONTROLLER,
    };
    int32_t *into[] = {
        &r_families->pose,
        &r_families->momentum,
        &r_families->controller,
    };
    for (int at = 0; at < 3; ++at) {
        const int32_t hash = predict::fingerprint_row(
            row->wiring.codec,
            live,
            predict::family_scope(row->wiring, families[at]),
            row->fingerprint_scratch,
            any,
            session
        );
        *into[at] = any ? hash : 0;
    }
    return out;
}

int32_t NetwPredictionEngine::payload_fingerprint(
    int64_t p_slot,
    const Dictionary &p_payload
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return 0;
    }
    predict::StateRow payload;
    predict::read_payload(row->wiring, p_payload, payload);
    bool any = false;
    return predict::fingerprint_row(
        row->wiring.codec,
        payload,
        ~uint64_t(0),
        row->fingerprint_scratch,
        any,
        session
    );
}

LocalVector<predict::ReplayEntry> NetwPredictionEngine::replay_entries(
    int64_t p_slot,
    int64_t p_basis
) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? row->replay_entries(p_basis)
                          : LocalVector<predict::ReplayEntry>();
}

void NetwPredictionEngine::close_drive(
    int64_t p_slot,
    int64_t p_transition,
    int64_t p_post_fp,
    int64_t p_post_pose_fp,
    int64_t p_post_momentum_fp,
    int64_t p_post_controller_fp
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return;
    }
    predict::StateStamp post;
    post.fp = int32_t(p_post_fp);
    post.families.pose = int32_t(p_post_pose_fp);
    post.families.momentum = int32_t(p_post_momentum_fp);
    post.families.controller = int32_t(p_post_controller_fp);
    row->close_drive(p_transition, post);
}

bool NetwPredictionEngine::witness_judged(
    int64_t p_slot,
    int64_t p_transition
) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr && row->journal.witness_judged(p_transition);
}

const predict::WritePlan *NetwPredictionEngine::recover_rows(
    int64_t p_slot,
    int64_t p_basis,
    int64_t p_current_label,
    const predict::StateRow &p_predicted,
    const predict::StateRow &p_authority,
    const predict::StateRow &p_current,
    const LocalVector<double> &p_tier_errors,
    int p_policy,
    double p_correction_threshold,
    double p_teleport_threshold,
    int p_max_restore_ticks,
    int p_ack_age_ticks,
    int p_correction_cooldown_ticks,
    int p_domain,
    int p_attribution,
    bool p_suppressed
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return nullptr;
    }
    const bool contact = out_of_domain_at(p_slot, p_current_label);
    const bool unmeasured = !has_pose_fields(p_slot);
    predict::RecoveryRequest &request = row->recovering.request;
    if (!recovery_request_in_range(
            p_policy,
            p_domain,
            p_attribution,
            row->tick_delta
        )) {
        request = predict::RecoveryRequest();
    } else {
        const int width = row->wiring.count();
        request.predicted = p_predicted;
        request.authority = p_authority;
        request.current = p_current;
        request.field_errors.resize(uint32_t(width));
        for (int at = 0; at < width; ++at) {
            request.field_errors[uint32_t(at)] = at < int(p_tier_errors.size())
                ? p_tier_errors[uint32_t(at)]
                : -1.0;
        }
        request.basis = p_basis;
        request.current_label = p_current_label;
        request.policy = p_policy;
        request.fallback_epsilon = p_correction_threshold;
        request.fallback_teleport = p_teleport_threshold;
        request.max_restore_ticks = p_max_restore_ticks;
        request.ack_age_ticks = p_ack_age_ticks;
        request.correction_cooldown_ticks = p_correction_cooldown_ticks;
        request.tick_delta = row->tick_delta;
        request.domain = predict::Domain(p_domain);
        request.attribution = predict::Attribution(p_attribution);
        request.contact_window = contact;
        request.suppressed = p_suppressed;
        request.pose_unmeasured = unmeasured;
    }
    return &row->recover(request);
}

predict::WritePlan NetwPredictionEngine::recover(
    int64_t p_slot,
    const predict::RecoveryRequest &p_request
) {
    predict::Slot *row = mutable_row_of(p_slot);
    return row != nullptr ? row->recover(p_request) : predict::WritePlan();
}

void NetwPredictionEngine::open_recovery_window(
    int64_t p_slot,
    int64_t p_label,
    int p_cooldown
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->open_recovery_window(p_label, p_cooldown);
    }
}

void NetwPredictionEngine::suppress_recovery_until(
    int64_t p_slot,
    int64_t p_label,
    int p_cooldown
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->suppress_recovery_until(p_label, p_cooldown);
    }
}

bool NetwPredictionEngine::escalation_pending(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr && row->recovery.escalate_next;
}

int64_t NetwPredictionEngine::recovery_window_until(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? row->recovery.window_until : -1;
}

int64_t NetwPredictionEngine::recovery_cooldown_until(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? row->recovery.cooldown_until : -1;
}

void NetwPredictionEngine::enter_quarantine(
    int64_t p_slot,
    int64_t p_transition,
    bool p_stream_reconstructed,
    int p_attribution,
    bool p_demoted
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->latch_quarantine(
            p_transition,
            p_stream_reconstructed,
            predict::Attribution(p_attribution),
            p_demoted
        );
    }
}

predict::WritePlan NetwPredictionEngine::quarantine_state(
    int64_t p_slot,
    int64_t p_tick,
    int64_t p_basis,
    const predict::StateRow &p_payload,
    bool p_whole
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return predict::WritePlan();
    }
    return row->quarantine_state(p_tick, p_basis, p_payload, p_whole);
}

predict::WritePlan NetwPredictionEngine::quarantine_witness(
    int64_t p_slot,
    int64_t p_basis,
    int p_bits
) {
    predict::Slot *row = mutable_row_of(p_slot);
    return row != nullptr ? row->quarantine_witness(p_basis, p_bits)
                          : predict::WritePlan();
}

void NetwPredictionEngine::confirm_reseed_epoch(int64_t p_slot) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->confirm_reseed_epoch();
    }
}

predict::WritePlan NetwPredictionEngine::align_reseed(
    int64_t p_slot,
    int64_t p_transition,
    const predict::StateRow &p_payload,
    int64_t p_ignore_through
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return predict::WritePlan();
    }
    return row->align_reseed(p_transition, p_payload, p_ignore_through);
}

bool NetwPredictionEngine::admit_post_reseed(int64_t p_slot, int64_t p_basis) {
    predict::Slot *row = mutable_row_of(p_slot);
    return row != nullptr && row->admit_post_reseed(p_basis);
}

void NetwPredictionEngine::adopt_alignment(
    int64_t p_slot,
    int64_t p_transition,
    int64_t p_ignore_through
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->adopt_alignment(p_transition, p_ignore_through);
    }
}

bool NetwPredictionEngine::finish_probation(int64_t p_slot, bool p_corrected) {
    predict::Slot *row = mutable_row_of(p_slot);
    return row != nullptr && row->finish_probation(p_corrected);
}

#define NETW_PREDICT_QUARANTINE_READ(m_name, m_member, m_type, m_absent) \
    m_type NetwPredictionEngine::m_name(int64_t p_slot) const { \
        const predict::Slot *row = row_of(p_slot); \
        return row != nullptr ? m_type(row->quarantine.m_member) : m_absent; \
    }

NETW_PREDICT_QUARANTINE_READ(quarantine_latched, latched, bool, false)
NETW_PREDICT_QUARANTINE_READ(quarantine_clean_run, clean_run, int, 0)
NETW_PREDICT_QUARANTINE_READ(quarantine_target, target, int, 0)
NETW_PREDICT_QUARANTINE_READ(reseed_align_pending, align_pending, bool, false)
NETW_PREDICT_QUARANTINE_READ(
    reseed_epoch_confirmed,
    epoch_confirmed,
    bool,
    false
)
NETW_PREDICT_QUARANTINE_READ(probation_pending, probation_pending, bool, false)
NETW_PREDICT_QUARANTINE_READ(reseed_ignore_through, ignore_through, int64_t, -1)

#undef NETW_PREDICT_QUARANTINE_READ

NetwEntityRecord *NetwPredictionEngine::seated_record(int64_t p_slot) const {
    const HashMap<int64_t, Seat>::ConstIterator seated
        = seat_by_slot.find(p_slot);
    if (seated == seat_by_slot.end()) {
        return nullptr;
    }
#if defined(NETW_TESTS)
    NETW_BUG_COND_V(!NetwEntityRecord::is_live(seated->value.record), nullptr);
#endif
    return seated->value.record;
}

void NetwPredictionEngine::bind_session(
    NetwMultiplayer *p_session,
    Clock *p_clock,
    store::Store *p_store,
    NetwLivenessCore *p_liveness,
    repl::BindingBook *p_bindings,
    const sim::Rows *p_sims
) {
    session = p_session;
    clock_engine = p_clock;
    store_world = p_store;
    liveness_core = p_liveness;
    binding_book = p_bindings;
    sim_rows = p_sims;
}

void NetwPredictionEngine::attach_bindings(repl::BindingBook *p_book) {
    binding_book = p_book;
}

repl::PropertyBinding *NetwPredictionEngine::binding_by_id(
    uint64_t p_id
) const {
    return binding_book != nullptr ? binding_book->get(p_id) : nullptr;
}

Object *NetwPredictionEngine::handle_of(int64_t p_slot) const {
    if (seated_record(p_slot) == nullptr) {
        return nullptr;
    }
    return gd::object_of(seat_by_slot.find(p_slot)->value.part);
}

const NetwEntityRecord *NetwPredictionEngine::owner_record(
    int64_t p_slot
) const {
    return seated_record(p_slot);
}

RID NetwPredictionEngine::owner_rid(int64_t p_slot) const {
    const NetwEntityRecord *record = seated_record(p_slot);
    return record != nullptr ? record->get_handle() : RID();
}

StringName NetwPredictionEngine::owner_name(int64_t p_slot) const {
    const NetwEntityRecord *record = seated_record(p_slot);
    return record != nullptr ? record->get_entity_id() : StringName();
}

Node *NetwPredictionEngine::owner_node(int64_t p_slot) const {
    if (seated_record(p_slot) == nullptr) {
        return nullptr;
    }
    return Object::cast_to<Node>(
        gd::object_of(seat_by_slot.find(p_slot)->value.node)
    );
}

void NetwPredictionEngine::announce_owner(
    int64_t p_slot,
    session::Announcement p_row
) {
    p_row.entity = owner_rid(p_slot);
    NetwMultiplayer *host = session;
    const bool routed = host != nullptr && p_row.entity.is_valid()
        && host->entity_record(p_row.entity) == owner_record(p_slot);
    if (!routed) {
        NetwMultiplayer::prediction_emit(
            Object::cast_to<NetwPrediction>(handle_of(p_slot)),
            p_row
        );
        return;
    }
    host->announce(p_row);
}

predict::Decl *NetwPredictionEngine::decl_of(int64_t p_slot) const {
    NetwEntityRecord *record = seated_record(p_slot);
    return record != nullptr ? record->find_predict_declaration() : nullptr;
}

void NetwPredictionEngine::report_consume(
    int64_t p_slot,
    int64_t p_depth,
    int64_t p_buffer
) {
    record_predict(p_slot, EventPlane::PREDICT_CONSUME, p_depth, p_buffer);
}

bool NetwPredictionEngine::announce_recovered_rows(
    int64_t p_slot,
    int64_t p_transition,
    int64_t p_attribution
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return false;
    }
    predict::RecoveryRows &recovering = row->recovering;
    if (!write_deltas_rows(
            p_slot,
            recovering.before,
            recovering.write,
            recovering.deltas
        )) {
        return false;
    }
    ledger_note_rows(p_slot, recovering.deltas);
    emit_recovered(
        p_slot,
        p_transition,
        p_attribution,
        last_correction_teleported_of(p_slot)
    );
    return true;
}

void NetwPredictionEngine::emit_recovered(
    int64_t p_slot,
    int64_t p_transition,
    int64_t p_attribution,
    bool p_teleported
) {
    record_predict(p_slot, EventPlane::RECOVERY, p_transition);
    Object *held = handle_of(p_slot);
    const bool heard = held != nullptr
        && (held->has_connections(signal_corrected())
            || reports_to_test(decl_of(p_slot)));
    const RID entity = owner_rid(p_slot);
    NetwMultiplayer *host = session;
    const bool absorbs = host != nullptr && entity.is_valid()
        && host->display_absorbs_recovery(entity);
    const predict::Slot *row = row_of(p_slot);
    if ((!heard && !absorbs) || row == nullptr) {
        return;
    }
    const Dictionary deltas
        = predict::payload_of(row->wiring, row->recovering.deltas);
    if (absorbs) {
        host->display_on_recovered(entity, deltas, p_teleported);
    }
    if (!heard) {
        return;
    }
    NetwMultiplayer::Announcement announced;
    announced.kind = NetwMultiplayer::AnnouncementKind::RECOVERED;
    announced.from = p_transition;
    announced.report = deltas;
    announced.flag = p_teleported;
    announced.code = p_attribution;
    announce_owner(p_slot, announced);
}

int64_t NetwPredictionEngine::current_tick() const {
    NetwMultiplayer *host = session;
    return host != nullptr ? clock_engine->get_tick() : -1;
}

int64_t NetwPredictionEngine::register_rewind(const RID &p_entity) {
    NetwMultiplayer *host = session;
    return host != nullptr ? host->register_rewind(p_entity) : -1;
}

Dictionary NetwPredictionEngine::recovery_write_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr
        ? predict::payload_of(row->wiring, row->recovering.write)
        : Dictionary();
}

const predict::WritePlan *NetwPredictionEngine::plan_recovery_rows(
    int64_t p_slot,
    int64_t p_ack,
    int64_t p_ack_label,
    const predict::StateRow &p_predicted,
    const predict::StateRow &p_payload,
    bool p_escalated,
    int p_domain,
    int p_attribution
) {
    predict::Slot *row = mutable_row_of(p_slot);
    NetwPrediction *handle = Object::cast_to<NetwPrediction>(handle_of(p_slot));
    const predict::Decl *decl = decl_of(p_slot);
    if (row == nullptr || handle == nullptr || decl == nullptr) {
        return nullptr;
    }
    const double epsilon = decl->correction_threshold;
    const double teleport = decl->teleport_threshold;
    const int ack_age = decl->ack_age_ticks;

    guard_projection_rows(p_slot, ack_age);
    row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->recovering.carried = p_payload;
    }
    if (row == nullptr) {
        return nullptr;
    }
    LocalVector<double> &tier_errors = row->recovering.tier_errors;
    if (p_escalated) {
        tier_errors.resize(uint32_t(row->wiring.count()));
        for (uint32_t at = 0; at < tier_errors.size(); ++at) {
            tier_errors[at] = -1.0;
        }
    } else {
        pose_errors_rows(p_slot, p_payload, ack_age, tier_errors);
    }
    row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return nullptr;
    }
    note_errors(*row, row->report.tier_error, row->recovering.tier_errors);

    const bool suppressed = decl->sleeping || probation_pending(p_slot)
        || latest_input_tick_of(p_slot) < cooldown_until_tick_of(p_slot);
    return recover_rows(
        p_slot,
        p_ack,
        p_ack_label,
        p_predicted,
        row->recovering.carried,
        row->recovering.before,
        row->recovering.tier_errors,
        NetwMultiplayer::predict_resolve_mode(
            *owner_record(p_slot),
            owner_node(p_slot)
        ),
        epsilon,
        teleport,
        NetwMultiplayer::sim_max_restore_ticks_of(*owner_record(p_slot)),
        ack_age,
        decl->correction_cooldown_ticks,
        p_domain,
        p_attribution,
        suppressed
    );
}

void NetwPredictionEngine::guard_projection_rows(
    int64_t p_slot,
    int p_ack_age_ticks
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return;
    }
    const int count = row->wiring.count();
    LocalVector<int> &projection = row->recovering.projection;
    projection.resize(uint32_t(count));
    const bool measured = !row->report.divergence.empty();
    const double span
        = double(std::clamp(p_ack_age_ticks, 0, row->config.max_restore_ticks))
        * row->tick_delta;
    for (int at = 0; at < count; ++at) {
        projection[uint32_t(at)] = -1;
        const int channel = row->wiring.projection[uint32_t(at)];
        if (channel < 0) {
            continue;
        }
        if (!measured) {
            projection[uint32_t(at)] = channel;
            continue;
        }
        const double declared = row->wiring.epsilon[uint32_t(at)];
        const double limit = declared >= 0.0 ? declared : row->config.epsilon;
        const double projected_error
            = row->report.divergence.at(channel, 0.0) * span;
        if (projected_error < limit) {
            projection[uint32_t(at)] = channel;
        }
    }
}

void NetwPredictionEngine::pose_errors_rows(
    int64_t p_slot,
    const predict::StateRow &p_payload,
    int p_ack_age_ticks,
    LocalVector<double> &r_errors
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return;
    }
    const int count = row->wiring.count();
    r_errors.resize(uint32_t(count));
    for (uint32_t at = 0; at < r_errors.size(); ++at) {
        r_errors[at] = -1.0;
    }
    if (!has_pose_fields(p_slot)) {
        return;
    }
    const int span
        = std::clamp(p_ack_age_ticks, 0, row->config.max_restore_ticks);
    const double age = double(span) * row->tick_delta;
    predict::StateRow &live = row->recovering.live;
    capture_canonical(p_slot, live);
    row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return;
    }
    const int given = std::min(count, int(p_payload.values.size()));
    for (int at = 0; at < given; ++at) {
        const uint32_t field = uint32_t(at);
        if (row->wiring.pose[field] == 0 || !live.has(at)
            || !p_payload.has(at)) {
            continue;
        }
        Variant target = p_payload.values[field];
        const int channel = row->wiring.projection[field];
        if (age > 0.0 && channel >= 0 && channel < given
            && p_payload.has(channel)
            && project::supports(int(target.get_type()))) {
            target = project::forward(
                target,
                p_payload.values[uint32_t(channel)],
                age
            );
        }
        r_errors[field] = predict::value_error(
            live.values[field],
            target,
            row->wiring.angle[field] != 0
        );
    }
}

bool NetwPredictionEngine::apply_plan_rows(
    int64_t p_slot,
    const predict::WritePlan &p_plan,
    int64_t p_ack
) {
    predict::Slot *row = mutable_row_of(p_slot);
    NetwPrediction *handle = Object::cast_to<NetwPrediction>(handle_of(p_slot));
    const predict::Decl *decl = decl_of(p_slot);
    if (row == nullptr || handle == nullptr || decl == nullptr) {
        return false;
    }
    bool projected = false;
    for (const int channel : row->recovering.projection) {
        projected = projected || channel >= 0;
    }
    record_predict(p_slot, EventPlane::PREDICT_RECOVER, p_ack);
    set_last_correction_teleported(p_slot, p_plan.teleport);
    if (p_plan.skip) {
        note_verdict_reason(p_slot, predict::VERDICT_REASON_DECLINED);
        return false;
    }
    const bool seated_basis
        = row->config.correction == int(CorrectionMode::REPLAY)
        && seat_restore_row(p_slot, p_ack + 1, p_plan.restore);
    row = mutable_row_of(p_slot);
    int op = int(predict::Operator::REBASE_EXACT);
    if (p_plan.teleport) {
        op = int(predict::Operator::FULL_CLOSURE);
    } else if (
        row->config.correction == int(CorrectionMode::SNAP)
        && NetwMultiplayer::sim_snap_restore_of(*owner_record(p_slot))
            == NetwSimulation::RESTORE_EXTRAPOLATED
        && projected
    ) {
        op = int(predict::Operator::REBASE_PROJECTED);
    }
    if (!apply_restore_row(
            p_slot,
            p_plan.restore,
            op,
            p_ack,
            p_slot,
            owner_name(p_slot),
            decl->ack_age_ticks,
            decl->correction_threshold,
            false,
            true,
            seated_basis ? p_ack + 1 : -1
        )) {
        NETW_BUG_REPORT();
    }
    row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->recovering.write = p_plan.write;
    }
    return seated_basis;
}

int NetwPredictionEngine::open_recovery_rows(
    int64_t p_slot,
    int64_t p_ack,
    const predict::StateRow &p_predicted,
    const predict::StateRow &p_payload,
    int p_meter,
    int p_domain
) {
    predict::Slot *row = mutable_row_of(p_slot);
    predict::Decl *decl = decl_of(p_slot);
    if (row == nullptr || decl == nullptr) {
        return RECOVERY_PLAN;
    }
    row->config.correction
        = settle_correction(p_slot, predict::correction_for_mode(decl->mode));
    capture_canonical(p_slot, row->recovering.before);
    row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return RECOVERY_PLAN;
    }
    blank_row(row->recovering.write, row->wiring.count());

    const double epsilon = decl->correction_threshold;
    const int policy = NetwMultiplayer::predict_resolve_mode(
        *owner_record(p_slot),
        owner_node(p_slot)
    );
    const bool escalated = escalation_pending(p_slot);

    Dictionary carried;
    const PackedInt64Array stats = episode_stats(p_slot);
    if (decl->correction_path.is_valid() && stats[STAT_EPISODE_ACTIVE] != 0
        && stats[STAT_EPISODE_TRANSPORT_DECIDED] == 0) {
        carried = try_transport(
            p_slot,
            predict::payload_of(row->wiring, p_predicted),
            predict::payload_of(row->wiring, p_payload),
            predict::payload_of(row->wiring, row->recovering.before),
            p_ack,
            escalated,
            decl->correction_path,
            decl->teleport_threshold,
            epsilon,
            policy
        );
        sync_episode(p_slot);
    }

    if (carried.is_empty()) {
        const int decision = try_dissipate(
            p_slot,
            p_ack,
            p_meter,
            p_domain,
            escalated,
            epsilon,
            policy == NetwPrediction::MODE_OBSERVE,
            owner_name(p_slot),
            decl->ack_age_ticks
        );
        if (decision != DISSIPATE_UNDECIDED) {
            sync_episode(p_slot);
        }
        if (decision == DISSIPATE_APPLIED) {
            note_verdict_reason(p_slot, predict::VERDICT_REASON_DISSIPATED);
            return RECOVERY_DISSIPATED;
        }
    }

    note_reconciling(p_slot, true);
    decl->facts.set_int_fact(
        NetwPredictStats::FACT_CORRECTIONS,
        decl->facts.get_int_fact(NetwPredictStats::FACT_CORRECTIONS) + 1
    );
    if (carried.is_empty()) {
        return RECOVERY_PLAN;
    }

    predict::StateRow restore_row;
    if (const predict::Slot *carrier = row_of(p_slot)) {
        predict::read_payload(
            carrier->wiring,
            carried.get(StringName("restore"), Dictionary()),
            restore_row
        );
    }
    set_last_correction_teleported(p_slot, false);
    if (apply_restore_row(
            p_slot,
            restore_row,
            int(predict::Operator::TRANSPORT_DELTA),
            p_ack,
            p_slot,
            owner_name(p_slot),
            decl->ack_age_ticks,
            epsilon,
            false,
            false
        )) {
        sync_episode(p_slot);
    } else {
        NETW_BUG_REPORT();
    }
    row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->recovering.write = restore_row;
    }
    return RECOVERY_TRANSPORTED;
}

void NetwPredictionEngine::record_witness_detail(
    int64_t p_slot,
    int64_t p_transition,
    const Dictionary &p_detail
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->witness_details
            .record(p_transition, p_detail, predict::TAPE_HISTORY_LIMIT);
    }
}

void NetwPredictionEngine::clear_witness_details(int64_t p_slot) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->witness_details.clear();
    }
}

namespace {

Dictionary causal_only(
    const predict::Slot &p_row,
    const Dictionary &p_payload
) {
    bool any_causal = false;
    for (uint32_t at = 0; at < p_row.wiring.causal.size(); ++at) {
        if (p_row.wiring.causal[at] != 0) {
            any_causal = true;
            break;
        }
    }
    if (!any_causal) {
        return p_payload;
    }
    Dictionary out;
    const Array fields = p_payload.keys();
    for (int at = 0; at < fields.size(); ++at) {
        const int field = p_row.wiring.fields.index_of(fields[at]);
        if (field >= 0 && p_row.wiring.causal[uint32_t(field)] != 0) {
            out[fields[at]] = p_payload[fields[at]];
        }
    }
    return out;
}

} // namespace

bool NetwPredictionEngine::open_state_stamp(
    int64_t p_slot,
    bool p_raw_enabled,
    predict::StateStamp &r_stamp
) {
    r_stamp = predict::StateStamp();
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr || !row->port.bound()) {
        return false;
    }
    r_stamp.fp = int32_t(live_fingerprint(p_slot, &r_stamp.families));
    if (p_raw_enabled) {
        r_stamp.raw_fp = int32_t(
            prediction_core::raw_state_fingerprint(
                causal_only(*row, capture_state(p_slot))
            )
        );
        r_stamp.evidence_mask = predict::EVIDENCE_RAW;
    }
    return true;
}

Dictionary NetwPredictionEngine::transport_deltas(
    int64_t p_slot,
    const Dictionary &p_current,
    const Dictionary &p_restore
) const {
    Dictionary out;
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr || p_restore.is_empty()) {
        return out;
    }
    const Array fields = p_restore.keys();
    for (int at = 0; at < fields.size(); ++at) {
        const Variant key = fields[at];
        if (!p_current.has(key)) {
            return Dictionary();
        }
        const int field = row->wiring.fields.index_of(key);
        const bool angle
            = field >= 0 && row->wiring.angle[uint32_t(field)] != 0;
        out[key] = predict::value_error(p_current[key], p_restore[key], angle);
    }
    return out;
}

bool NetwPredictionEngine::advanced_seed(
    int64_t p_slot,
    const predict::StateRow &p_payload,
    int64_t p_basis,
    int p_snap_restore,
    int p_max_restore_ticks,
    double p_teleport_threshold,
    double p_correction_threshold,
    predict::StateRow &r_seed
) {
    r_seed = p_payload;
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr || !p_payload.any()) {
        return false;
    }
    if (predict::Slot *held = mutable_row_of(p_slot)) {
        held->recovering.carried = p_payload;
    }
    bool projects = false;
    for (const int channel : row->wiring.projection) {
        projects = projects || channel >= 0;
    }
    if (p_snap_restore != int(NetwSimulation::RESTORE_EXTRAPOLATED)
        || !projects) {
        return false;
    }
    const int64_t span = CLAMP(
        transition_span(p_slot, p_basis),
        int64_t(0),
        int64_t(p_max_restore_ticks)
    );
    if (span <= 0) {
        return false;
    }
    project_row(row->wiring, double(span) * row->tick_delta, r_seed);
    return true;
}

Dictionary NetwPredictionEngine::dissipate_fields(
    int64_t p_slot,
    const Dictionary &p_field_divergence,
    int p_domain,
    double p_fallback_epsilon
) const {
    Dictionary fields;
    bool momentum_active = false;
    bool other_active = false;
    const predict::Slot *row = row_of(p_slot);
    if (row != nullptr) {
        const PackedFloat64Array tolerances
            = meter_tolerances(p_slot, p_domain, p_fallback_epsilon);
        for (int at = 0; at < row->wiring.count(); ++at) {
            const uint32_t field = uint32_t(at);
            const bool momentum = row->wiring.withheld[field] != 0;
            const bool excluded = row->wiring.trigger_exclude[field] != 0;
            if (excluded && !momentum) {
                continue;
            }
            const StringName key = row->wiring.fields.name_at(at);
            if (!p_field_divergence.has(key)) {
                continue;
            }
            const double error = double(p_field_divergence[key]);
            double tolerance = tolerances[at];
            if (tolerance < 0.0) {
                const double declared = row->wiring.epsilon[field];
                tolerance = declared >= 0.0 ? declared : p_fallback_epsilon;
            }
            const bool active = error > tolerance;
            Dictionary judged;
            judged[StringName("error")] = error;
            judged[StringName("epsilon")] = tolerance;
            judged[StringName("active")] = active;
            judged[StringName("momentum")] = momentum;
            fields[key] = judged;
            if (!active) {
                continue;
            }
            if (momentum) {
                momentum_active = true;
            } else if (!excluded) {
                other_active = true;
            }
        }
    }
    Dictionary out;
    out[StringName("momentum_active")] = momentum_active;
    out[StringName("other_active")] = other_active;
    out[StringName("fields")] = fields;
    return out;
}

Dictionary NetwPredictionEngine::non_pose_eligibility(
    int64_t p_slot,
    const Dictionary &p_predicted,
    const Dictionary &p_authority,
    double p_fallback_epsilon
) const {
    Dictionary fields;
    bool agrees = true;
    const predict::Slot *row = row_of(p_slot);
    if (row != nullptr) {
        for (int at = 0; at < row->wiring.count(); ++at) {
            const uint32_t field = uint32_t(at);
            if (row->wiring.causal[field] == 0
                || row->wiring.state_family[field]
                    == int(predict::StateFamily::POSE)) {
                continue;
            }
            const StringName key = row->wiring.fields.name_at(at);
            double error = std::numeric_limits<double>::infinity();
            if (p_predicted.has(key) && p_authority.has(key)) {
                error = predict::value_error(
                    p_predicted[key],
                    p_authority[key],
                    row->wiring.angle[field] != 0
                );
            }
            const double declared = row->wiring.epsilon[field];
            const double epsilon
                = declared >= 0.0 ? declared : p_fallback_epsilon;
            const bool field_agrees = !(error > epsilon);
            Dictionary judged;
            judged[StringName("error")] = error;
            judged[StringName("epsilon")] = epsilon;
            judged[StringName("agrees")] = field_agrees;
            fields[key] = judged;
            agrees = agrees && field_agrees;
        }
    }
    Dictionary out;
    out[StringName("agrees")] = agrees;
    out[StringName("fields")] = fields;
    return out;
}

bool NetwPredictionEngine::transport_moved(const Dictionary &p_deltas) {
    const Array fields = p_deltas.keys();
    for (int at = 0; at < fields.size(); ++at) {
        if (double(p_deltas[fields[at]]) > 0.0) {
            return true;
        }
    }
    return false;
}

void NetwPredictionEngine::tape_prepare_tick(int64_t p_slot, int64_t p_tick) {
    predict::Slot *row = mutable_row_of(p_slot);
    NETW_BUG_COND(row == nullptr);
    row->prepare_tick_tape(p_tick);
}

void NetwPredictionEngine::tape_author(
    int64_t p_slot,
    int64_t p_label,
    bool p_fresh
) {
    predict::Slot *row = mutable_row_of(p_slot);
    NETW_BUG_COND(row == nullptr);
    row->author_tape_entry(p_label, p_fresh);
}

bool NetwPredictionEngine::record_predicted(
    int64_t p_slot,
    int64_t p_key,
    const predict::StateRow &p_state
) {
    predict::Slot *row = mutable_row_of(p_slot);
    return row != nullptr && row->record_state_row(p_key, p_state);
}

DriveOutcome NetwPredictionEngine::record_drive_hashed(
    int64_t p_slot,
    int64_t p_transition,
    int64_t p_label,
    int p_kind,
    int64_t p_c_hash,
    int64_t p_drive_tick,
    bool p_caller_selected,
    bool p_input_recorded,
    bool p_authoring
) {
    NETW_PREDICT_TICK_SCOPE();
    DriveOutcome drove;
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return drove;
    }
    const bool is_new = !journal_has(p_slot, p_transition);
    const NetwEntityRecord *declared = is_new ? owner_record(p_slot) : nullptr;
    const int schedule = declared != nullptr
        ? int(NetwMultiplayer::predict_schedule_of(
              *declared,
              owner_node(p_slot)
          ))
        : -1;
    if (schedule == NetwSimulation::SCHEDULE_FRAME) {
        report_quantum_declaration();
        row = mutable_row_of(p_slot);
        if (row == nullptr) {
            return drove;
        }
    }
    if (schedule == NetwSimulation::SCHEDULE_STEPPED
        && row->config.schedule != int(Schedule::STEPPED)) {
        report_stepper_absence(p_slot);
    }
    predict::StateStamp stamp;
    open_state_stamp(p_slot, row->cursor.raw_fingerprints, stamp);
    const int64_t pre_fp = stamp.fp;
    const int64_t raw_fp = stamp.raw_fp;
    const int evidence_mask = int(stamp.evidence_mask);
    const int64_t c_hash = p_c_hash;

    const PackedStringArray &participants = row->island_participants;
    if (!row->topology_open || row->topology_schedule != row->config.schedule
        || row->topology_epoch != row->axes.epoch
        || row->topology_participants != participants) {
        row->rows.open_topology
            = topology_facts(p_slot, row->config.schedule, row->axes.epoch);
        row->topology_schedule = row->config.schedule;
        row->topology_epoch = row->axes.epoch;
        row->topology_participants = participants;
        row->topology_open = true;
    }
    const Dictionary facts = row->rows.open_topology;

    if (p_caller_selected || p_drive_tick >= 0) {
        if (!p_input_recorded) {
            record_input(p_slot, p_label, c_hash);
        }
        const bool selected_kind
            = p_caller_selected || p_kind == int(DriveKind::SUBSTITUTED);
        const predict::DriveRecord record = selected_kind
            ? replay_drive(
                  p_slot,
                  facts,
                  p_transition,
                  p_label,
                  p_kind,
                  p_drive_tick,
                  row->frame_index,
                  row->tick_delta,
                  row->declared_quantum,
                  pre_fp,
                  stamp.families.pose,
                  stamp.families.momentum,
                  stamp.families.controller,
                  raw_fp,
                  evidence_mask,
                  p_authoring
              )
            : open_drive(
                  p_slot,
                  facts,
                  p_drive_tick,
                  row->frame_index,
                  row->tick_delta,
                  row->declared_quantum,
                  true,
                  pre_fp,
                  stamp.families.pose,
                  stamp.families.momentum,
                  stamp.families.controller,
                  raw_fp,
                  evidence_mask
              );
        const int64_t opened = record.transition;
        if (record.ran) {
            drove.transition = opened;
            drove.label = record.label;
            drove.kind = int(record.kind);
            drove.fresh = record.fresh;
            drove.ran = true;
        }
        if (opened >= 0) {
            stamp_pending_provenance(p_slot, opened);
        }
    }
    if (is_new) {
        if (predict::Slot *held = mutable_row_of(p_slot)) {
            held->pending_provenance = predict::PendingProvenance();
        }
    }
    adopt_environment_epoch(
        p_slot,
        row->axes.epoch,
        row->cursor.latest_input_tick,
        row->config.correction_cooldown_ticks
    );
    sample_environment(p_slot, row->axes.epoch);

    const int domain = prediction_core::domain_of(
        row->config.island_declared,
        row->config.island_approximate,
        p_label,
        out_of_domain_until(p_slot)
    );
    if (journal_has(p_slot, p_transition)) {
        mark_domain(p_slot, p_transition, domain);
    }
    if (drove.ran) {
        record_predict(p_slot, EventPlane::PREDICT_DRIVE, drove.transition);
    }
    return drove;
}

bool NetwPredictionEngine::read_arrival(
    int64_t p_slot,
    const predict::Arrival &p_arrival,
    predict::StateRow &r_row
) const {
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr || !p_arrival.read(row->wiring.codec, r_row)) {
        return false;
    }
    read_parts(*row, r_row);
    return true;
}

bool NetwPredictionEngine::defers_arrival(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr && !row->parts.is_empty();
}

void NetwPredictionEngine::take_received(
    int64_t p_slot,
    const predict::Arrival *p_arrival,
    const predict::StateRow *p_held
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return;
    }
    if (p_arrival != nullptr) {
        if (p_arrival->read(row->wiring.codec, row->received)) {
            read_parts(*row, row->received);
        }
    } else if (p_held != nullptr) {
        row->received = *p_held;
    } else {
        row->received.resize(row->wiring.count());
    }
}

int NetwPredictionEngine::admit_state(int64_t p_slot, int64_t p_ack) {
    if (row_of(p_slot) == nullptr) {
        return ADMIT_CLOSED;
    }
    note_verdict_reason(p_slot, int(predict::VERDICT_REASON_NONE));
    clear_tier_errors(p_slot);
    if (reseed_align_pending(p_slot)) {
        if (reseed_epoch_confirmed(p_slot)) {
            return ADMIT_REALIGN;
        }
        note_verdict_reason(
            p_slot,
            int(predict::VERDICT_REASON_REALIGN_PENDING)
        );
        return ADMIT_REALIGN_PENDING;
    }
    if (!admit_post_reseed(p_slot, p_ack)) {
        note_verdict_reason(
            p_slot,
            int(predict::VERDICT_REASON_RESEED_IGNORED)
        );
        return ADMIT_RESEED_IGNORED;
    }
    if (p_ack <= row_of(p_slot)->written_through) {
        return ADMIT_PREDATES_WRITE;
    }
    return ADMIT_PROCEED;
}

int NetwPredictionEngine::refuse_recovery(int64_t p_slot, bool p_corrected) {
    const int none = int(predict::VERDICT_REASON_NONE);
    if (!p_corrected || row_of(p_slot) == nullptr) {
        return none;
    }
    int reason = none;
    if (episode_budget_exhausted(p_slot)) {
        reason = int(predict::VERDICT_REASON_EVIDENCE_EXHAUSTED);
    } else if (
        episode_operator_pending(
            p_slot,
            int(predict::Operator::TRANSPORT_DELTA)
        )
    ) {
        reason = int(predict::VERDICT_REASON_TRANSPORT_PENDING);
    } else if (
        episode_operator_pending(p_slot, int(predict::Operator::DISSIPATE))
    ) {
        reason = int(predict::VERDICT_REASON_DISSIPATE_PENDING);
    }
    if (reason != none) {
        note_verdict_reason(p_slot, reason);
    }
    return reason;
}

void NetwPredictionEngine::record_restore_row(
    int64_t p_slot,
    const predict::StateRow &p_restore,
    int p_operator,
    int64_t p_basis,
    int64_t p_provenance_slot,
    const StringName &p_target,
    int p_ack_age_ticks,
    double p_correction_threshold,
    bool p_evidence_free,
    bool p_pool_planned
) {
    if (row_of(p_slot) == nullptr) {
        return;
    }
    if (p_operator == int(predict::Operator::NONE)) {
        return;
    }
    const int64_t owner
        = row_of(p_provenance_slot) != nullptr ? p_provenance_slot : p_slot;
    const PackedInt64Array stats = episode_stats(owner);
    const int64_t episode
        = stats[STAT_EPISODE_STATE] == int(predict::EpisodeState::OPEN)
        ? stats[STAT_EPISODE_ID]
        : int64_t(0);
    int64_t delta_fp = 0;
    if (predict::Slot *row = mutable_row_of(p_slot)) {
        bool any = false;
        delta_fp = predict::fingerprint_row(
            row->wiring.codec,
            p_restore,
            ~uint64_t(0),
            row->fingerprint_scratch,
            any,
            session
        );
    }
    int64_t write_id = 0;
    if (p_pool_planned) {
        write_id = stats[STAT_EPISODE_LAST_WRITE_ID];
        stamp_episode_write_delta(owner, int(delta_fp));
    } else {
        write_id = record_episode_write(
            owner,
            p_operator,
            p_basis,
            int(delta_fp),
            p_target,
            p_ack_age_ticks,
            trigger_shape(owner, p_correction_threshold),
            p_evidence_free,
            false
        );
    }
    if (predict::Slot *row = mutable_row_of(p_slot)) {
        predict::PendingProvenance &pending = row->pending_provenance;
        pending.episode = episode;
        pending.write_id = write_id;
        pending.op = p_operator;
        pending.basis = p_basis;
        pending.held = true;
    }
}

void NetwPredictionEngine::bind_property_sets(
    int64_t p_slot,
    const PropertyBindingPtr &p_state,
    const PropertyBindingPtr &p_input
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return;
    }
    const uint64_t state = p_state != nullptr ? p_state->id : 0;
    const uint64_t input = p_input != nullptr ? p_input->id : 0;
    if (binding_book != nullptr) {
        binding_book->pin(state, repl::Pin::STATE);
        binding_book->pin(input, repl::Pin::INPUT);
    }
    release_pins(*row);
    row->state_binding_id = state;
    row->input_binding_id = input;
}

void NetwPredictionEngine::release_pins(predict::Slot &r_row) {
    const uint64_t state = r_row.state_binding_id;
    const uint64_t input = r_row.input_binding_id;
    r_row.state_binding_id = 0;
    r_row.input_binding_id = 0;
    if (binding_book != nullptr) {
        binding_book->unpin(state, repl::Pin::STATE);
        binding_book->unpin(input, repl::Pin::INPUT);
    }
}

void NetwPredictionEngine::release_bindings() {
    for (predict::Slot *row : rows) {
        if (row != nullptr) {
            release_pins(*row);
        }
    }
}

repl::PropertyBinding *NetwPredictionEngine::state_binding_in(
    const predict::Slot &p_row
) const {
    return binding_book != nullptr ? binding_book->get(p_row.state_binding_id)
                                   : nullptr;
}

repl::PropertyBinding *NetwPredictionEngine::input_binding_in(
    const predict::Slot &p_row
) const {
    return binding_book != nullptr ? binding_book->get(p_row.input_binding_id)
                                   : nullptr;
}

PropertyBindingPtr NetwPredictionEngine::state_binding_of(
    int64_t p_slot
) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? nullptr : state_binding_in(*row);
}

PropertyBindingPtr NetwPredictionEngine::input_binding_of(
    int64_t p_slot
) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? nullptr : input_binding_in(*row);
}

bool NetwPredictionEngine::record_authored_command(
    int64_t p_slot,
    int64_t p_transition,
    int64_t p_label,
    bool p_fresh,
    const predict::StateRow &p_command
) {
    NETW_PREDICT_TICK_SCOPE();
    predict::Slot *row = mutable_row_of(p_slot);
    return row != nullptr && row->lane_open
        && row->record_authored_row(p_transition, p_label, p_fresh, p_command);
}

int64_t NetwPredictionEngine::record_arrived_inputs(
    int64_t p_slot,
    const predict::Arrival &p_arrival,
    bool p_keyed_by_tick
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr || !row->lane_open) {
        return -1;
    }
    int64_t oldest = -1;
    const auto admit = [&](int64_t p_tick) {
        if (p_keyed_by_tick) {
            row->record_authored_row(p_tick, p_tick, true, row->arrived);
        }
        oldest = oldest < 0 ? p_tick : std::min(oldest, p_tick);
    };
    const uint32_t samples = p_arrival.sample_count();
    if (samples == 0) {
        if (p_arrival.tick >= 0
            && p_arrival.read(row->input_codec, row->arrived)) {
            admit(p_arrival.tick);
        }
        return oldest;
    }
    for (uint32_t sample = 0; sample < samples; ++sample) {
        const int64_t tick = p_arrival.sample_tick(sample);
        if (tick >= 0
            && p_arrival.read_sample(row->input_codec, sample, row->arrived)) {
            admit(tick);
        }
    }
    return oldest;
}

bool NetwPredictionEngine::has_authored_input(
    int64_t p_slot,
    int64_t p_transition
) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr && row->has_authored(p_transition);
}

int64_t NetwPredictionEngine::newest_authored_input(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr && row->lane_open ? row->newest_authored() : -1;
}

void NetwPredictionEngine::note_relayed(int64_t p_slot, int64_t p_transition) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->inputs.note_relayed(p_transition);
    }
}

bool NetwPredictionEngine::relayed_at(
    int64_t p_slot,
    int64_t p_transition
) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr && row->inputs.relayed_at(p_transition);
}

bool NetwPredictionEngine::input_fresh_at(
    int64_t p_slot,
    int64_t p_transition
) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr && row->inputs.fresh_at(p_transition);
}

int64_t NetwPredictionEngine::input_label_of(
    int64_t p_slot,
    int64_t p_transition
) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? row->inputs.label_of(p_transition) : -1;
}

bool NetwPredictionEngine::command_codes_agree(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    const SchemaRecord *schema = input_schema(p_slot);
    if (row == nullptr || schema == nullptr) {
        return false;
    }
    predict::CommandFrameRecord frame;
    return predict::CommandFrameRecord::open(*schema, frame)
        && predict::codes_agree(
               row->input_codec,
               frame.row_plan(),
               *schema,
               input_keys(p_slot)
        );
}

void NetwPredictionEngine::trim_history(int64_t p_slot, int64_t p_ack) {
    predict::Slot *row = mutable_row_of(p_slot);
    NETW_BUG_COND(row == nullptr);
    row->trim_history(p_ack);
}

bool NetwPredictionEngine::command_admit(
    int64_t p_slot,
    int64_t p_transition,
    int64_t p_label,
    bool p_fresh,
    const predict::StateRow &p_command
) {
    predict::Slot *row = mutable_row_of(p_slot);
    NETW_BUG_COND_V(row == nullptr, false);
    return row->admit_command(p_transition, p_label, p_fresh, p_command);
}

bool NetwPredictionEngine::command_has(
    int64_t p_slot,
    int64_t p_transition
) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr && row->holds_command(p_transition);
}

int64_t NetwPredictionEngine::command_label_of(
    int64_t p_slot,
    int64_t p_transition
) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr && row->holds_command(p_transition)
        ? row->inputs.label_of(p_transition)
        : -1;
}

bool NetwPredictionEngine::command_is_fresh(
    int64_t p_slot,
    int64_t p_transition
) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr && row->holds_command(p_transition)
        && row->inputs.fresh_at(p_transition);
}

int NetwPredictionEngine::command_depth_from(
    int64_t p_slot,
    int64_t p_cursor
) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? row->command_depth_from(p_cursor) : 0;
}

PackedInt64Array NetwPredictionEngine::command_transitions(
    int64_t p_slot
) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? row->command_keys() : PackedInt64Array();
}

void NetwPredictionEngine::record_idle_drive(
    int64_t p_slot,
    int64_t p_label,
    int p_kind
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->record_idle_drive(p_label, DriveKind(p_kind));
    }
}

void NetwPredictionEngine::record_authoring_clamp(int64_t p_slot) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->record_authoring_clamp();
    }
}

void NetwPredictionEngine::record_speculation_hold(int64_t p_slot) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->record_speculation_hold();
    }
}

void NetwPredictionEngine::refresh_ack_age(int64_t p_slot) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->refresh_ack_age();
    }
}

} // namespace netw
