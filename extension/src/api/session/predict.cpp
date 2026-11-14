#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/scene.hpp"

#include "netw/api/participant.hpp"

#include <algorithm>
#include <cmath>

#include "godot/class_db.hpp"
#include "godot/engine.hpp"
#include "godot/node.hpp"
#include "godot/object.hpp"
#include "godot/os.hpp"
#include "godot/packed_scene.hpp"
#include "godot/physics_body.hpp"
#include "godot/physics_server.hpp"
#include "godot/project_settings.hpp"
#include "godot/rendering_server.hpp"
#include "godot/resource.hpp"
#include "godot/scene_tree.hpp"
#include "godot/script.hpp"
#include "godot/spatial_node.hpp"
#include "godot/time.hpp"
#include "godot/utility.hpp"
#include "godot/viewport.hpp"
#include "godot/world.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/entity_record.hpp"
#include "netw/api/join_request.hpp"
#include "netw/api/lag_compensation_config.hpp"
#include "netw/api/predict_slot_engine.hpp"
#include "netw/api/prediction.hpp"
#include "netw/api/replication_core.hpp"
#include "netw/api/schema_model.hpp"
#include "netw/colliders.hpp"
#include "netw/colors.hpp"
#include "netw/comp_table.hpp"
#include "netw/fault_book.hpp"
#include "netw/game_call.hpp"
#include "netw/log.hpp"
#include "netw/predict/axes.hpp"
#include "netw/predict/decl.hpp"
#include "netw/prediction_core.hpp"
#include "netw/profile.hpp"
#include "netw/repl/sync_pipeline.hpp"
#include "netw/script/model.hpp"
#include "netw/session/frames.hpp"
#include "netw/stage.hpp"
#include "netw/synchronizers.hpp"
#include "netw/wire/frame.hpp"
#include "netw/wire/registry.hpp"

using namespace godot;
using namespace netw;

namespace netw {

Error NetwMultiplayer::predict_admit_frame(
    int64_t p_sender,
    int64_t p_route,
    int64_t p_channel,
    const PackedByteArray &p_payload
) {
    NETW_ZONE_NC("Predict admit frame", colors::PREDICTION);
    const Ref<NetwEntity> entity
        = Object::cast_to<NetwEntity>(wrapper_for_route(p_route).ptr());
    return Error(
        prediction_core::admit_frame(
            int(p_channel),
            int(p_sender),
            entity.is_valid() ? int(entity->steering()) : 0,
            SERVER_PEER,
            is_host(),
            p_payload.is_empty(),
            int(entity_frame_verdict(p_route))
        )
    );
}

void NetwMultiplayer::predict_relay_subscribe(
    const RID &p_entity,
    bool p_subscribed
) {
    const Ref<NetwEntity> wrapper = entity_get_view(p_entity);
    if (wrapper.is_null() || is_host()) {
        return;
    }
    const int64_t route = liveness_route_of(wrapper.ptr());
    if (route <= 0) {
        return;
    }
    send_to(
        MultiplayerPeer::TARGET_PEER_SERVER,
        route,
        wire::builtin_channel("PREDICT_RELAY_REQUEST"),
        predict::RelayBook::request_bytes(p_subscribed),
        true,
        0,
        String(),
        false
    );
}

int64_t NetwMultiplayer::arm_rewind_timeline(const Ref<NetwEntity> &p_entity) {
    const int64_t slot = lagcomp_core.timeline_slot_of(p_entity);
    if (slot < 0) {
        return -1;
    }
    const repl::PropertyBindingPtr state = p_entity->get_state_binding();
    if (state == nullptr || state->get_set() == nullptr) {
        return -1;
    }
    Node *node = state->node();
    if (node == nullptr) {
        return -1;
    }
    lagcomp_core.timeline_bind_owner(slot, node);
    Array keys;
    for (const repl::PropertyColumn &column : state->get_set()->columns) {
        keys.push_back(column.get_key());
    }
    lagcomp_core.timeline_declare(slot, keys);
    return slot;
}

void NetwMultiplayer::record_rewind_parts(
    int64_t p_slot,
    NetwEntity *p_entity,
    const repl::PropertyBindingPtr &p_state,
    int64_t p_tick
) {
    NetwEntityRecord *record = record_of(p_entity);
    if (record == nullptr || record->get_held_bindings().size() < 2) {
        return;
    }
    if (!lagcomp_core.timeline_owner_bound(p_slot)) {
        lagcomp_core.timeline_bind_owner(p_slot, p_state->node());
    }
    for (const NetwEntityRecord::HeldBinding &held :
         record->get_held_bindings()) {
        repl::PropertyBinding *child = binding_book.live(held.binding);
        if (child == nullptr || child == p_state || child->node() == nullptr) {
            continue;
        }
        repl::PropertySet *declared = child->get_set();
        if (declared == nullptr
            || declared->get_record() != repl::SET_RECORD_STATE) {
            continue;
        }
        Array keys;
        for (const repl::PropertyColumn &column : declared->columns) {
            keys.push_back(column.get_key());
        }
        const int64_t part
            = lagcomp_core
                  .timeline_part(p_slot, child->id, child->node(), keys);
        lagcomp_core.timeline_record_coded(
            part,
            p_tick,
            child->canonicalize_payload(child->snapshot_payload()),
            declared->get_codec()
        );
    }
    lagcomp_core.timeline_prune_parts(p_slot);
}

void NetwMultiplayer::lagcomp_rewind(
    const TypedArray<RID> &p_entities,
    int64_t p_tick,
    const Callable &p_body
) {
    TypedArray<NetwEntity> wrappers;
    for (int at = 0; at < p_entities.size(); at++) {
        wrappers.push_back(entity_get_view(p_entities[at]));
    }
    lagcomp_rewind_of(wrappers, p_tick, p_body);
}

Variant NetwMultiplayer::lagcomp_rewind_of(
    const TypedArray<NetwEntity> &p_entities,
    int64_t p_tick,
    const Callable &p_body
) {
    PackedInt64Array slots;
    for (int at = 0; at < p_entities.size(); at++) {
        const Ref<NetwEntity> wrapper = p_entities[at];
        if (wrapper.is_null()) {
            continue;
        }
        const int64_t slot = arm_rewind_timeline(wrapper);
        if (slot >= 0) {
            slots.push_back(slot);
        }
    }
    bool awaited = false;
    const Variant answered
        = lagcomp_core.rewind(slots, p_tick, p_body, nullptr, &awaited);
    if (awaited) {
        Node *runner = Object::cast_to<Node>(p_body.get_object());
        fault::raise(
            this,
            NETW_FAULT_REWIND_MISUSE,
            {runner != nullptr ? String(runner->get_name())
                               : String(p_body.get_method()),
             String(),
             String(),
             0,
             NETW_REWIND_AWAITED},
            gd::instance_id(runner)
        );
    }
    return answered;
}

Ref<DictionaryRecord> NetwMultiplayer::lagcomp_sample(
    const RID &p_entity,
    int64_t p_tick
) const {
    return lagcomp_sample_of(entity_get_view(p_entity), p_tick);
}

Ref<DictionaryRecord> NetwMultiplayer::lagcomp_sample_of(
    const Ref<RefCounted> &p_entity,
    int64_t p_tick
) const {
    Ref<DictionaryRecord> sampled;
    sampled.instantiate();
    if (p_entity.is_null()) {
        return sampled;
    }
    sampled->set_data(
        lagcomp_core.timeline_sample_entity(p_entity, p_tick).duplicate()
    );
    return sampled;
}

bool NetwMultiplayer::predict_tap_open() {
    if (predict_tap_disarmed) {
        return false;
    }
    if (predict_tap.ready()) {
        return true;
    }
    const String dir = predict::Tap::armed_dir();
    if (dir.is_empty()) {
        predict_tap_disarmed = true;
        return false;
    }
    predict_tap.open(dir);
    prediction_engine.attach_journal_readers(true);
    return true;
}

void NetwMultiplayer::predict_tap_drain(
    const StringName &p_entity_id,
    int64_t p_slot,
    const Dictionary &p_stats
) {
    if (predict_tap.ready()) {
        predict_tap.drain(p_entity_id, prediction_engine, p_slot, p_stats);
    }
}

void NetwMultiplayer::predict_tap_pump() {
    NETW_ZONE_NC("Predict tap pump", colors::PREDICTION);
    if (!predict_tap_open()) {
        return;
    }
    if (predict_tap_every <= 0) {
        const int64_t authored = int64_t(
            OS::get_singleton()
                ->get_environment("NETW_PREDICT_TAP_EVERY")
                .to_int()
        );
        predict_tap_every = authored > 1 ? authored : 1;
    }
    predict_tap_frame += 1;
    if (predict_tap_frame % predict_tap_every != 0) {
        NETW_PLOT(profile::names::PREDICT_TAP_DRAINED, 0.0);
        return;
    }
    const TypedArray<NetwEntity> entities = predict_engine_entities();
    int64_t drained = 0;
    for (int at = 0; at < entities.size(); at++) {
        const Ref<NetwEntity> entity = entities[at];
        if (entity.is_null()) {
            continue;
        }
        const predict::Decl *decl = predict_find_declaration(entity.ptr());
        if (decl == nullptr) {
            continue;
        }
        predict_tap_drain(
            entity->get_name(),
            native_prediction_slot(entity),
            decl->facts.to_dictionary()
        );
        drained += 1;
    }
    NETW_PLOT(profile::names::PREDICT_TAP_DRAINED, double(drained));
    NETW_ZONE_VALUE(uint64_t(entities.size() - drained));
}

void NetwMultiplayer::predict_flush_tap() {
    if (predict_tap.ready()) {
        predict_tap.flush();
    }
}

Dictionary NetwMultiplayer::predict_get_tap_cost() const {
    return predict_tap.ready() ? predict_tap.cost() : Dictionary();
}

void NetwMultiplayer::predict_tap_close() {
    if (predict_tap.ready()) {
        predict_tap.close();
    }
    predict_tap_every = 0;
}

namespace {

void space_set_active(
    const NetwMultiplayer::EntitySpace &p_held,
    bool p_active
) {
    if (!p_held.space.is_valid()) {
        return;
    }
    if (p_held.dimension == 3) {
        PhysicsServer3D::get_singleton()->space_set_active(
            p_held.space,
            p_active
        );
        return;
    }
    if (p_held.dimension == 2) {
        PhysicsServer2D::get_singleton()->space_set_active(
            p_held.space,
            p_active
        );
    }
}

} // namespace

void NetwMultiplayer::warn_pacing_rate_mismatch_once() {
    const double factor = clock_engine().physics_factor();
    if (std::abs(factor - std::round(factor)) < 1e-6) {
        return;
    }
    const Engine *engine = Engine::get_singleton();
    const int physics_rate
        = engine != nullptr ? engine->get_physics_ticks_per_second() : 60;
    fault::raise(
        this,
        NETW_FAULT_TICK_SPLITS_PHYSICS,
        {String(),
         String(),
         String(),
         0,
         physics_rate,
         clock_engine().get_tickrate()}
    );
}

void NetwMultiplayer::simulation_gate_set(const RID &p_entity, bool p_wanted) {
    if (!p_entity.is_valid()) {
        return;
    }
    int64_t index = -1;
    for (uint32_t i = 0; i < gated_bodies.size(); ++i) {
        if (gated_bodies[i].entity == p_entity) {
            index = int64_t(i);
            break;
        }
    }
    if (p_wanted == (index >= 0)) {
        return;
    }
    ClockEngine &clock = clock_engine();
    if (p_wanted) {
        GatedBody row;
        row.entity = p_entity;
        row.held = entity_space_of(entity_get_view(p_entity));
        gated_bodies.push_back(row);
        clock.arm_gate();
        predict_gate_arms += 1;
        warn_pacing_rate_mismatch_once();
        return;
    }
    const GatedBody &releasing = gated_bodies[uint32_t(index)];
    if (!predict_space_is_stepped(releasing.held.space)) {
        space_set_active(releasing.held, true);
    }
    gated_bodies.remove_at(uint32_t(index));
    clock.release_gate();
    predict_gate_arms -= 1;
}

void NetwMultiplayer::simulation_gate_apply() {
    if (gated_bodies.is_empty()) {
        return;
    }
    const bool active = clock_engine().is_simulating();
    uint32_t i = 0;
    while (i < gated_bodies.size()) {
        GatedBody &row = gated_bodies[i];
        const Ref<NetwEntity> wrapper = entity_get_view(row.entity);
        if (wrapper.is_null() || wrapper->get_owner() == nullptr) {
            gated_bodies.remove_at(i);
            continue;
        }
        const EntitySpace resolved = entity_space_of(wrapper);
        if (resolved.space != row.held.space) {
            if (!predict_space_is_stepped(row.held.space)) {
                space_set_active(row.held, true);
            }
            row.held = resolved;
        }
        space_set_active(
            row.held,
            active && !predict_space_is_stepped(row.held.space)
        );
        ++i;
    }
    if (gated_bodies.is_empty()) {
        while (predict_gate_arms > 0) {
            clock_engine().release_gate();
            predict_gate_arms -= 1;
        }
    }
}

int64_t NetwMultiplayer::simulation_gate_count() const {
    return int64_t(gated_bodies.size());
}

void NetwMultiplayer::physics_frame_advance() {
    if (clock_engine().is_simulating()) {
        physics_frame += 1;
    }
}

NetwPredictTiming NetwMultiplayer::tick_timing(
    int64_t p_tick,
    double p_delta
) const {
    return NetwPredictTiming::of(
        p_tick,
        p_delta,
        clock_engine().ticktime(),
        physics_frame,
        declared_quantum()
    );
}

Error NetwMultiplayer::predict_stepper_install(
    const RID &p_space,
    const Ref<NetwPhysicsStepper> &p_stepper
) {
    if (!p_space.is_valid()) {
        fault::raise(
            this,
            NETW_FAULT_EMPTY_ARGUMENT,
            {"{NetwSession.install_physics_stepper}",
             String(),
             "physics space"}
        );
        return ERR_INVALID_PARAMETER;
    }
    if (p_stepper.is_null() || !p_stepper->can_step()) {
        const SteppedSpace *held = space_steppers.getptr(p_space.get_id());
        if (held != nullptr && held->inactive) {
            space_set_active({held->space, held->dimension}, true);
        }
        space_steppers.erase(p_space.get_id());
        predict_reresolve_space(p_space);
        return OK;
    }
    SteppedSpace row;
    row.space = p_space;
    row.stepper = p_stepper;
    space_steppers.insert(p_space.get_id(), row);
    predict_reresolve_space(p_space);
    return OK;
}

void NetwMultiplayer::predict_rewire_seated(
    const LocalVector<int64_t> &p_keys
) {
    for (const int64_t key : p_keys) {
        NetwPredictSlotEngine *const *held = predict_engines.getptr(key);
        if (held != nullptr) {
            (*held)->rewire();
        }
    }
}

void NetwMultiplayer::predict_reresolve_space(const RID &p_space) {
    LocalVector<int64_t> named;
    for (const KeyValue<int64_t, NetwPredictSlotEngine *> &row :
         predict_engines) {
        const Ref<NetwEntity> seated = wrapper_for_id(row.key);
        if (seated.is_valid() && entity_space_of(seated).space == p_space) {
            named.push_back(row.key);
        }
    }
    predict_rewire_seated(named);
}

void NetwMultiplayer::predict_rebind_replaced(uint64_t p_binding) {
    LocalVector<int64_t> named;
    for (const KeyValue<int64_t, NetwPredictSlotEngine *> &row :
         predict_engines) {
        const repl::PropertyBindingPtr state = row.value->state_binding();
        const repl::PropertyBindingPtr input = row.value->input_binding();
        if ((state != nullptr && state->id == p_binding)
            || (input != nullptr && input->id == p_binding)) {
            named.push_back(row.key);
        }
    }
    predict_rewire_seated(named);
}

Ref<NetwPhysicsStepper> NetwMultiplayer::predict_get_stepper(
    const RID &p_space
) const {
    const SteppedSpace *held = space_steppers.getptr(p_space.get_id());
    return held != nullptr ? held->stepper : Ref<NetwPhysicsStepper>();
}

void NetwMultiplayer::predict_stepper_hold(
    const RID &p_space,
    int p_dimension
) {
    SteppedSpace *held = space_steppers.getptr(p_space.get_id());
    if (held == nullptr && gd::can_step_space(p_dimension)) {
        SteppedSpace row;
        row.space = p_space;
        space_steppers.insert(p_space.get_id(), row);
        held = space_steppers.getptr(p_space.get_id());
    }
    if (held == nullptr || held->inactive) {
        return;
    }
    held->dimension = p_dimension;
    held->inactive = true;
    space_set_active({held->space, held->dimension}, false);
}

bool NetwMultiplayer::predict_space_can_step(
    const RID &p_space,
    int p_dimension
) const {
    const SteppedSpace *held = space_steppers.getptr(p_space.get_id());
    return (held != nullptr && held->stepper.is_valid())
        || gd::can_step_space(p_dimension);
}

bool NetwMultiplayer::predict_entity_can_step(
    const Ref<NetwEntity> &p_entity
) const {
    if (p_entity.is_null()) {
        return false;
    }
    const EntitySpace held = entity_space_of(p_entity);
    if (!held.space.is_valid()
        || !predict_space_can_step(held.space, held.dimension)) {
        return false;
    }
    return gd::can_flush_kinematic()
        || colliders::first_kinematic_body(p_entity->get_owner()) == nullptr;
}

void NetwMultiplayer::predict_step_space(
    const RID &p_space,
    double p_delta,
    int64_t p_tick,
    int p_dimension
) {
    const SteppedSpace *held = space_steppers.getptr(p_space.get_id());
    if (held != nullptr && held->stepper.is_valid()) {
        const Ref<NetwPhysicsStepper> stepper = held->stepper;
        const int dimension = held->dimension;
        stepper->step(p_space, p_delta);
        stepper->flush_queries(p_space, dimension);
        stepper->snapshot(p_space, p_tick);
        return;
    }
    const int dimension = held != nullptr ? held->dimension : p_dimension;
    if (!gd::can_step_space(dimension)) {
        return;
    }
    gd::step_space(p_space, dimension, p_delta);
    gd::flush_space_queries(p_space, dimension);
}

void NetwMultiplayer::predict_held_spaces(LocalVector<RID> &r_spaces) const {
    r_spaces.clear();
    for (const KeyValue<int64_t, SteppedSpace> &row : space_steppers) {
        if (row.value.inactive) {
            r_spaces.push_back(row.value.space);
        }
    }
}

bool NetwMultiplayer::predict_space_has_stepped_member(
    const RID &p_space
) const {
    for (const KeyValue<int64_t, NetwPredictSlotEngine *> &row :
         predict_engines) {
        const Ref<NetwEntity> member = wrapper_for_id(row.key);
        if (member.is_null()) {
            continue;
        }
        const sim::Decl *simulation = sim_find_declaration(member.ptr());
        const bool stepped = row.value != nullptr
            && row.value->runs_in_pass(NetwSimulation::SCHEDULE_STEPPED);
        const bool declared = simulation != nullptr
            && simulation->declaration.schedule == sim::Schedule::STEPPED;
        if ((stepped || declared) && entity_space_of(member).space == p_space) {
            return true;
        }
    }
    return false;
}

void NetwMultiplayer::predict_release_empty_holds() {
    LocalVector<int64_t> built_in;
    for (KeyValue<int64_t, SteppedSpace> &row : space_steppers) {
        SteppedSpace &held = row.value;
        if (!held.inactive || predict_space_has_stepped_member(held.space)) {
            continue;
        }
        held.inactive = false;
        space_set_active({held.space, held.dimension}, true);
        if (held.stepper.is_null()) {
            built_in.push_back(row.key);
        }
    }
    for (const int64_t key : built_in) {
        space_steppers.erase(key);
    }
}

bool NetwMultiplayer::predict_space_is_stepped(const RID &p_space) const {
    return space_steppers.has(p_space.get_id());
}

void NetwMultiplayer::predict_engine_install(
    const RID &p_entity,
    NetwPredictSlotEngine *p_engine
) {
    if (p_engine == nullptr || predict_engines.has(p_entity.get_id())) {
        return;
    }
    predict_engines.insert(p_entity.get_id(), p_engine);
}

NetwPredictSlotEngine *NetwMultiplayer::predict_engine_release(
    const RID &p_entity
) {
    NetwPredictSlotEngine *const *held
        = predict_engines.getptr(p_entity.get_id());
    if (held == nullptr) {
        return nullptr;
    }
    NetwPredictSlotEngine *engine = *held;
    predict_engines.erase(p_entity.get_id());
    return engine;
}

NetwPredictSlotEngine *NetwMultiplayer::predict_engine_for(
    const RID &p_entity
) const {
    NetwPredictSlotEngine *const *held
        = predict_engines.getptr(p_entity.get_id());
    return held != nullptr ? *held : nullptr;
}

bool NetwMultiplayer::predict_is_declared(const RID &p_entity) const {
    return predict_engine_for(p_entity) != nullptr;
}

void NetwMultiplayer::predict_follow_control(const RID &p_entity) {
    NetwPredictSlotEngine *engine = predict_engine_for(p_entity);
    if (engine != nullptr && engine->follows_control) {
        engine->rewire();
    }
}

void NetwMultiplayer::predict_retire(NetwPredictSlotEngine *p_engine) {
    predict_retired.push_back(p_engine);
    if (is_inside_step() || predict_free_queued) {
        return;
    }
    predict_free_queued = true;
    callable_mp(this, &NetwMultiplayer::predict_free_deferred).call_deferred();
}

void NetwMultiplayer::predict_free_deferred() {
    predict_free_queued = false;
    predict_free_retired();
}

void NetwMultiplayer::predict_free_retired() {
    if (is_inside_step()) {
        return;
    }
    while (!predict_retired.is_empty()) {
        NetwPredictSlotEngine *engine
            = predict_retired[predict_retired.size() - 1];
        predict_retired.resize(predict_retired.size() - 1);
        godot::memdelete(engine);
    }
    predict_free_queued = false;
}

TypedArray<NetwEntity> NetwMultiplayer::predict_engine_entities() const {
    TypedArray<NetwEntity> out;
    for (const KeyValue<int64_t, NetwPredictSlotEngine *> &row :
         predict_engines) {
        const Ref<NetwEntity> wrapper = wrapper_for_id(row.key);
        if (wrapper.is_valid()) {
            out.push_back(wrapper);
        }
    }
    return out;
}

sim::Row &NetwMultiplayer::sim_row(const RID &p_entity) {
    return sim_rows.ensure(p_entity);
}

const sim::Row *NetwMultiplayer::sim_row_of(const RID &p_entity) const {
    return sim_rows.row_of(p_entity);
}

sim::Mode NetwMultiplayer::sim_resolve(
    const RID &p_entity,
    const sim::Facts &p_facts
) {
    return sim_rows.resolve(p_entity, p_facts);
}

void NetwMultiplayer::sim_follow_server() {
    const LocalVector<RID> moved = sim_rows.follow_server(is_host());
    for (const RID &entity : moved) {
        if (NetwPredictSlotEngine *engine = predict_engine_for(entity)) {
            engine->rewire();
        } else {
            sim_settle_body_of(entity);
            sim_announce(entity);
        }
    }
}

NetwPredictRunner *NetwMultiplayer::predict_runner_seated() {
    return &predict_runner;
}

namespace {

const char *SIG_NODE_ADDED = "node_added";

} // namespace

void NetwMultiplayer::predict_history_record(
    int64_t p_tick,
    int p_schedule,
    bool p_include_unregistered
) {
    NETW_STAGE(LAG_COMPENSATION, "Predict history record", colors::PREDICTION);
    if (!lagcomp_configured || !is_host()) {
        return;
    }
    for (uint32_t seat = 0; seat < lagcomp_core.seat_capacity(); ++seat) {
        const int64_t at = lagcomp_core.slot_at_seat(seat);
        const Ref<RefCounted> carrier = lagcomp_core.entity_at(at);
        NetwEntity *entity = Object::cast_to<NetwEntity>(carrier.ptr());
        if (entity == nullptr) {
            continue;
        }
        Node *owner = entity->get_owner();
        if (owner == nullptr) {
            continue;
        }
        if (owner->is_inside_tree() && !owner->can_process()) {
            continue;
        }
        const repl::PropertyBindingPtr state = entity->get_state_binding();
        if (state == nullptr) {
            continue;
        }
        int64_t record_tick = p_tick;
        NetwPredictSlotEngine *engine
            = predict_engine_for(entity->get_rid_handle());
        if (engine != nullptr) {
            if (!engine->runs_in_pass(p_schedule)) {
                continue;
            }
            record_tick = engine->history_record_tick(p_tick);
            if (record_tick < 0 && !engine->consumed_unslotted_transition()) {
                NETW_TRACE(
                    sys::PREDICTION,
                    "Tick %d has no history entry an acknowledgement can use.",
                    int(p_tick)
                );
                continue;
            }
        } else if (!p_include_unregistered) {
            continue;
        }
        const Dictionary payload
            = state->canonicalize_payload(state->snapshot_payload());
        repl::PropertySet *declared = state->get_set();
        if (record_tick >= 0 && declared != nullptr) {
            lagcomp_core.timeline_record_coded(
                at,
                record_tick,
                payload,
                declared->get_codec()
            );
            record_rewind_parts(at, entity, state, record_tick);
        }
        if (engine != nullptr) {
            engine->finalize_recorded_state();
        }
    }
}

bool NetwMultiplayer::lagcomp_is_configured() const {
    return lagcomp_configured;
}

void NetwMultiplayer::set_lagcomp_configured(bool p_configured) {
    lagcomp_configured = p_configured;
}

Callable NetwMultiplayer::lagcomp_node_watcher() {
    return callable_mp(this, &NetwMultiplayer::observe_node_added);
}

void NetwMultiplayer::lagcomp_observe_mounted(Node *p_node) {
    if (p_node == nullptr) {
        return;
    }
    observe_node_added(p_node);
    const int children = p_node->get_child_count();
    for (int at = 0; at < children; ++at) {
        lagcomp_observe_mounted(p_node->get_child(at));
    }
}

void NetwMultiplayer::lagcomp_arm() {
    if (lagcomp_configured) {
        return;
    }
    set_lagcomp_configured(true);
    SceneTree *tree = gd::scene_tree();
    const Callable watcher = lagcomp_node_watcher();
    if (tree != nullptr && !tree->is_connected(SIG_NODE_ADDED, watcher)) {
        tree->connect(SIG_NODE_ADDED, watcher);
        lagcomp_observe_mounted(tree->get_root());
    }
    wire_lagcomp_service();
}

void NetwMultiplayer::lagcomp_release() {
    set_lagcomp_configured(false);
    SceneTree *tree = gd::scene_tree();
    const Callable watcher = lagcomp_node_watcher();
    if (tree != nullptr && tree->is_connected(SIG_NODE_ADDED, watcher)) {
        tree->disconnect(SIG_NODE_ADDED, watcher);
    }
    predict_tap_close();
}

void NetwMultiplayer::wire_lagcomp_service() {
    if (!clock_engine().get_configured() || !lagcomp_configured) {
        return;
    }
    ReplicationCore *plane = (get_replication_plane());
    if (plane == nullptr) {
        NETW_TRACE(
            sys::PREDICTION,
            "Action and prediction channels were not set up, because "
            "replication is not running."
        );
        return;
    }
    plane->register_channel(
        declared_channel("ACTION"),
        callable_mp(this, &NetwMultiplayer::action_receive_carrier),
        false
    );
    plane->register_channel(
        declared_channel("PREDICT_COMMAND"),
        callable_mp(this, &NetwMultiplayer::predict_receive_command_carrier),
        false
    );
    plane->register_channel(
        declared_channel("PREDICT_ACK"),
        callable_mp(this, &NetwMultiplayer::predict_receive_ack_carrier),
        false
    );
    plane->register_channel(
        declared_channel("PREDICT_RELAY"),
        callable_mp(this, &NetwMultiplayer::predict_receive_relay_carrier),
        false
    );
    plane->register_channel(
        declared_channel("PREDICT_RELAY_REQUEST"),
        callable_mp(
            this,
            &NetwMultiplayer::predict_receive_relay_request_carrier
        ),
        false
    );
}

void NetwMultiplayer::predict_receive_command_carrier(
    const Ref<NetwEntity> &p_entity,
    const PackedByteArray &p_payload,
    int64_t
) {
    NETW_ZONE_NC("Predict command carrier", colors::PREDICTION);
    NetwPredictSlotEngine *engine = predict_engine_for(
        p_entity.is_valid() ? p_entity->get_rid_handle() : RID()
    );
    if (engine != nullptr) {
        engine->receive_command_frame(p_payload);
    }
}

void NetwMultiplayer::predict_receive_relay_carrier(
    const Ref<NetwEntity> &p_entity,
    const PackedByteArray &p_payload,
    int64_t
) {
    NETW_ZONE_NC("Predict relay carrier", colors::PREDICTION);
    NetwPredictSlotEngine *engine = predict_engine_for(
        p_entity.is_valid() ? p_entity->get_rid_handle() : RID()
    );
    if (engine != nullptr) {
        engine->receive_relayed_command_frame(p_payload);
    }
}

void NetwMultiplayer::predict_receive_ack_carrier(
    const Ref<NetwEntity> &p_entity,
    const PackedByteArray &p_payload,
    int64_t
) {
    NETW_ZONE_NC("Predict ack carrier", colors::PREDICTION);
    NetwPredictSlotEngine *engine = predict_engine_for(
        p_entity.is_valid() ? p_entity->get_rid_handle() : RID()
    );
    if (engine != nullptr) {
        engine->receive_ack_frame(p_payload);
    }
}

void NetwMultiplayer::predict_receive_relay_request_carrier(
    const Ref<NetwEntity> &p_entity,
    const PackedByteArray &p_payload,
    int64_t p_sender
) {
    const int request = relay_request_of(p_payload);
    if (request < 0) {
        NETW_TRACE(
            sys::PREDICTION,
            "Peer %d sent a relay request that could not be read.",
            int(p_sender)
        );
        return;
    }
    relay_subscribe(p_entity, p_sender, request == 1);
}

void NetwMultiplayer::predict_relay_command_frame(
    const Ref<NetwEntity> &p_entity,
    const PackedByteArray &p_payload,
    int64_t p_author
) {
    if (!is_host() || p_entity.is_null()) {
        return;
    }
    const RID handle = p_entity->get_rid_handle();
    const PackedInt64Array subscribers = relay_peers(int64_t(handle.get_id()));
    if (subscribers.is_empty()) {
        return;
    }
    const int64_t route = liveness_route_of(p_entity.ptr());
    if (route <= 0) {
        NETW_TRACE(
            sys::PREDICTION,
            "A relayed input found no live route for entity %d.",
            int(handle.get_id())
        );
        return;
    }
    ReplicationCore *plane = (get_replication_plane());
    if (plane == nullptr) {
        return;
    }
    const int64_t channel = declared_channel("PREDICT_RELAY");
    for (int at = 0; at < subscribers.size(); at++) {
        const int64_t peer = subscribers[at];
        if (peer == p_author || !interest_admits(handle, peer)) {
            continue;
        }
        plane->send_to(
            peer,
            route,
            channel,
            p_payload,
            false,
            0,
            String(),
            true
        );
    }
}

void NetwMultiplayer::action_receive_carrier(
    const Ref<NetwEntity> &p_entity,
    const PackedByteArray &p_payload,
    int64_t p_sender
) {
    if (!is_host() || p_entity.is_null() || p_entity->get_owner() == nullptr) {
        return;
    }
    if (!lagcomp_configured) {
        fault::raise(
            this,
            NETW_FAULT_ACTION_UNREADY,
            {String(p_entity->get_owner()->get_name()),
             String(),
             String(),
             p_sender},
            gd::instance_id(p_entity->get_owner())
        );
        return;
    }
    if (p_sender != SERVER_PEER && p_sender != p_entity->get_controller()) {
        fault::raise(
            this,
            NETW_FAULT_RPC_REFUSED,
            {"{NetwAction.request}",
             String(p_entity->get_owner()->get_name()),
             "the server or the controller of the entity",
             p_sender},
            gd::instance_id(p_entity->get_owner())
        );
        return;
    }
    session::ActionRequest body;
    if (!session::frame_read(p_payload, body)) {
        NETW_TRACE(
            sys::PREDICTION,
            "Peer %d sent an action that did not decode fully.",
            int(p_sender)
        );
        return;
    }
    NETW_NETWORK_PROFILE(network_profiler.name_arrival(profiler_member(
        p_entity->get_route(),
        wire::builtin_channel("ACTION"),
        p_entity->get_owner(),
        body.method
    )));
    const Variant carried = gd::bytes_to_var(body.data);
    if (carried.get_type() != Variant::ARRAY) {
        NETW_TRACE(
            sys::PREDICTION,
            "Peer %d sent action arguments that are not an Array.",
            int(p_sender)
        );
        return;
    }
    submit_action(
        p_entity->get_route(),
        body.method,
        body.view_tick,
        Array(carried),
        body.key,
        int(body.timing),
        p_sender
    );
}

void NetwMultiplayer::lagcomp_action_deny(
    int64_t p_requester,
    const StringName &p_key
) {
    effect_deny_action(p_requester, p_key);
    effect_drain();
}

void NetwMultiplayer::effect_deny_action(
    int64_t p_requester,
    const StringName &p_key
) {
    if (action_requester_is_remote(p_requester)) {
        NETW_TRACE(
            sys::PREDICTION,
            "Action %s was refused, so peer %d is told to undo it.",
            String(p_key).utf8().get_data(),
            int(p_requester)
        );
        send_to(
            p_requester,
            0,
            wire::builtin_channel("LAGCOMP_DENY"),
            session::frame_write(session::DenyKey{p_key}),
            true,
            0,
            String(),
            false
        );
        return;
    }
    NETW_TRACE(
        sys::PREDICTION,
        "Action %s was refused on this peer, so its effect is undone.",
        String(p_key).utf8().get_data()
    );
    effect_discard(p_key);
}

bool NetwMultiplayer::action_requester_is_remote(int64_t p_requester) {
    const bool transport = lagcomp_configured
        && NETW_API_VIRTUAL(get_multiplayer_peer)().is_valid();
    const int64_t local_peer
        = transport ? int64_t(NETW_API_VIRTUAL(get_unique_id)()) : 0;
    return transport && p_requester != 0 && p_requester != local_peer
        && NETW_API_VIRTUAL(get_peer_ids)().has(int32_t(p_requester));
}

void NetwMultiplayer::effect_confirm_action(
    int64_t p_requester,
    const StringName &p_key
) {
    if (!action_requester_is_remote(p_requester)) {
        effect_adopt(p_key);
        return;
    }
    send_to(
        p_requester,
        0,
        wire::builtin_channel("LAGCOMP_CONFIRM"),
        session::frame_write(session::ConfirmKey{p_key}),
        true,
        0,
        String(),
        false
    );
}

void NetwMultiplayer::predict_history_record_tick(int64_t p_tick) {
    predict_history_record(p_tick, NetwSimulation::SCHEDULE_TICK, true);
}

void NetwMultiplayer::tick_step(double p_delta, int64_t p_tick) {
    NETW_STAGE(SIMULATION, "Simulation tick", colors::PREDICTION);
    sim_contact_pass(p_tick);
    sim_drain_installs();
    sim_run(sim::Schedule::TICK, p_delta, p_tick);
    if (!lagcomp_configured) {
        return;
    }
    drain_pending_actions(p_tick);
    predict_runner.tick_step(tick_timing(p_tick, p_delta));
    predict_history_record_tick(p_tick);
}

void NetwMultiplayer::before_frame_step() {
    NETW_STAGE(SIMULATION, "Frame start step", colors::PREDICTION);
    if (!lagcomp_configured) {
        return;
    }
    physics_frame_advance();
    predict_runner.before_frame_step();
    predict_history_record_frame(clock_engine().get_tick());
    predict_tap_pump();
}

void NetwMultiplayer::frame_step() {
    NETW_STAGE(SIMULATION, "Frame step", colors::PREDICTION);
    const ClockEngine &clock = clock_engine();
    if (clock.is_simulating()) {
        sim_run(sim::Schedule::FRAME, clock.ticktime(), clock.get_tick() - 1);
    }
    if (!lagcomp_configured) {
        return;
    }
    predict_runner.frame_step(frame_timing());
    simulation_gate_apply();
    ReplicationCore *plane = (get_replication_plane());
    if (plane == nullptr) {
        NETW_TRACE(
            sys::PREDICTION,
            "The frame ended with nothing sent, because replication is not "
            "running."
        );
        return;
    }
    plane->on_frame_end();
}

void NetwMultiplayer::predict_history_record_frame(int64_t p_tick) {
    predict_history_record(p_tick, NetwSimulation::SCHEDULE_FRAME, false);
}

namespace {

enum PredictCensusFact {
    CENSUS_CORRECTIONS,
    CENSUS_INSTALLS,
    CENSUS_MAX_REPLAY_DEPTH,
    CENSUS_CONSUMED,
    CENSUS_MISSING,
    CENSUS_FOLDED,
    CENSUS_JOINT_PASSES,
    CENSUS_JOINT_MEMBERS,
    CENSUS_CELLS_RELAYED,
    CENSUS_CELLS_SUBSTITUTED,
    CENSUS_HEAL_SNAPS,
    CENSUS_LINGER_HELD,
    CENSUS_COUNT,
};

const char *const CENSUS_NAMES[CENSUS_COUNT] = {
    "corrections",
    "installs",
    "max_replay_depth",
    "consumed",
    "missing",
    "folded",
    "joint_passes",
    "joint_members",
    "cells_relayed",
    "cells_substituted",
    "heal_snaps",
    "linger_held",
};

const int *predict_census_columns() {
    static int seated[CENSUS_COUNT];
    static bool resolved = false;
    if (resolved) {
        return seated;
    }
    const NetwPredictStats::Fact *facts = NetwPredictStats::facts();
    for (int at = 0; at < CENSUS_COUNT; ++at) {
        seated[at] = -1;
        for (int row = 0; row < NetwPredictStats::fact_count(); ++row) {
            if (String(facts[row].name) == String(CENSUS_NAMES[at])) {
                seated[at] = row;
                break;
            }
        }
    }
    resolved = true;
    return seated;
}

} // namespace

TypedArray<NetwEntity> NetwMultiplayer::predict_stepped_entities() const {
    TypedArray<NetwEntity> out;
    for (const KeyValue<int64_t, NetwPredictSlotEngine *> &row :
         predict_engines) {
        const Ref<NetwEntity> wrapper = wrapper_for_id(row.key);
        if (wrapper.is_null()) {
            continue;
        }
        const int64_t slot = prediction_engine.slot_of(wrapper);
        if (slot >= 0 && prediction_engine.registered_of(slot)) {
            out.push_back(wrapper);
        }
    }
    return out;
}

Dictionary NetwMultiplayer::predict_metrics() const {
    const int *column = predict_census_columns();
    const TypedArray<NetwEntity> stepped = predict_stepped_entities();
    int64_t total[CENSUS_COUNT] = {0};
    for (int at = 0; at < stepped.size(); ++at) {
        const Ref<NetwEntity> wrapper = stepped[at];
        if (wrapper.is_null()) {
            continue;
        }
        const predict::Decl *decl = predict_find_declaration(wrapper.ptr());
        if (decl == nullptr) {
            continue;
        }
        for (int fact = 0; fact < CENSUS_COUNT; ++fact) {
            const int64_t held = decl->facts.get_int_fact(column[fact]);
            if (fact == CENSUS_MAX_REPLAY_DEPTH
                || fact == CENSUS_JOINT_MEMBERS) {
                total[fact] = held > total[fact] ? held : total[fact];
            } else {
                total[fact] += held;
            }
        }
    }

    Dictionary joint;
    joint["joint_passes"] = total[CENSUS_JOINT_PASSES];
    joint["joint_members"] = total[CENSUS_JOINT_MEMBERS];
    joint["cells_relayed"] = total[CENSUS_CELLS_RELAYED];
    joint["cells_substituted"] = total[CENSUS_CELLS_SUBSTITUTED];
    joint["heal_snaps"] = total[CENSUS_HEAL_SNAPS];
    joint["linger_held"] = total[CENSUS_LINGER_HELD];

    Dictionary out;
    out["entities"] = int64_t(stepped.size());
    out["corrections"] = total[CENSUS_CORRECTIONS];
    out["installs"] = total[CENSUS_INSTALLS];
    out["max_replay_depth"] = total[CENSUS_MAX_REPLAY_DEPTH];
    out["consumed"] = total[CENSUS_CONSUMED];
    out["missing"] = total[CENSUS_MISSING];
    out["folded"] = total[CENSUS_FOLDED];
    out["joint"] = joint;
    out["timelines"] = lagcomp_core.timeline_registered();
    return out;
}

void NetwMultiplayer::advance_frame() {
    frame_counter += 1;
    NETW_NETWORK_PROFILE(profiler_publish());
}

void NetwMultiplayer::session_defer(
    const Callable &p_fn,
    const StringName &p_key
) {
    settle_queue.schedule(p_fn, p_key);
}

void NetwMultiplayer::session_defer_after(
    const Callable &p_fn,
    const StringName &p_key,
    int p_pumps
) {
    settle_queue.schedule_after(p_fn, p_key, p_pumps);
}

void NetwMultiplayer::settle_advance() {
    settle_queue.advance_windows();
}

void NetwMultiplayer::session_cancel_deferred(const StringName &p_key) {
    settle_queue.cancel(p_key);
}

PackedStringArray NetwMultiplayer::settle_drain() {
    return settle_queue.drain();
}

int NetwMultiplayer::settle_pending() const {
    return settle_queue.size();
}

bool NetwMultiplayer::settle_has_key(const StringName &p_key) const {
    return settle_queue.has(p_key);
}

int NetwMultiplayer::settle_max_passes() {
    return SettleQueue::MAX_PASSES;
}

void NetwMultiplayer::session_flush_deferred() {
    if (settle_drain().is_empty()) {
        return;
    }
    fault::raise(
        this,
        NETW_FAULT_SETTLE_BOUND,
        {String(), String(), String(), 0, settle_max_passes()}
    );
}

int64_t NetwMultiplayer::native_prediction_slot(
    const Ref<NetwEntity> &p_entity
) const {
    return prediction_engine.slot_of(p_entity);
}

void NetwMultiplayer::predict_reconcile_declaration(
    const Ref<NetwEntity> &p_entity
) {
    if (p_entity.is_null()) {
        return;
    }
    if (!predict_implied(p_entity.ptr())) {
        return;
    }
    predict_declare(p_entity->get_rid_handle());
}

void NetwMultiplayer::predict_readmit(const Ref<NetwEntity> &p_entity) {
    const RID entity = p_entity->get_rid_handle();
    if (predict_is_declared(entity) || predict_implied(p_entity.ptr())) {
        return;
    }
    Node *owner = p_entity->get_owner();
    const String named = owner != nullptr ? String(owner->get_path())
                                          : String(p_entity->get_name());
    if (p_entity->get_state_binding() == nullptr) {
        fault::raise(
            this,
            NETW_FAULT_PREDICTION_OFF,
            {named, String(), String(), 0, NETW_OFF_STATE_LOST},
            gd::instance_id(owner)
        );
        return;
    }
    const Error declared = predict_declare(entity);
    if (declared != OK) {
        fault::raise(
            this,
            NETW_FAULT_PREDICTION_OFF,
            {named, String(), String(), 0, NETW_OFF_LAGCOMP_FAILED},
            gd::instance_id(owner)
        );
    }
}

bool NetwMultiplayer::predict_lacks_state_rows(
    const Ref<NetwEntity> &p_entity
) const {
    return predict_find_declaration(p_entity.ptr()) != nullptr
        && sim_simulated(p_entity.ptr())
        && p_entity->get_state_binding() == nullptr;
}

bool NetwMultiplayer::predict_implied(const NetwEntity *p_wrapper) {
    return sim_simulated(p_wrapper)
        && p_wrapper->get_state_binding() != nullptr;
}

void NetwMultiplayer::predict_settle_kind(NetwEntity *p_wrapper) {
    NetwEntityRecord *record = record_of_wrapper(p_wrapper);
    if (record == nullptr) {
        return;
    }
    const predict::KindDefaults defaults
        = predict::kind_defaults(sim_physics_driven(p_wrapper));
    predict::Decl &decl = record->predict_declaration();
    if (!decl.wrote_repeat_missing_input) {
        decl.repeat_missing_input = defaults.repeat_missing_input;
    }
    if (!decl.wrote_teleport_threshold) {
        decl.teleport_threshold = defaults.teleport_threshold;
    }
    if (!defaults.restore_extrapolated
        && record->find_sim_declaration() == nullptr) {
        return;
    }
    sim::Declaration &declared = record->sim_declaration().declaration;
    if (!declared.wrote_restore) {
        declared.restore = defaults.restore_extrapolated
            ? sim::Restore::EXTRAPOLATED
            : sim::Restore::BUFFERED;
    }
}

void NetwMultiplayer::register_prediction(const Ref<NetwEntity> &p_entity) {
    NETW_ZONE_NC("Predict register", colors::PREDICTION);
    if (p_entity.is_null()) {
        return;
    }
    const RID entity = p_entity->get_rid_handle();
    if (predict_is_declared(entity)) {
        return;
    }
    predict_settle_kind(p_entity.ptr());
    sim_declare(p_entity);
    NetwPredictSlotEngine *engine = memnew(NetwPredictSlotEngine);
    predict_engine_install(entity, engine);
    prediction_engine.slot_register(p_entity);
    store_bind_entity(p_entity);
    const Ref<NetwPrediction> handle = p_entity->get_prediction();
    engine->follows_control = handle.is_valid();
    engine->attach(this, p_entity, &books);
}

void NetwMultiplayer::unregister_prediction(const Ref<NetwEntity> &p_entity) {
    NETW_ZONE_NC("Predict unregister", colors::PREDICTION);
    if (p_entity.is_null()) {
        return;
    }
    const RID entity = p_entity->get_rid_handle();
    predict_awaiting_config.erase(entity);
    NetwPredictSlotEngine *engine = predict_engine_release(entity);
    if (engine == nullptr) {
        return;
    }
    engine->release();
    sim_release_body(entity, true);
    sim_release_row(entity);
    sim_tell_mode(p_entity->get_record(), p_entity, sim::Mode::NONE);
    prediction_engine.slot_unregister(p_entity);
    relay_release(entity.get_id());
    predict_retire(engine);
}

Error NetwMultiplayer::predict_declare(const RID &p_entity) {
    const Ref<NetwEntity> wrapper = entity_get_view(p_entity);
    if (wrapper.is_null()) {
        NETW_TRACE(
            sys::PREDICTION,
            "Prediction was declared for %d, which has no entity.",
            int(p_entity.get_id())
        );
        return ERR_DOES_NOT_EXIST;
    }
    const Error required = lagcomp_require("prediction");
    if (required == ERR_BUSY) {
        if (predict_awaiting_config.find(p_entity) < 0) {
            predict_awaiting_config.push_back(p_entity);
        }
        declarations_changed();
        return OK;
    }
    if (required != OK) {
        return required;
    }
    register_prediction(wrapper);
    return OK;
}

void NetwMultiplayer::predict_undeclare(const RID &p_entity) {
    const Ref<NetwEntity> wrapper = entity_get_view(p_entity);
    if (wrapper.is_valid()) {
        NetwEntityRecord *const record = record_of(wrapper.ptr());
        NETW_BUG_COND(record == nullptr);
        record->set_predicted_at_departure(false);
        unregister_prediction(wrapper);
    }
}

Ref<NetwPrediction> NetwMultiplayer::prediction_handle(
    const RID &p_entity
) const {
    const Ref<NetwEntity> wrapper = entity_get_view(p_entity);
    return wrapper.is_valid() ? Ref<NetwPrediction>(wrapper->get_prediction())
                              : Ref<NetwPrediction>();
}

Error NetwMultiplayer::predict_set_param(
    const RID &p_entity,
    PredictParam p_param,
    const Variant &p_value
) {
    const Ref<NetwEntity> wrapper = entity_get_view(p_entity);
    if (wrapper.is_null()) {
        return ERR_DOES_NOT_EXIST;
    }
    return predict_write_param(wrapper.ptr(), p_param, p_value);
}

Variant NetwMultiplayer::predict_get_param(
    const RID &p_entity,
    PredictParam p_param
) {
    const Ref<NetwEntity> wrapper = entity_get_view(p_entity);
    return wrapper.is_valid() ? predict_read_param(wrapper.ptr(), p_param)
                              : Variant();
}

void NetwMultiplayer::predict_set_correction_path_callback(
    const RID &p_entity,
    const Callable &p_callback
) {
    const Ref<NetwEntity> wrapper = entity_get_view(p_entity);
    if (wrapper.is_valid()) {
        predict_write_correction_path(wrapper.ptr(), p_callback);
    }
}

void NetwMultiplayer::predict_set_input_callback(
    const RID &p_entity,
    const Callable &p_callback
) {
    const Ref<NetwEntity> wrapper = entity_get_view(p_entity);
    if (wrapper.is_valid()) {
        predict_write_input(wrapper.ptr(), p_callback);
    }
}

namespace {

const GameCall PREDICT_INPUT_CALL
    = {"NetwPrediction.predict_input",
       "func(entity: NetwEntity, tick: int) -> Dictionary",
       2};

const GameCall CORRECTION_PATH_CALL
    = {"NetwPrediction.correction_path_clear",
       "func(current: Dictionary, proposed: Dictionary) -> bool",
       2};

bool refuses_dead_callable(
    const NetwEntity *p_wrapper,
    const Callable &p_value,
    const char *p_member
) {
    if (p_value.is_null() || p_value.is_valid()) {
        return false;
    }
    fault::raise(
        nullptr,
        NETW_FAULT_BAD_CALLABLE,
        {String(p_member), String(), String("a method of a live object")},
        p_wrapper != nullptr ? uint64_t(gd::instance_id(p_wrapper->get_owner()))
                             : 0
    );
    return true;
}

} // namespace

NetwMultiplayer *NetwMultiplayer::entity_home(const NetwEntity *p_wrapper) {
    return p_wrapper != nullptr ? p_wrapper->home : nullptr;
}

NetwPredictSlotEngine *NetwMultiplayer::predict_engine_of(
    const NetwEntity *p_wrapper
) {
    NetwMultiplayer *home = entity_home(p_wrapper);
    return home != nullptr
        ? home->predict_engine_for(p_wrapper->get_rid_handle())
        : nullptr;
}

const predict::Decl *NetwMultiplayer::predict_find_declaration(
    const NetwEntity *p_wrapper
) {
    const NetwEntityRecord *record = record_of_wrapper(p_wrapper);
    return record != nullptr ? record->find_predict_declaration() : nullptr;
}

Error NetwMultiplayer::predict_write_param(
    NetwEntity *p_wrapper,
    PredictParam p_param,
    const Variant &p_value
) {
    NetwEntityRecord *record = record_of_wrapper(p_wrapper);
    if (record == nullptr) {
        return ERR_DOES_NOT_EXIST;
    }
    predict::Decl &decl = record->predict_declaration();
    NetwPredictSlotEngine *engine = predict_engine_of(p_wrapper);
    switch (p_param) {
        case PREDICT_PARAM_REPEAT_MISSING_INPUT:
            decl.repeat_missing_input = bool(p_value);
            decl.wrote_repeat_missing_input = true;
            return OK;
        case PREDICT_PARAM_MODE:
            decl.mode = int(p_value);
            if (engine != nullptr) {
                engine->reconfigure();
            }
            engine = predict_engine_of(p_wrapper);
            if (engine != nullptr) {
                engine->rewire();
            }
            return OK;
        case PREDICT_PARAM_TELEPORT_THRESHOLD:
            decl.teleport_threshold = std::max(0.0, double(p_value));
            decl.wrote_teleport_threshold = true;
            break;
        case PREDICT_PARAM_CORRECTION_THRESHOLD:
            decl.correction_threshold = std::max(0.0, double(p_value));
            break;
        case PREDICT_PARAM_PAUSE_ON_UNPREDICTED_CONTACT:
            decl.pause_on_unpredicted_contact = bool(p_value);
            if (engine != nullptr && engine->seated_pool() != nullptr) {
                engine->seated_pool()->note_breach_source(
                    engine->native_slot(),
                    StringName("code")
                );
            }
            return OK;
        case PREDICT_PARAM_CORRECTION_COOLDOWN_TICKS:
            decl.correction_cooldown_ticks = std::max(0, int(p_value));
            break;
        case PREDICT_PARAM_MAX_INPUTS_PER_TICK:
            decl.max_inputs_per_tick = int(p_value);
            return OK;
        case PREDICT_PARAM_MAX_INPUT_LAG_TICKS:
            decl.max_input_lag_ticks = std::max(0, int(p_value));
            return OK;
        case PREDICT_PARAM_INPUT_BUFFER_TICKS:
            decl.input_buffer_ticks = int(p_value);
            return OK;
        case PREDICT_PARAM_ROLLBACK_TOGETHER:
            decl.rollback_together = bool(p_value);
            decl.wrote_rollback_together = true;
            break;
        case PREDICT_PARAM_WORLD_VERSION:
            decl.world_version = int64_t(p_value);
            if (engine != nullptr && engine->seated_pool() != nullptr) {
                engine->seated_pool()->set_declared_epoch(
                    engine->native_slot(),
                    decl.world_version
                );
            }
            return OK;
        default:
            fault::raise(
                entity_home(p_wrapper),
                NETW_FAULT_BAD_VALUE,
                {"A prediction setting",
                 vformat("setting %d", int(p_param)),
                 "a prediction setting that exists"}
            );
            return ERR_INVALID_PARAMETER;
    }
    if (engine != nullptr) {
        engine->reconfigure();
    }
    return OK;
}

Variant NetwMultiplayer::predict_read_param(
    const NetwEntity *p_wrapper,
    PredictParam p_param
) {
    const predict::Decl *found = predict_find_declaration(p_wrapper);
    const predict::Decl fallback;
    const predict::Decl &decl = found != nullptr ? *found : fallback;
    switch (p_param) {
        case PREDICT_PARAM_REPEAT_MISSING_INPUT:
            return decl.repeat_missing_input;
        case PREDICT_PARAM_MODE:
            return decl.mode;
        case PREDICT_PARAM_TELEPORT_THRESHOLD:
            return decl.teleport_threshold;
        case PREDICT_PARAM_CORRECTION_THRESHOLD:
            return decl.correction_threshold;
        case PREDICT_PARAM_PAUSE_ON_UNPREDICTED_CONTACT:
            return decl.pause_on_unpredicted_contact;
        case PREDICT_PARAM_CORRECTION_COOLDOWN_TICKS:
            return decl.correction_cooldown_ticks;
        case PREDICT_PARAM_MAX_INPUTS_PER_TICK:
            return decl.max_inputs_per_tick;
        case PREDICT_PARAM_MAX_INPUT_LAG_TICKS:
            return decl.max_input_lag_ticks;
        case PREDICT_PARAM_INPUT_BUFFER_TICKS:
            return decl.input_buffer_ticks;
        case PREDICT_PARAM_ROLLBACK_TOGETHER:
            return predict_rolls_back_together(p_wrapper);
        case PREDICT_PARAM_WORLD_VERSION:
            return decl.world_version;
        case PREDICT_PARAM_CORRECTION_COUNT:
            return decl.facts.get_int_fact(NetwPredictStats::FACT_CORRECTIONS);
        default:
            return Variant();
    }
}

void NetwMultiplayer::predict_write_correction_path(
    NetwEntity *p_wrapper,
    const Callable &p_callback
) {
    NetwEntityRecord *record = record_of_wrapper(p_wrapper);
    if (record == nullptr
        || refuses_dead_callable(
            p_wrapper,
            p_callback,
            "{NetwPrediction.correction_path_clear}"
        )) {
        return;
    }
    game_call_declared(p_callback, CORRECTION_PATH_CALL);
    record->predict_declaration().correction_path = p_callback;
}

void NetwMultiplayer::predict_write_input(
    NetwEntity *p_wrapper,
    const Callable &p_callback
) {
    NetwEntityRecord *record = record_of_wrapper(p_wrapper);
    if (record == nullptr) {
        return;
    }
    game_call_declared(p_callback, PREDICT_INPUT_CALL);
    record->predict_declaration().input = p_callback;
}

NetwSimulation::Schedule NetwMultiplayer::predict_schedule_of(
    const NetwEntity *p_wrapper
) {
    if (p_wrapper == nullptr) {
        return NetwSimulation::SCHEDULE_TICK;
    }
    const sim::Decl *found = sim_find_declaration(p_wrapper);
    const sim::Declaration undeclared;
    const sim::Declaration &declared
        = found != nullptr ? found->declaration : undeclared;
    if (declared.schedule != sim::Schedule::AUTO) {
        return NetwSimulation::Schedule(int(declared.schedule));
    }
    if (!sim_physics_driven(p_wrapper)) {
        return NetwSimulation::SCHEDULE_TICK;
    }
    NetwMultiplayer *session
        = NetwMultiplayer::session_holding(p_wrapper->get_owner());
    return session != nullptr
            && session->predict_entity_can_step(
                Ref<NetwEntity>(const_cast<NetwEntity *>(p_wrapper))
            )
        ? NetwSimulation::SCHEDULE_STEPPED
        : NetwSimulation::SCHEDULE_FRAME;
}

bool NetwMultiplayer::predict_body_solves(const NetwEntity *p_wrapper) {
    return sim_physics_driven(p_wrapper);
}

NetwPrediction::Mode NetwMultiplayer::predict_resolve_mode(
    const NetwEntity *p_wrapper
) {
    const bool solves = predict_body_solves(p_wrapper);
    const int schedule = predict_schedule_of(p_wrapper);
    const predict::Decl *decl = predict_find_declaration(p_wrapper);
    const int declared
        = decl != nullptr ? decl->mode : int(NetwPrediction::MODE_AUTO);
    return NetwPrediction::Mode(predict::integrable_mode(
        predict::resolve_mode(declared, solves, schedule),
        schedule,
        solves
    ));
}

bool NetwMultiplayer::predict_rolls_back_together(const NetwEntity *p_wrapper) {
    const predict::Decl *decl = predict_find_declaration(p_wrapper);
    if (decl != nullptr && decl->wrote_rollback_together) {
        return decl->rollback_together;
    }
    if (p_wrapper == nullptr
        || predict_schedule_of(p_wrapper) != NetwSimulation::SCHEDULE_STEPPED
        || !sim_physics_driven(p_wrapper)) {
        return false;
    }
    NetwMultiplayer *session
        = NetwMultiplayer::session_holding(p_wrapper->get_owner());
    return session != nullptr
        && session->predict_entity_can_step(
            Ref<NetwEntity>(const_cast<NetwEntity *>(p_wrapper))
        );
}

void NetwMultiplayer::predict_restate(const NetwEntity *p_wrapper) {
    NetwPredictSlotEngine *engine = predict_engine_of(p_wrapper);
    if (engine != nullptr) {
        engine->reconfigure();
    }
}

void NetwMultiplayer::predict_step_changed(const NetwEntity *p_wrapper) {
    NetwPredictSlotEngine *engine = predict_engine_of(p_wrapper);
    if (engine != nullptr) {
        engine->push_simulate();
    }
}

Error NetwMultiplayer::predict_bind_owner(
    const RID &p_entity,
    Object *p_owner
) {
    const Ref<NetwEntity> wrapper = entity_get_view(p_entity);
    if (wrapper.is_null()) {
        return ERR_DOES_NOT_EXIST;
    }
    if (p_owner == nullptr) {
        return ERR_INVALID_DATA;
    }
    if (!prediction_engine.slot_bind_owner(wrapper, p_owner)) {
        return ERR_DOES_NOT_EXIST;
    }
    const sim::Decl *declared = sim_find_declaration(wrapper.ptr());
    if ((declared == nullptr || !declared->declaration.step.is_valid())
        && p_owner->has_method(StringName("_network_process"))) {
        sim_adopt_step(
            wrapper.ptr(),
            Callable(p_owner, StringName("_network_process"))
        );
    }
    return OK;
}

void NetwMultiplayer::predict_unbind_owner(const RID &p_entity) {
    const Ref<NetwEntity> wrapper = entity_get_view(p_entity);
    if (wrapper.is_valid()) {
        prediction_engine.slot_unbind_owner(wrapper);
    }
}

void NetwMultiplayer::predict_notify_collision(const RID &p_entity) {
    const Ref<NetwEntity> wrapper = entity_get_view(p_entity);
    const predict::Decl *decl = predict_find_declaration(wrapper.ptr());
    if (decl == nullptr) {
        return;
    }
    const int64_t slot = prediction_engine.slot_of(wrapper);
    if (slot < 0) {
        return;
    }
    prediction_engine.notify_collision(
        slot,
        prediction_engine.samples_contacts(slot),
        decl->correction_cooldown_ticks
    );
}

Error NetwMultiplayer::lagcomp_timeline_declare(const RID &p_entity) {
    const Ref<NetwEntity> wrapper = entity_get_view(p_entity);
    if (wrapper.is_null()) {
        return ERR_DOES_NOT_EXIST;
    }
    register_rewind(wrapper);
    return OK;
}

void NetwMultiplayer::lagcomp_timeline_undeclare(const RID &p_entity) {
    const Ref<NetwEntity> wrapper = wrapper_for_id(int64_t(p_entity.get_id()));
    if (wrapper.is_valid()) {
        lagcomp_core.timeline_unregister(wrapper);
    }
}

void NetwMultiplayer::predict_report_joint_refusal(int64_t p_entity_id) {
    const Ref<NetwEntity> wrapper = wrapper_for_id(p_entity_id);
    if (wrapper.is_null()) {
        return;
    }
    const int64_t slot = prediction_engine.slot_of(wrapper);
    if (slot >= 0) {
        prediction_engine.report_joint_refusal(slot);
    }
}

static const char *OBSERVE_KEY_PREFIX = "lagcomp-observe-node?";

void NetwMultiplayer::settle_observe(Node *p_node) {
    if (p_node == nullptr) {
        return;
    }
    const int64_t instance = int64_t(p_node->get_instance_id());
    session_defer(
        callable_mp(this, &NetwMultiplayer::observe_node_entity_ref)
            .bind(gd::weak_ref(p_node)),
        StringName(String(OBSERVE_KEY_PREFIX) + String::num_int64(instance))
    );
}

} // namespace netw
