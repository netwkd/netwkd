#include "netw/api/netw_multiplayer.hpp"

#include "godot/callable.hpp"
#include "godot/node.hpp"
#include "godot/object.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/predict_slot_engine.hpp"
#include "netw/log.hpp"
#include "netw/sim/body.hpp"
#include "netw/sim/row.hpp"
#include "netw/subsystems.hpp"

using namespace godot;

namespace netw {

namespace {

const StringName &network_process_name() {
    static const StringName name("_network_process");
    return name;
}

} // namespace

void NetwMultiplayer::sim_run(
    sim::Schedule p_phase,
    double p_delta,
    int64_t p_tick
) {
    NETW_ZONE_NC("Entity steps", colors::PREDICTION);
    sim_rows.stepping(sim_stepping);
    NETW_ZONE_VALUE(sim_stepping.size());
#if defined(NETW_TESTS)
    idle_work.runs += 1;
    idle_work.walked += sim_stepping.size();
#endif
    for (const RID &entity : sim_stepping) {
        NETW_ZONE_LEAF_F(
            "Entity step",
            colors::PREDICTION,
            profile::SUBSYSTEM_ENTITY,
            "entity %lld",
            liveness_core.is_valid()
                ? static_cast<long long>(liveness_core->route_of(entity))
                : 0LL
        );
        sim_step(entity, p_phase, p_delta, p_tick);
    }
}

NetwSimulation::Schedule NetwMultiplayer::simulation_get_schedule(
    const RID &p_entity
) {
    const Ref<NetwEntity> entity = entity_get_view(p_entity);
    if (entity.is_null()) {
        return NetwSimulation::SCHEDULE_TICK;
    }
    if (NetwPredictSlotEngine *engine = predict_engine_for(p_entity)) {
        return NetwSimulation::Schedule(engine->schedule());
    }
    const sim::Row *row = sim_rows.row_of(p_entity);
    const sim::Decl *found = sim_find_declaration(entity.ptr());
    const sim::Declaration undeclared;
    const sim::Declaration &declared = row != nullptr ? row->declaration
        : found != nullptr                            ? found->declaration
                                                      : undeclared;
    Node *owner = entity->get_owner();
    const bool has_body = row != nullptr && row->bodies.recorded
        ? !row->bodies.held.is_empty()
        : (owner != nullptr && sim::is_solver_body(owner))
            || !declared.bodies.is_empty();
    return NetwSimulation::Schedule(
        int(sim::resolved_schedule(declared.schedule, has_body))
    );
}

sim::Schedule NetwMultiplayer::sim_runs_on(sim::Row &r_row, Node *p_owner) {
    const bool has_body = r_row.bodies.recorded
        ? !r_row.bodies.held.is_empty()
        : sim::is_solver_body(p_owner) || !r_row.declaration.bodies.is_empty();
    if (r_row.declaration.schedule == sim::Schedule::STEPPED
        && !r_row.stepped_refused) {
        r_row.stepped_refused = true;
        fault::raise(
            this,
            NETW_FAULT_STEPPED_UNPREDICTED,
            {String(p_owner->get_name())},
            gd::instance_id(p_owner)
        );
    }
    return sim::resolved_schedule(r_row.declaration.schedule, has_body);
}

void NetwMultiplayer::sim_step(
    const RID &p_entity,
    sim::Schedule p_phase,
    double p_delta,
    int64_t p_tick
) {
    if (predict_engine_for(p_entity) != nullptr) {
        return;
    }
    const Ref<NetwEntity> entity = entity_get_view(p_entity);
    if (entity.is_null()) {
        sim_release_row(p_entity);
        return;
    }
    Node *owner = entity->get_owner();
    sim::Row *row = sim_rows.peek(p_entity);
    if (row == nullptr || owner == nullptr || !owner->is_inside_tree()) {
        return;
    }
    const sim::Mode mode = row->bodies.recorded
        ? row->mode
        : sim_rows.resolve(p_entity, sim_body_facts(entity));
    sim_announce(p_entity);
    if (!sim::executes(mode) || sim_runs_on(*row, owner) != p_phase) {
        return;
    }
    if (p_phase == sim::Schedule::FRAME) {
        if (p_tick <= row->last_frame_tick) {
            return;
        }
        row->last_frame_tick = p_tick;
    }
    Callable step = row->declaration.step;
    if (!step.is_valid()) {
        if (!owner->has_method(network_process_name())) {
            return;
        }
        step = Callable(owner, network_process_name());
    }
    step.call(p_delta, p_tick, sim::leads(mode));
}

} // namespace netw
