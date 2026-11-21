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

const StringName &network_process_name() {
    static const StringName name("_network_process");
    return name;
}

String entity_name(const NetwEntityRecord &p_record, const Node *p_owner) {
    const String named = String(p_record.get_entity_id());
    if (!named.is_empty()) {
        return named;
    }
    return p_owner != nullptr ? String(p_owner->get_name())
                              : String("<unnamed>");
}

} // namespace

void NetwMultiplayer::sim_restate(
    NetwMultiplayer *p_home,
    NetwEntityRecord &p_record,
    bool p_declares
) {
    if (p_declares) {
        p_record.sim_declaration().declared = true;
    }
    if (p_home != nullptr) {
        p_home->sim_declare(p_record.get_handle());
    }
    predict_restate(p_home, p_record);
}

Error NetwMultiplayer::sim_write_param(
    NetwMultiplayer *p_home,
    NetwEntityRecord &p_record,
    Node *p_owner,
    SimulationParam p_param,
    const Variant &p_value
) {
    sim::Declaration &declared = p_record.sim_declaration().declaration;
    Error verdict = OK;
    switch (p_param) {
        case SIMULATION_PARAM_BODIES: {
            const sim::Row *row = p_home != nullptr
                ? p_home->sim_rows.row_of(p_record.get_handle())
                : nullptr;
            if (row != nullptr && row->bodies.recorded) {
                fault::raise(
                    p_home,
                    NETW_FAULT_TOO_LATE,
                    {String("NetwSimulation.bodies"),
                     String("'") + entity_name(p_record, p_owner) + String("'"),
                     String("the entity enters the tree")},
                    uint64_t(gd::instance_id(p_owner))
                );
                verdict = ERR_LOCKED;
                break;
            }
            const Array bodies = p_value;
            declared.bodies.clear();
            for (int64_t at = 0; at < bodies.size(); ++at) {
                declared.bodies.push_back(NodePath(bodies[at]));
            }
            break;
        }
        case SIMULATION_PARAM_SCHEDULE:
            declared.schedule = sim::Schedule(int(p_value));
            break;
        case SIMULATION_PARAM_REMOTE_MODE:
            declared.remote_mode = sim::RemoteMode(int(p_value));
            break;
        case SIMULATION_PARAM_RESTORE:
            declared.restore = sim::Restore(int(p_value));
            declared.wrote_restore = true;
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
        case SIMULATION_PARAM_PHYSICS_DRIVEN:
            declared.physics_driven = bool(p_value);
            declared.wrote_physics_driven = true;
            break;
        default:
            return ERR_INVALID_PARAMETER;
    }
    if (predict_engine_of(p_home, p_record) != nullptr) {
        predict_settle_kind(p_record, p_owner);
    }
    sim_restate(p_home, p_record, true);
    return verdict;
}

Variant NetwMultiplayer::sim_read_param(
    const NetwEntityRecord &p_record,
    Node *p_owner,
    SimulationParam p_param
) {
    const sim::Decl *found = p_record.find_sim_declaration();
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
        case SIMULATION_PARAM_REMOTE_MODE:
            return int(declared.remote_mode);
        case SIMULATION_PARAM_RESTORE:
            return int(declared.restore);
        case SIMULATION_PARAM_MAX_RESTORE_TICKS:
            return declared.max_restore_ticks;
        case SIMULATION_PARAM_CLAIM_ON_CONTACT:
            return declared.claim_on_contact;
        case SIMULATION_PARAM_RELEASE_ON_REST:
            return declared.release_on_rest;
        case SIMULATION_PARAM_PHYSICS_DRIVEN:
            return sim_physics_driven(p_record, p_owner);
        default:
            return Variant();
    }
}

void NetwMultiplayer::sim_write_step(
    NetwMultiplayer *p_home,
    NetwEntityRecord &p_record,
    const Callable &p_step
) {
    p_record.sim_declaration().declaration.step = p_step;
    sim_restate(p_home, p_record, true);
    sim_adopt_step(p_home, p_record, p_step);
}

void NetwMultiplayer::sim_adopt_step(
    NetwMultiplayer *p_home,
    NetwEntityRecord &p_record,
    const Callable &p_step
) {
    p_record.sim_declaration().declaration.step = p_step;
    NetwPredictSlotEngine *engine = predict_engine_of(p_home, p_record);
    if (engine != nullptr) {
        engine->push_simulate();
    }
}

Callable NetwMultiplayer::sim_step_of(
    const NetwEntityRecord &p_record,
    Node *p_owner
) {
    const sim::Decl *found = p_record.find_sim_declaration();
    if (found != nullptr && found->declaration.step.is_valid()) {
        return found->declaration.step;
    }
    if (p_owner != nullptr && p_owner->has_method(network_process_name())) {
        return Callable(p_owner, network_process_name());
    }
    return Callable();
}

bool NetwMultiplayer::sim_physics_driven(
    const NetwEntityRecord &p_record,
    Node *p_owner
) {
    const sim::Decl *found = p_record.find_sim_declaration();
    if (found != nullptr && found->declaration.wrote_physics_driven) {
        return found->declaration.physics_driven;
    }
    if (p_owner == nullptr) {
        return false;
    }
    if (sim::is_solver_body(p_owner)) {
        return true;
    }
    if (found == nullptr) {
        return false;
    }
    for (const NodePath &path : found->declaration.bodies) {
        if (sim::is_solver_body(p_owner->get_node_or_null(path))) {
            return true;
        }
    }
    return false;
}

bool NetwMultiplayer::sim_simulated(
    const NetwEntityRecord &p_record,
    Node *p_owner
) {
    return sim_step_of(p_record, p_owner).is_valid()
        || sim_physics_driven(p_record, p_owner);
}

NetwSimulation::Restore NetwMultiplayer::sim_snap_restore_of(
    const NetwEntityRecord &p_record
) {
    const sim::Decl *found = p_record.find_sim_declaration();
    return found != nullptr
            && found->declaration.restore == sim::Restore::EXTRAPOLATED
        ? NetwSimulation::RESTORE_EXTRAPOLATED
        : NetwSimulation::RESTORE_EXACT;
}

int NetwMultiplayer::sim_max_restore_ticks_of(
    const NetwEntityRecord &p_record
) {
    const sim::Decl *found = p_record.find_sim_declaration();
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
    return sim_write_param(
        wrapper->home_session(),
        *wrapper->get_record(),
        wrapper->get_owner(),
        p_param,
        p_value
    );
}

Variant NetwMultiplayer::simulation_get_param(
    const RID &p_entity,
    SimulationParam p_param
) {
    const Ref<NetwEntity> wrapper = entity_get_view(p_entity);
    if (wrapper.is_null()) {
        return Variant();
    }
    return sim_read_param(
        *wrapper->get_record(),
        wrapper->get_owner(),
        p_param
    );
}

void NetwMultiplayer::simulation_set_network_process_callback(
    const RID &p_entity,
    const Callable &p_callback
) {
    const Ref<NetwEntity> wrapper = entity_get_view(p_entity);
    if (wrapper.is_valid()) {
        game_call_declared(p_callback, game_calls::SIMULATION_STEP);
        sim_write_step(
            wrapper->home_session(),
            *wrapper->get_record(),
            p_callback
        );
    }
}

NetwSimulation::Mode NetwMultiplayer::simulation_get_mode(const RID &p_entity) {
    const sim::Row *row = sim_rows.row_of(p_entity);
    return row != nullptr ? NetwSimulation::Mode(int(row->mode))
                          : NetwSimulation::MODE_NONE;
}

} // namespace netw
