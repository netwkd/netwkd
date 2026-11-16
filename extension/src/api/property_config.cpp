#include "netw/api/property_config.hpp"

#include "godot/callable.hpp"
#include "godot/class_db.hpp"
#include "godot/node.hpp"
#include "netw/fault_book.hpp"
#include "netw/log.hpp"

using namespace godot;

namespace netw {

using repl::PropertySet;

namespace {

enum Kind {
    KIND_STATE,
    KIND_INPUT,
    KIND_BROADCAST,
};

enum StateSetting {
    SETTING_UNCHECKED,
    SETTING_CORRECTION_WEIGHT,
    SETTING_EXTRAPOLATE,
    SETTING_TELEPORT_ONLY,
    SETTING_CORRECTION_THRESHOLD,
    SETTING_TELEPORT_THRESHOLD,
    STATE_SETTING_COUNT,
};

constexpr const char *KINDS[] = {"state", "input", "broadcast"};
constexpr const char *STATE_SETTINGS[] = {
    "unchecked",
    "correction_weight",
    "extrapolate",
    "teleport_only",
    "correction_threshold",
    "teleport_threshold",
};
constexpr const char *ON_STATE
    = "it on a property declared with {NetwPropertyConfig.state}";
constexpr const char *ON_STATE_OR_INPUT
    = "it on a property declared with {NetwPropertyConfig.state} or "
      "{NetwPropertyConfig.input}";

} // namespace

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

void NetwPropertyConfig::refuse_setting(
    const char *p_setting,
    const char *p_kind,
    const char *p_fits
) const {
    fault::raise(
        nullptr,
        NETW_FAULT_SETTING_REFUSED,
        {vformat("{NetwPropertyConfig.%s}", String(p_setting)),
         vformat(
             "{NetwPropertyConfig.%s} on '%s'",
             String(p_kind),
             String(get_context_name())
         ),
         String(p_fits)}
    );
}

bool NetwPropertyConfig::takes_kind(int p_kind) {
    if (is_refused()) {
        return false;
    }
    const int held = in_state_set ? KIND_STATE
        : in_input_set            ? KIND_INPUT
        : in_broadcast_set        ? KIND_BROADCAST
                                  : p_kind;
    if (held == p_kind) {
        return true;
    }
    fault::raise(
        nullptr,
        NETW_FAULT_SETTING_REFUSED,
        {vformat("'%s'", String(get_context_name())),
         vformat(
             "{NetwPropertyConfig.%s} and {NetwPropertyConfig.%s} together",
             String(KINDS[MIN(held, p_kind)]),
             String(KINDS[MAX(held, p_kind)])
         ),
         String("one of them, and a second property for the other")}
    );
    in_state_set = false;
    in_input_set = false;
    in_broadcast_set = false;
    refuse();
    return false;
}

void NetwPropertyConfig::drop_settings_unread_by(int p_kind) {
    for (int setting = 0; setting < STATE_SETTING_COUNT; ++setting) {
        if ((state_settings & (1u << setting)) != 0) {
            refuse_setting(STATE_SETTINGS[setting], KINDS[p_kind], ON_STATE);
        }
    }
    if (state_settings != 0) {
        state_settings = 0;
        is_unchecked = false;
        weight = 0.0;
        rate_property = StringName();
        explicit_teleport_only = false;
        correction_threshold_override = -1.0;
        teleport_threshold_override = -1.0;
    }
    if (p_kind != KIND_BROADCAST) {
        return;
    }
    if (set_window != UNSET) {
        refuse_setting("windowed", KINDS[p_kind], ON_STATE_OR_INPUT);
        set_window = UNSET;
    }
    if (set_audience == PropertySet::AUDIENCE_SERVER_ONLY) {
        refuse_setting("audience", KINDS[p_kind], ON_STATE);
        set_audience = PropertySet::AUDIENCE_PUBLIC;
    }
}

bool NetwPropertyConfig::takes_state_setting(int p_setting) {
    if (is_refused()) {
        return false;
    }
    if (in_input_set || in_broadcast_set) {
        refuse_setting(
            STATE_SETTINGS[p_setting],
            KINDS[in_input_set ? KIND_INPUT : KIND_BROADCAST],
            ON_STATE
        );
        return false;
    }
    state_settings |= 1u << p_setting;
    return true;
}

bool NetwPropertyConfig::takes_amount(
    const char *p_setting,
    double p_amount
) const {
    if (p_amount >= 0.0) {
        return true;
    }
    fault::raise(
        nullptr,
        NETW_FAULT_BAD_VALUE,
        {vformat("{NetwPropertyConfig.%s}", String(p_setting)),
         vformat(
             "%s for '%s'",
             String::num(p_amount),
             String(get_context_name())
         ),
         String("a number from zero")}
    );
    return false;
}

Ref<NetwPropertyConfig> NetwPropertyConfig::state() {
    if (takes_kind(KIND_STATE)) {
        in_state_set = true;
    }
    return Ref<NetwPropertyConfig>(this);
}

Ref<NetwPropertyConfig> NetwPropertyConfig::input() {
    if (takes_kind(KIND_INPUT)) {
        drop_settings_unread_by(KIND_INPUT);
        in_input_set = true;
        set_audience = PropertySet::AUDIENCE_SERVER_ONLY;
    }
    return Ref<NetwPropertyConfig>(this);
}

Ref<NetwPropertyConfig> NetwPropertyConfig::broadcast() {
    if (takes_kind(KIND_BROADCAST)) {
        drop_settings_unread_by(KIND_BROADCAST);
        in_broadcast_set = true;
    }
    return Ref<NetwPropertyConfig>(this);
}

Ref<NetwPropertyConfig> NetwPropertyConfig::volatile_lane() {
    if (is_refused()) {
        return Ref<NetwPropertyConfig>(this);
    }
    lane = PropertySet::VOLATILE;
    return Ref<NetwPropertyConfig>(this);
}

Ref<NetwPropertyConfig> NetwPropertyConfig::retained() {
    if (is_refused()) {
        return Ref<NetwPropertyConfig>(this);
    }
    lane = PropertySet::RETAINED;
    return Ref<NetwPropertyConfig>(this);
}

Ref<NetwPropertyConfig> NetwPropertyConfig::unchecked() {
    if (takes_state_setting(SETTING_UNCHECKED)) {
        is_unchecked = true;
    }
    return Ref<NetwPropertyConfig>(this);
}

Ref<NetwPropertyConfig> NetwPropertyConfig::correction_weight(double p_weight) {
    if (takes_amount("correction_weight", p_weight)
        && takes_state_setting(SETTING_CORRECTION_WEIGHT)) {
        weight = p_weight;
    }
    return Ref<NetwPropertyConfig>(this);
}

Ref<NetwPropertyConfig> NetwPropertyConfig::extrapolate(
    const StringName &p_rate_property
) {
    if (takes_state_setting(SETTING_EXTRAPOLATE)) {
        rate_property = p_rate_property;
    }
    return Ref<NetwPropertyConfig>(this);
}

Ref<NetwPropertyConfig> NetwPropertyConfig::teleport_only() {
    if (takes_state_setting(SETTING_TELEPORT_ONLY)) {
        explicit_teleport_only = true;
    }
    return Ref<NetwPropertyConfig>(this);
}

Ref<NetwPropertyConfig> NetwPropertyConfig::correction_threshold(
    double p_threshold
) {
    if (takes_amount("correction_threshold", p_threshold)
        && takes_state_setting(SETTING_CORRECTION_THRESHOLD)) {
        correction_threshold_override = p_threshold;
    }
    return Ref<NetwPropertyConfig>(this);
}

Ref<NetwPropertyConfig> NetwPropertyConfig::teleport_threshold(
    double p_distance
) {
    if (takes_amount("teleport_threshold", p_distance)
        && takes_state_setting(SETTING_TELEPORT_THRESHOLD)) {
        teleport_threshold_override = p_distance;
    }
    return Ref<NetwPropertyConfig>(this);
}

Ref<NetwPropertyConfig> NetwPropertyConfig::heartbeat(int64_t p_ticks) {
    if (is_refused() || !takes_amount("heartbeat", double(p_ticks))) {
        return Ref<NetwPropertyConfig>(this);
    }
    warn_double_set(
        set_heartbeat_ticks != UNSET && set_heartbeat_ticks != p_ticks,
        "heartbeat"
    );
    set_heartbeat_ticks = p_ticks;
    return Ref<NetwPropertyConfig>(this);
}

Ref<NetwPropertyConfig> NetwPropertyConfig::windowed(int64_t p_samples) {
    if (is_refused() || !takes_amount("windowed", double(p_samples))) {
        return Ref<NetwPropertyConfig>(this);
    }
    if (in_broadcast_set) {
        refuse_setting("windowed", KINDS[KIND_BROADCAST], ON_STATE_OR_INPUT);
        return Ref<NetwPropertyConfig>(this);
    }
    warn_double_set(set_window != UNSET && set_window != p_samples, "window");
    set_window = p_samples;
    return Ref<NetwPropertyConfig>(this);
}

Ref<NetwPropertyConfig> NetwPropertyConfig::audience(bool p_server_only) {
    if (is_refused()) {
        return Ref<NetwPropertyConfig>(this);
    }
    if (p_server_only && in_broadcast_set) {
        refuse_setting("audience", KINDS[KIND_BROADCAST], ON_STATE);
        return Ref<NetwPropertyConfig>(this);
    }
    set_audience = p_server_only ? PropertySet::AUDIENCE_SERVER_ONLY
                                 : PropertySet::AUDIENCE_PUBLIC;
    return Ref<NetwPropertyConfig>(this);
}

Ref<NetwPropertyConfig> NetwPropertyConfig::persisted(
    const StringName &p_saved_column_name
) {
    if (is_refused()) {
        return Ref<NetwPropertyConfig>(this);
    }
    persist = true;
    saved_column = p_saved_column_name;
    return Ref<NetwPropertyConfig>(this);
}

Ref<NetwPropertyConfig> NetwPropertyConfig::on_spawn() {
    if (is_refused()) {
        return Ref<NetwPropertyConfig>(this);
    }
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
