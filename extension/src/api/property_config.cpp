#include "netw/api/property_config.hpp"

#include "godot/callable.hpp"
#include "godot/class_db.hpp"
#include "godot/node.hpp"
#include "netw/fault_book.hpp"
#include "netw/log.hpp"

using namespace godot;

namespace netw {

using repl::PropertySet;

void NetwPropertyConfig::warn_double_set(
    bool p_replaces,
    const char *p_axis
) const {
    if (!p_replaces) {
        return;
    }
    fault::raise(
        nullptr,
        NETW_FAULT_DECLARED_AGAIN,
        {vformat("The %s", String(p_axis)), String(get_context_name())}
    );
}

Ref<NetwPropertyConfig> NetwPropertyConfig::state() {
    in_state_set = true;
    lane = PropertySet::VOLATILE;
    return Ref<NetwPropertyConfig>(this);
}

Ref<NetwPropertyConfig> NetwPropertyConfig::input() {
    in_input_set = true;
    lane = PropertySet::VOLATILE;
    set_audience = PropertySet::AUDIENCE_SERVER_ONLY;
    return Ref<NetwPropertyConfig>(this);
}

Ref<NetwPropertyConfig> NetwPropertyConfig::broadcast() {
    in_broadcast_set = true;
    lane = PropertySet::VOLATILE;
    return Ref<NetwPropertyConfig>(this);
}

Ref<NetwPropertyConfig> NetwPropertyConfig::volatile_lane() {
    lane = PropertySet::VOLATILE;
    return Ref<NetwPropertyConfig>(this);
}

Ref<NetwPropertyConfig> NetwPropertyConfig::retained() {
    lane = PropertySet::RETAINED;
    return Ref<NetwPropertyConfig>(this);
}

Ref<NetwPropertyConfig> NetwPropertyConfig::unchecked() {
    is_unchecked = true;
    return Ref<NetwPropertyConfig>(this);
}

Ref<NetwPropertyConfig> NetwPropertyConfig::correction_weight(double p_weight) {
    weight = p_weight;
    return Ref<NetwPropertyConfig>(this);
}

Ref<NetwPropertyConfig> NetwPropertyConfig::extrapolate(
    const StringName &p_rate_property
) {
    rate_property = p_rate_property;
    return Ref<NetwPropertyConfig>(this);
}

Ref<NetwPropertyConfig> NetwPropertyConfig::teleport_only() {
    explicit_teleport_only = true;
    return Ref<NetwPropertyConfig>(this);
}

Ref<NetwPropertyConfig> NetwPropertyConfig::correction_threshold(
    double p_threshold
) {
    correction_threshold_override = p_threshold;
    return Ref<NetwPropertyConfig>(this);
}

Ref<NetwPropertyConfig> NetwPropertyConfig::teleport_threshold(
    double p_distance
) {
    teleport_threshold_override = p_distance;
    return Ref<NetwPropertyConfig>(this);
}

Ref<NetwPropertyConfig> NetwPropertyConfig::heartbeat(int64_t p_ticks) {
    warn_double_set(
        set_heartbeat_ticks != UNSET && set_heartbeat_ticks != p_ticks,
        "heartbeat"
    );
    set_heartbeat_ticks = p_ticks;
    return Ref<NetwPropertyConfig>(this);
}

Ref<NetwPropertyConfig> NetwPropertyConfig::windowed(int64_t p_samples) {
    warn_double_set(set_window != UNSET && set_window != p_samples, "window");
    set_window = p_samples;
    return Ref<NetwPropertyConfig>(this);
}

Ref<NetwPropertyConfig> NetwPropertyConfig::audience(bool p_server_only) {
    set_audience = p_server_only ? PropertySet::AUDIENCE_SERVER_ONLY
                                 : PropertySet::AUDIENCE_PUBLIC;
    return Ref<NetwPropertyConfig>(this);
}

Ref<NetwPropertyConfig> NetwPropertyConfig::persisted(
    const StringName &p_saved_column_name
) {
    persist = true;
    saved_column = p_saved_column_name;
    return Ref<NetwPropertyConfig>(this);
}

Ref<NetwPropertyConfig> NetwPropertyConfig::on_spawn() {
    is_spawn_state = true;
    return Ref<NetwPropertyConfig>(this);
}

void NetwPropertyConfig::_bind_methods() {
    ClassDB::bind_method(D_METHOD("state"), &NetwPropertyConfig::state);
    ClassDB::bind_method(D_METHOD("input"), &NetwPropertyConfig::input);
    ClassDB::bind_method(D_METHOD("broadcast"), &NetwPropertyConfig::broadcast);
    ClassDB::bind_method(
        D_METHOD("volatile"),
        &NetwPropertyConfig::volatile_lane
    );
    ClassDB::bind_method(D_METHOD("retained"), &NetwPropertyConfig::retained);
    ClassDB::bind_method(D_METHOD("unchecked"), &NetwPropertyConfig::unchecked);
    ClassDB::bind_method(
        D_METHOD("correction_weight", "weight"),
        &NetwPropertyConfig::correction_weight
    );
    ClassDB::bind_method(
        D_METHOD("extrapolate", "rate_property"),
        &NetwPropertyConfig::extrapolate
    );
    ClassDB::bind_method(
        D_METHOD("teleport_only"),
        &NetwPropertyConfig::teleport_only
    );
    ClassDB::bind_method(
        D_METHOD("correction_threshold", "threshold"),
        &NetwPropertyConfig::correction_threshold
    );
    ClassDB::bind_method(
        D_METHOD("teleport_threshold", "distance"),
        &NetwPropertyConfig::teleport_threshold
    );
    ClassDB::bind_method(
        D_METHOD("heartbeat", "ticks"),
        &NetwPropertyConfig::heartbeat
    );
    ClassDB::bind_method(
        D_METHOD("windowed", "samples"),
        &NetwPropertyConfig::windowed
    );
    ClassDB::bind_method(
        D_METHOD("audience", "server_only"),
        &NetwPropertyConfig::audience,
        DEFVAL(true)
    );
    ClassDB::bind_method(
        D_METHOD("persisted", "saved_column_name"),
        &NetwPropertyConfig::persisted,
        DEFVAL(StringName())
    );
    ClassDB::bind_method(D_METHOD("on_spawn"), &NetwPropertyConfig::on_spawn);

    ClassDB::bind_method(
        D_METHOD("set_lane", "lane"),
        &NetwPropertyConfig::set_lane
    );
    ClassDB::bind_method(D_METHOD("get_lane"), &NetwPropertyConfig::get_lane);
    ADD_PROPERTY(
        PropertyInfo(
            Variant::INT,
            "lane",
            PROPERTY_HINT_ENUM,
            "Volatile,Retained"
        ),
        "set_lane",
        "get_lane"
    );

    ClassDB::bind_method(
        D_METHOD("set_set_audience", "set_audience"),
        &NetwPropertyConfig::set_set_audience
    );
    ClassDB::bind_method(
        D_METHOD("get_set_audience"),
        &NetwPropertyConfig::get_set_audience
    );
    ADD_PROPERTY(
        PropertyInfo(
            Variant::INT,
            "set_audience",
            PROPERTY_HINT_ENUM,
            "Public,Server Only"
        ),
        "set_set_audience",
        "get_set_audience"
    );

#define NETW_PROPERTY_CONFIG_PROPERTY(m_type, m_name) \
    ClassDB::bind_method( \
        D_METHOD("set_" #m_name, #m_name), \
        &NetwPropertyConfig::set_##m_name \
    ); \
    ClassDB::bind_method( \
        D_METHOD("get_" #m_name), \
        &NetwPropertyConfig::get_##m_name \
    ); \
    ADD_PROPERTY(PropertyInfo(m_type, #m_name), "set_" #m_name, "get_" #m_name)

    NETW_PROPERTY_CONFIG_PROPERTY(Variant::BOOL, is_property);
    NETW_PROPERTY_CONFIG_PROPERTY(Variant::BOOL, is_spawn_state);
    NETW_PROPERTY_CONFIG_PROPERTY(Variant::BOOL, in_state_set);
    NETW_PROPERTY_CONFIG_PROPERTY(Variant::BOOL, in_input_set);
    NETW_PROPERTY_CONFIG_PROPERTY(Variant::BOOL, in_broadcast_set);
    NETW_PROPERTY_CONFIG_PROPERTY(Variant::INT, set_heartbeat_ticks);
    NETW_PROPERTY_CONFIG_PROPERTY(Variant::INT, set_window);

#undef NETW_PROPERTY_CONFIG_PROPERTY

    BIND_ENUM_CONSTANT(VOLATILE);
    BIND_ENUM_CONSTANT(RETAINED);
    BIND_ENUM_CONSTANT(AUDIENCE_PUBLIC);
    BIND_ENUM_CONSTANT(AUDIENCE_SERVER_ONLY);
    BIND_CONSTANT(UNSET);
}

} // namespace netw
