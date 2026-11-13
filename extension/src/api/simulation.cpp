#include "netw/api/simulation.hpp"

#include "godot/class_db.hpp"
#include "godot/node.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/fault_book.hpp"

using namespace godot;

namespace netw {

void NetwSimulation::bind(NetwEntity *p_entity) {
    entity_id = gd::instance_id(p_entity);
}

Ref<NetwEntity> NetwSimulation::entity() const {
    return Ref<NetwEntity>(
        Object::cast_to<NetwEntity>(gd::object_of(entity_id))
    );
}

RID NetwSimulation::entity_rid() const {
    const Ref<NetwEntity> bound = entity();
    return bound.is_valid() ? bound->get_rid_handle() : RID();
}

void NetwSimulation::write(int p_param, const Variant &p_value) {
    const Ref<NetwEntity> bound = entity();
    NetwMultiplayer::sim_write_param(
        bound.ptr(),
        NetwMultiplayer::SimulationParam(p_param),
        p_value
    );
}

Variant NetwSimulation::read(int p_param) const {
    const Ref<NetwEntity> bound = entity();
    return NetwMultiplayer::sim_read_param(
        bound.ptr(),
        NetwMultiplayer::SimulationParam(p_param)
    );
}


TypedArray<NodePath> NetwSimulation::get_bodies() const {
    return read(NetwMultiplayer::SIMULATION_PARAM_BODIES);
}

void NetwSimulation::set_bodies(const TypedArray<NodePath> &p_value) {
    write(NetwMultiplayer::SIMULATION_PARAM_BODIES, p_value);
}

NetwSimulation::Schedule NetwSimulation::get_schedule() const {
    return Schedule(int(read(NetwMultiplayer::SIMULATION_PARAM_SCHEDULE)));
}

void NetwSimulation::set_schedule(Schedule p_value) {
    write(NetwMultiplayer::SIMULATION_PARAM_SCHEDULE, p_value);
}

NetwSimulation::Replicas NetwSimulation::get_replicas() const {
    return Replicas(int(read(NetwMultiplayer::SIMULATION_PARAM_REPLICAS)));
}

void NetwSimulation::set_replicas(Replicas p_value) {
    write(NetwMultiplayer::SIMULATION_PARAM_REPLICAS, p_value);
}

NetwSimulation::Restore NetwSimulation::get_restore() const {
    return Restore(int(read(NetwMultiplayer::SIMULATION_PARAM_RESTORE)));
}

void NetwSimulation::set_restore(Restore p_value) {
    write(NetwMultiplayer::SIMULATION_PARAM_RESTORE, p_value);
}

int NetwSimulation::get_max_restore_ticks() const {
    return read(NetwMultiplayer::SIMULATION_PARAM_MAX_RESTORE_TICKS);
}

void NetwSimulation::set_max_restore_ticks(int p_value) {
    write(NetwMultiplayer::SIMULATION_PARAM_MAX_RESTORE_TICKS, p_value);
}

Callable NetwSimulation::get_step() const {
    const Ref<NetwEntity> bound = entity();
    return NetwMultiplayer::sim_step_of(bound.ptr());
}

void NetwSimulation::set_step(const Callable &p_value) {
    const Ref<NetwEntity> bound = entity();
    NetwMultiplayer::sim_write_step(bound.ptr(), p_value);
}

bool NetwSimulation::get_claim_on_contact() const {
    return read(NetwMultiplayer::SIMULATION_PARAM_CLAIM_ON_CONTACT);
}

void NetwSimulation::set_claim_on_contact(bool p_value) {
    write(NetwMultiplayer::SIMULATION_PARAM_CLAIM_ON_CONTACT, p_value);
}

double NetwSimulation::get_release_on_rest() const {
    return read(NetwMultiplayer::SIMULATION_PARAM_RELEASE_ON_REST);
}

void NetwSimulation::set_release_on_rest(double p_value) {
    write(NetwMultiplayer::SIMULATION_PARAM_RELEASE_ON_REST, p_value);
}

NetwSimulation::Mode NetwSimulation::get_mode() const {
    const Ref<NetwEntity> bound = entity();
    NetwMultiplayer *home = NetwMultiplayer::entity_home(bound.ptr());
    return home != nullptr ? home->simulation_get_mode(entity_rid())
                           : MODE_NONE;
}

TypedArray<NetwEntity> NetwSimulation::get_selected() const {
    TypedArray<NetwEntity> out;
    const Ref<NetwEntity> bound = entity();
    NetwMultiplayer *home = NetwMultiplayer::entity_home(bound.ptr());
    if (home == nullptr) {
        return out;
    }
    const TypedArray<RID> selected
        = home->simulation_get_selected(entity_rid());
    for (int64_t at = 0; at < selected.size(); ++at) {
        out.push_back(home->entity_get_view(selected[at]));
    }
    return out;
}

void NetwSimulation::simulate(const Ref<NetwEntity> &p_entity) {
    const Ref<NetwEntity> bound = entity();
    NetwMultiplayer *home = NetwMultiplayer::entity_home(bound.ptr());
    if (p_entity.is_null()) {
        fault::raise(
            home,
            NETW_FAULT_EMPTY_ARGUMENT,
            {String("{NetwSimulation.simulate}"), String(), String("entity")},
            bound.is_valid() ? uint64_t(gd::instance_id(bound->get_owner())) : 0
        );
        return;
    }
    const RID rid = entity_rid();
    if (home != nullptr && rid.is_valid()
        && home->entity_get_view(rid).is_valid()
        && p_entity->get_owner() != nullptr) {
        home->sim_choose(rid, p_entity, "{NetwSimulation.simulate}");
        return;
    }
    NetwMultiplayer::sim_write_choice(
        bound.ptr(),
        p_entity->get_rid_handle(),
        NetwMultiplayer::SIM_PICK_CHOSEN
    );
}

void NetwSimulation::forget(const Ref<NetwEntity> &p_entity) {
    if (p_entity.is_null()) {
        return;
    }
    const Ref<NetwEntity> bound = entity();
    NetwMultiplayer::sim_write_choice(
        bound.ptr(),
        p_entity->get_rid_handle(),
        NetwMultiplayer::SIM_PICK_FORGET
    );
}

void NetwSimulation::simulate_all(const StringName &p_layer) {
    const Ref<NetwEntity> bound = entity();
    NetwMultiplayer::sim_write_layer(
        bound.ptr(),
        p_layer,
        NetwMultiplayer::SIM_PICK_CHOSEN
    );
}

void NetwSimulation::simulate_none() {
    const Ref<NetwEntity> bound = entity();
    NetwMultiplayer::sim_write_layer(
        bound.ptr(),
        StringName(),
        NetwMultiplayer::SIM_PICK_FORGET
    );
}

void NetwSimulation::_bind_methods() {
    ClassDB::bind_method(D_METHOD("get_bodies"), &NetwSimulation::get_bodies);
    ClassDB::bind_method(
        D_METHOD("set_bodies", "value"),
        &NetwSimulation::set_bodies
    );
    ADD_PROPERTY(
        PropertyInfo(
            Variant::ARRAY,
            "bodies",
            PROPERTY_HINT_ARRAY_TYPE,
            "NodePath"
        ),
        "set_bodies",
        "get_bodies"
    );
    ClassDB::bind_method(
        D_METHOD("get_schedule"),
        &NetwSimulation::get_schedule
    );
    ClassDB::bind_method(
        D_METHOD("set_schedule", "value"),
        &NetwSimulation::set_schedule
    );
    ADD_PROPERTY(
        PropertyInfo(
            Variant::INT,
            "schedule",
            PROPERTY_HINT_ENUM,
            "Tick,Frame,Stepped,Auto",
            PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_CLASS_IS_ENUM,
            "NetwSimulation.Schedule"
        ),
        "set_schedule",
        "get_schedule"
    );
    ClassDB::bind_method(
        D_METHOD("get_replicas"),
        &NetwSimulation::get_replicas
    );
    ClassDB::bind_method(
        D_METHOD("set_replicas", "value"),
        &NetwSimulation::set_replicas
    );
    ADD_PROPERTY(
        PropertyInfo(
            Variant::INT,
            "replicas",
            PROPERTY_HINT_ENUM,
            "Proxy,Active",
            PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_CLASS_IS_ENUM,
            "NetwSimulation.Replicas"
        ),
        "set_replicas",
        "get_replicas"
    );
    ClassDB::bind_method(D_METHOD("get_restore"), &NetwSimulation::get_restore);
    ClassDB::bind_method(
        D_METHOD("set_restore", "value"),
        &NetwSimulation::set_restore
    );
    ADD_PROPERTY(
        PropertyInfo(
            Variant::INT,
            "restore",
            PROPERTY_HINT_ENUM,
            "Exact,Extrapolated,Buffered",
            PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_CLASS_IS_ENUM,
            "NetwSimulation.Restore"
        ),
        "set_restore",
        "get_restore"
    );
    ClassDB::bind_method(
        D_METHOD("get_max_restore_ticks"),
        &NetwSimulation::get_max_restore_ticks
    );
    ClassDB::bind_method(
        D_METHOD("set_max_restore_ticks", "value"),
        &NetwSimulation::set_max_restore_ticks
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::INT, "max_restore_ticks"),
        "set_max_restore_ticks",
        "get_max_restore_ticks"
    );
    ClassDB::bind_method(
        D_METHOD("get_claim_on_contact"),
        &NetwSimulation::get_claim_on_contact
    );
    ClassDB::bind_method(
        D_METHOD("set_claim_on_contact", "value"),
        &NetwSimulation::set_claim_on_contact
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::BOOL, "claim_on_contact"),
        "set_claim_on_contact",
        "get_claim_on_contact"
    );
    ClassDB::bind_method(
        D_METHOD("get_release_on_rest"),
        &NetwSimulation::get_release_on_rest
    );
    ClassDB::bind_method(
        D_METHOD("set_release_on_rest", "value"),
        &NetwSimulation::set_release_on_rest
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::FLOAT, "release_on_rest"),
        "set_release_on_rest",
        "get_release_on_rest"
    );
    ClassDB::bind_method(D_METHOD("get_step"), &NetwSimulation::get_step);
    ClassDB::bind_method(
        D_METHOD("set_step", "value"),
        &NetwSimulation::set_step
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::CALLABLE, "step"),
        "set_step",
        "get_step"
    );
    ClassDB::bind_method(D_METHOD("get_mode"), &NetwSimulation::get_mode);
    ADD_PROPERTY(
        PropertyInfo(
            Variant::INT,
            "mode",
            PROPERTY_HINT_ENUM,
            "None,Authority,Predict,Active,Proxy",
            PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_CLASS_IS_ENUM,
            "NetwSimulation.Mode"
        ),
        String(),
        "get_mode"
    );
    ClassDB::bind_method(
        D_METHOD("get_selected"),
        &NetwSimulation::get_selected
    );
    ADD_PROPERTY(
        PropertyInfo(
            Variant::ARRAY,
            "selected",
            PROPERTY_HINT_ARRAY_TYPE,
            "NetwEntity"
        ),
        String(),
        "get_selected"
    );

    ClassDB::bind_method(
        D_METHOD("simulate", "entity"),
        &NetwSimulation::simulate
    );
    ClassDB::bind_method(D_METHOD("forget", "entity"), &NetwSimulation::forget);
    ClassDB::bind_method(
        D_METHOD("simulate_all", "layer"),
        &NetwSimulation::simulate_all,
        DEFVAL(StringName())
    );
    ClassDB::bind_method(
        D_METHOD("simulate_none"),
        &NetwSimulation::simulate_none
    );

    ADD_SIGNAL(MethodInfo(
        "mode_changed",
        PropertyInfo(
            Variant::INT,
            "previous",
            PROPERTY_HINT_NONE,
            "",
            PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_CLASS_IS_ENUM,
            "NetwSimulation.Mode"
        ),
        PropertyInfo(
            Variant::INT,
            "mode",
            PROPERTY_HINT_NONE,
            "",
            PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_CLASS_IS_ENUM,
            "NetwSimulation.Mode"
        )
    ));

    BIND_ENUM_CONSTANT(MODE_NONE);
    BIND_ENUM_CONSTANT(MODE_AUTHORITY);
    BIND_ENUM_CONSTANT(MODE_PREDICT);
    BIND_ENUM_CONSTANT(MODE_ACTIVE);
    BIND_ENUM_CONSTANT(MODE_PROXY);
    BIND_ENUM_CONSTANT(SCHEDULE_TICK);
    BIND_ENUM_CONSTANT(SCHEDULE_FRAME);
    BIND_ENUM_CONSTANT(SCHEDULE_STEPPED);
    BIND_ENUM_CONSTANT(SCHEDULE_AUTO);
    BIND_ENUM_CONSTANT(REPLICAS_PROXY);
    BIND_ENUM_CONSTANT(REPLICAS_ACTIVE);
    BIND_ENUM_CONSTANT(RESTORE_EXACT);
    BIND_ENUM_CONSTANT(RESTORE_EXTRAPOLATED);
    BIND_ENUM_CONSTANT(RESTORE_BUFFERED);
}

Ref<NetwSimulation> build_simulation_handle(Object *p_entity) {
    Ref<NetwSimulation> made;
    made.instantiate();
    made->bind(Object::cast_to<NetwEntity>(p_entity));
    return made;
}

} // namespace netw
