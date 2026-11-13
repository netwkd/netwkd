#pragma once

#include <cstdint>

#include "godot/callable.hpp"
#include "godot/object.hpp"
#include "godot/ref_counted.hpp"
#include "godot/rid.hpp"
#include "godot/string_name.hpp"
#include "godot/variant.hpp"
#include "netw/sim/row.hpp"
#include "netw/sim/select.hpp"

namespace netw {

class NetwEntity;
class NetwMultiplayer;

class NetwSimulation : public godot::RefCounted {
    GDCLASS(NetwSimulation, godot::RefCounted)

public:
    enum Mode {
        MODE_NONE = int(sim::Mode::NONE),
        MODE_AUTHORITY = int(sim::Mode::AUTHORITY),
        MODE_PREDICT = int(sim::Mode::PREDICT),
        MODE_ACTIVE = int(sim::Mode::ACTIVE),
        MODE_PROXY = int(sim::Mode::PROXY),
    };

    enum Schedule {
        SCHEDULE_TICK = int(sim::Schedule::TICK),
        SCHEDULE_FRAME = int(sim::Schedule::FRAME),
        SCHEDULE_STEPPED = int(sim::Schedule::STEPPED),
        SCHEDULE_AUTO = int(sim::Schedule::AUTO),
    };

    enum Replicas {
        REPLICAS_PROXY = int(sim::Replicas::PROXY),
        REPLICAS_ACTIVE = int(sim::Replicas::ACTIVE),
    };

    enum Restore {
        RESTORE_EXACT = int(sim::Restore::EXACT),
        RESTORE_EXTRAPOLATED = int(sim::Restore::EXTRAPOLATED),
        RESTORE_BUFFERED = int(sim::Restore::BUFFERED),
    };

private:
    godot::ObjectID entity_id;

    godot::RID entity_rid() const;
    void write(int p_param, const godot::Variant &p_value);
    godot::Variant read(int p_param) const;

protected:
    static void _bind_methods();

public:
    void bind(NetwEntity *p_entity);
    godot::Ref<NetwEntity> entity() const;

    godot::TypedArray<godot::NodePath> get_bodies() const;
    void set_bodies(const godot::TypedArray<godot::NodePath> &p_value);
    Schedule get_schedule() const;
    void set_schedule(Schedule p_value);
    Replicas get_replicas() const;
    void set_replicas(Replicas p_value);
    Restore get_restore() const;
    void set_restore(Restore p_value);
    int get_max_restore_ticks() const;
    void set_max_restore_ticks(int p_value);
    bool get_claim_on_contact() const;
    void set_claim_on_contact(bool p_value);
    double get_release_on_rest() const;
    void set_release_on_rest(double p_value);
    godot::Callable get_step() const;
    void set_step(const godot::Callable &p_value);
    Mode get_mode() const;
    godot::TypedArray<NetwEntity> get_selected() const;

    void simulate(const godot::Ref<NetwEntity> &p_entity);
    void forget(const godot::Ref<NetwEntity> &p_entity);
    void simulate_all(const godot::StringName &p_layer = godot::StringName());
    void simulate_none();
};

godot::Ref<NetwSimulation> build_simulation_handle(godot::Object *p_entity);

} // namespace netw

VARIANT_ENUM_CAST(netw::NetwSimulation::Mode);
VARIANT_ENUM_CAST(netw::NetwSimulation::Schedule);
VARIANT_ENUM_CAST(netw::NetwSimulation::Replicas);
VARIANT_ENUM_CAST(netw::NetwSimulation::Restore);
