#include "netw/api/netw_multiplayer.hpp"

#include "godot/node.hpp"
#include "godot/object.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/entity_record.hpp"
#include "netw/api/simulation.hpp"
#include "netw/fault_book.hpp"
#include "netw/game_call.hpp"
#include "netw/sim/row.hpp"
#include "netw/sim/select.hpp"

using namespace godot;

namespace netw {

namespace {

const GameCall SIMULATION_STEP
    = {"NetwSimulation.step",
       "func(delta: float, tick: int, is_fresh: bool) -> void",
       3};

const StringName &network_tick_name() {
    static const StringName name("_network_tick");
    return name;
}

String entity_name(const NetwEntity *p_entity) {
    const String named = String(p_entity->get_name());
    if (!named.is_empty()) {
        return named;
    }
    Node *owner = p_entity->get_owner();
    return owner != nullptr ? String(owner->get_name()) : String("<unnamed>");
}

} // namespace

const sim::Decl *NetwMultiplayer::sim_find_declaration(
    const NetwEntity *p_wrapper
) {
    const NetwEntityRecord *record = record_of_wrapper(p_wrapper);
    return record != nullptr ? record->find_sim_declaration() : nullptr;
}

void NetwMultiplayer::sim_restate(NetwEntity *p_wrapper, bool p_declares) {
    NetwEntityRecord *record = record_of_wrapper(p_wrapper);
    if (record == nullptr) {
        return;
    }
    if (p_declares) {
        record->sim_declaration().declared = true;
    }
    NetwMultiplayer *home = entity_home(p_wrapper);
    if (home != nullptr) {
        home->sim_declare(Ref<NetwEntity>(p_wrapper));
    }
    predict_restate(p_wrapper);
}

Error NetwMultiplayer::sim_write_param(
    NetwEntity *p_wrapper,
    SimulationParam p_param,
    const Variant &p_value
) {
    NetwEntityRecord *record = record_of_wrapper(p_wrapper);
    if (record == nullptr) {
        return ERR_DOES_NOT_EXIST;
    }
    sim::Declaration &declared = record->sim_declaration().declaration;
    Error verdict = OK;
    switch (p_param) {
        case SIMULATION_PARAM_BODIES: {
            const Array bodies = p_value;
            declared.bodies.clear();
            for (int64_t at = 0; at < bodies.size(); ++at) {
                declared.bodies.push_back(NodePath(bodies[at]));
            }
            NetwMultiplayer *home = entity_home(p_wrapper);
            const sim::Row *row = home != nullptr
                ? home->sim_rows.row_of(p_wrapper->get_rid_handle())
                : nullptr;
            if (row != nullptr && row->bodies.recorded) {
                fault::raise(
                    home,
                    NETW_FAULT_TOO_LATE,
                    {String("NetwSimulation.bodies"),
                     String("'") + entity_name(p_wrapper) + String("'"),
                     String("the entity enters the tree")},
                    uint64_t(gd::instance_id(p_wrapper->get_owner()))
                );
                verdict = ERR_LOCKED;
            }
            break;
        }
        case SIMULATION_PARAM_SCHEDULE:
            declared.schedule = sim::Schedule(int(p_value));
            break;
        case SIMULATION_PARAM_REPLICAS:
            declared.replicas = sim::Replicas(int(p_value));
            break;
        case SIMULATION_PARAM_RESTORE:
            declared.restore = sim::Restore(int(p_value));
            break;
        case SIMULATION_PARAM_MAX_RESTORE_TICKS:
            declared.max_restore_ticks = int(p_value);
            break;
        case SIMULATION_PARAM_CLAIM_ON_CONTACT:
            declared.claim_on_contact = bool(p_value);
            break;
        case SIMULATION_PARAM_RELEASE_ON_REST:
            declared.release_on_rest = MAX(double(p_value), 0.0);
            break;
        default:
            return ERR_INVALID_PARAMETER;
    }
    sim_restate(p_wrapper, true);
    return verdict;
}

Variant NetwMultiplayer::sim_read_param(
    const NetwEntity *p_wrapper,
    SimulationParam p_param
) {
    const sim::Decl *found = sim_find_declaration(p_wrapper);
    const sim::Declaration undeclared;
    const sim::Declaration &declared
        = found != nullptr ? found->declaration : undeclared;
    switch (p_param) {
        case SIMULATION_PARAM_BODIES: {
            TypedArray<NodePath> out;
            for (const NodePath &path : declared.bodies) {
                out.push_back(path);
            }
            return out;
        }
        case SIMULATION_PARAM_SCHEDULE:
            return int(declared.schedule);
        case SIMULATION_PARAM_REPLICAS:
            return int(declared.replicas);
        case SIMULATION_PARAM_RESTORE:
            return int(declared.restore);
        case SIMULATION_PARAM_MAX_RESTORE_TICKS:
            return declared.max_restore_ticks;
        case SIMULATION_PARAM_CLAIM_ON_CONTACT:
            return declared.claim_on_contact;
        case SIMULATION_PARAM_RELEASE_ON_REST:
            return declared.release_on_rest;
        default:
            return Variant();
    }
}

void NetwMultiplayer::sim_write_step(
    NetwEntity *p_wrapper,
    const Callable &p_step
) {
    game_call_declared(p_step, SIMULATION_STEP);
    NetwEntityRecord *record = record_of_wrapper(p_wrapper);
    if (record == nullptr) {
        return;
    }
    record->sim_declaration().declaration.step = p_step;
    sim_restate(p_wrapper, true);
    sim_adopt_step(p_wrapper, p_step);
}

void NetwMultiplayer::sim_adopt_step(
    NetwEntity *p_wrapper,
    const Callable &p_step
) {
    NetwEntityRecord *record = record_of_wrapper(p_wrapper);
    if (record == nullptr) {
        return;
    }
    record->sim_declaration().declaration.step = p_step;
    predict_step_changed(p_wrapper);
}

Callable NetwMultiplayer::sim_step_of(const NetwEntity *p_wrapper) {
    const sim::Decl *found = sim_find_declaration(p_wrapper);
    if (found != nullptr && found->declaration.step.is_valid()) {
        return found->declaration.step;
    }
    Node *owner = p_wrapper != nullptr ? p_wrapper->get_owner() : nullptr;
    if (owner != nullptr && owner->has_method(network_tick_name())) {
        return Callable(owner, network_tick_name());
    }
    return Callable();
}

void NetwMultiplayer::sim_preset(
    NetwEntity *p_wrapper,
    sim::Schedule p_schedule,
    bool p_restores,
    sim::Restore p_restore
) {
    NetwEntityRecord *record = record_of_wrapper(p_wrapper);
    if (record == nullptr) {
        return;
    }
    sim::Declaration &declared = record->sim_declaration().declaration;
    declared.schedule = p_schedule;
    if (p_restores) {
        declared.restore = p_restore;
    }
    sim_restate(p_wrapper, false);
}

void NetwMultiplayer::sim_write_choice(
    NetwEntity *p_wrapper,
    const RID &p_other,
    SimPick p_pick
) {
    NetwEntityRecord *record = record_of_wrapper(p_wrapper);
    if (record == nullptr) {
        return;
    }
    sim::Choice &choice = record->sim_declaration().choice;
    if (p_pick == SIM_PICK_CHOSEN) {
        sim::choose(choice, p_other, sim::Pick::CHOSEN);
    } else {
        sim::forget(choice, p_other);
    }
    sim_restate(p_wrapper, true);
}

void NetwMultiplayer::sim_write_layer(
    NetwEntity *p_wrapper,
    const StringName &p_layer,
    SimPick p_pick
) {
    NetwEntityRecord *record = record_of_wrapper(p_wrapper);
    if (record == nullptr) {
        return;
    }
    sim::Choice &choice = record->sim_declaration().choice;
    if (p_pick == SIM_PICK_CHOSEN) {
        sim::simulate_all(choice, p_layer);
    } else {
        sim::simulate_none(choice);
    }
    sim_restate(p_wrapper, true);
}

NetwSimulation::Restore NetwMultiplayer::sim_snap_restore_of(
    const NetwEntity *p_wrapper
) {
    const sim::Decl *found = sim_find_declaration(p_wrapper);
    return found != nullptr
            && found->declaration.restore == sim::Restore::EXTRAPOLATED
        ? NetwSimulation::RESTORE_EXTRAPOLATED
        : NetwSimulation::RESTORE_EXACT;
}

int NetwMultiplayer::sim_max_restore_ticks_of(const NetwEntity *p_wrapper) {
    const sim::Decl *found = sim_find_declaration(p_wrapper);
    return found != nullptr ? found->declaration.max_restore_ticks
                            : sim::Declaration().max_restore_ticks;
}

Ref<NetwSimulation> NetwMultiplayer::simulation_handle(
    const RID &p_entity
) const {
    const Ref<NetwEntity> wrapper = entity_get_view(p_entity);
    return wrapper.is_valid() ? Ref<NetwSimulation>(wrapper->get_simulation())
                              : Ref<NetwSimulation>();
}

Error NetwMultiplayer::simulation_set_param(
    const RID &p_entity,
    SimulationParam p_param,
    const Variant &p_value
) {
    const Ref<NetwEntity> wrapper = entity_get_view(p_entity);
    if (wrapper.is_null()) {
        return ERR_DOES_NOT_EXIST;
    }
    if (p_param < SIMULATION_PARAM_BODIES
        || p_param > SIMULATION_PARAM_RELEASE_ON_REST) {
        fault::raise(
            this,
            NETW_FAULT_BAD_VALUE,
            {"A simulation setting",
             vformat("setting %d", int(p_param)),
             "a simulation setting that exists"}
        );
        return ERR_INVALID_PARAMETER;
    }
    return sim_write_param(wrapper.ptr(), p_param, p_value);
}

Variant NetwMultiplayer::simulation_get_param(
    const RID &p_entity,
    SimulationParam p_param
) {
    const Ref<NetwEntity> wrapper = entity_get_view(p_entity);
    return wrapper.is_valid() ? sim_read_param(wrapper.ptr(), p_param)
                              : Variant();
}

void NetwMultiplayer::simulation_set_step_callback(
    const RID &p_entity,
    const Callable &p_callback
) {
    const Ref<NetwEntity> wrapper = entity_get_view(p_entity);
    if (wrapper.is_valid()) {
        sim_write_step(wrapper.ptr(), p_callback);
    }
}

NetwSimulation::Mode NetwMultiplayer::simulation_get_mode(const RID &p_entity) {
    const sim::Row *row = sim_rows.row_of(p_entity);
    return row != nullptr ? NetwSimulation::Mode(int(row->mode))
                          : NetwSimulation::MODE_NONE;
}

} // namespace netw
