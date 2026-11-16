#pragma once

#include <cstdint>

#include "godot/callable.hpp"
#include "godot/variant.hpp"
#include "netw/api/member_config.hpp"
#include "netw/api/schema_model.hpp"
#include "netw/repl/property_set.hpp"

namespace netw {

class NetwPropertyConfig : public NetwMemberConfig {
    GDCLASS(NetwPropertyConfig, NetwMemberConfig)

public:
    using Lane = repl::PropertySet::Lane;
    using Audience = repl::PropertySet::Audience;

    static constexpr Lane VOLATILE = repl::PropertySet::VOLATILE;
    static constexpr Lane RETAINED = repl::PropertySet::RETAINED;
    static constexpr Audience AUDIENCE_PUBLIC
        = repl::PropertySet::AUDIENCE_PUBLIC;
    static constexpr Audience AUDIENCE_SERVER_ONLY
        = repl::PropertySet::AUDIENCE_SERVER_ONLY;
    static constexpr int64_t UNSET = -1;

private:
    int declared_type = -1;
    bool is_property = true;
    bool is_spawn_state = false;
    repl::PropertySet::Lane lane = repl::PropertySet::VOLATILE;
    bool in_state_set = false;
    bool in_input_set = false;
    bool in_broadcast_set = false;
    double correction_threshold_override = -1.0;
    double teleport_threshold_override = -1.0;
    int64_t set_heartbeat_ticks = UNSET;
    int64_t set_window = UNSET;
    repl::PropertySet::Audience set_audience
        = repl::PropertySet::AUDIENCE_PUBLIC;
    godot::StringName saved_column;
    bool persist = false;
    bool is_unchecked = false;
    double weight = 0.0;
    godot::StringName rate_property;
    bool explicit_teleport_only = false;
    uint32_t state_settings = 0;

    void warn_double_set(bool p_replaces, const char *p_axis) const;
    void refuse_setting(
        const char *p_setting,
        const char *p_kind,
        const char *p_fits
    ) const;
    bool takes_kind(int p_kind);
    void drop_settings_unread_by(int p_kind);
    bool takes_state_setting(int p_setting);
    bool takes_amount(const char *p_setting, double p_amount) const;

protected:
    static void _bind_methods();

public:
    int get_declared_type() const {
        return declared_type;
    }
    void set_declared_type(int p_type) {
        declared_type = p_type;
    }

    void set_is_property(bool p_is_property) {
        is_property = p_is_property;
    }
    bool get_is_property() const {
        return is_property;
    }

    void set_is_spawn_state(bool p_is_spawn_state) {
        is_spawn_state = p_is_spawn_state;
    }
    bool get_is_spawn_state() const {
        return is_spawn_state;
    }

    void set_lane(repl::PropertySet::Lane p_lane) {
        lane = p_lane;
    }
    repl::PropertySet::Lane get_lane() const {
        return lane;
    }

    void set_in_state_set(bool p_in_state_set) {
        in_state_set = p_in_state_set;
    }
    bool get_in_state_set() const {
        return in_state_set;
    }

    void set_in_input_set(bool p_in_input_set) {
        in_input_set = p_in_input_set;
    }
    bool get_in_input_set() const {
        return in_input_set;
    }

    void set_in_broadcast_set(bool p_in_broadcast_set) {
        in_broadcast_set = p_in_broadcast_set;
    }
    bool get_in_broadcast_set() const {
        return in_broadcast_set;
    }

    double get_correction_threshold() const {
        return correction_threshold_override;
    }
    double get_teleport_threshold() const {
        return teleport_threshold_override;
    }

    void set_set_heartbeat_ticks(int64_t p_set_heartbeat_ticks) {
        set_heartbeat_ticks = p_set_heartbeat_ticks;
    }
    int64_t get_set_heartbeat_ticks() const {
        return set_heartbeat_ticks;
    }

    void set_set_window(int64_t p_set_window) {
        set_window = p_set_window;
    }
    int64_t get_set_window() const {
        return set_window;
    }

    void set_set_audience(repl::PropertySet::Audience p_set_audience) {
        set_audience = p_set_audience;
    }
    repl::PropertySet::Audience get_set_audience() const {
        return set_audience;
    }

    bool is_persisted() const {
        return persist;
    }
    godot::StringName get_saved_column() const {
        return saved_column;
    }

    bool get_unchecked() const {
        return is_unchecked;
    }
    double get_correction_weight() const {
        return weight;
    }
    godot::StringName get_rate_property() const {
        return rate_property;
    }
    bool get_explicit_teleport_only() const {
        return explicit_teleport_only;
    }

    godot::Ref<NetwPropertyConfig> state();
    godot::Ref<NetwPropertyConfig> input();
    godot::Ref<NetwPropertyConfig> broadcast();
    godot::Ref<NetwPropertyConfig> volatile_lane();
    godot::Ref<NetwPropertyConfig> retained();
    godot::Ref<NetwPropertyConfig> unchecked();
    godot::Ref<NetwPropertyConfig> correction_weight(double p_weight);
    godot::Ref<NetwPropertyConfig> extrapolate(
        const godot::StringName &p_rate_property
    );
    godot::Ref<NetwPropertyConfig> teleport_only();
    godot::Ref<NetwPropertyConfig> correction_threshold(double p_threshold);
    godot::Ref<NetwPropertyConfig> teleport_threshold(double p_distance);
    godot::Ref<NetwPropertyConfig> heartbeat(int64_t p_ticks);
    godot::Ref<NetwPropertyConfig> windowed(int64_t p_samples);
    godot::Ref<NetwPropertyConfig> audience(bool p_server_only);
    godot::Ref<NetwPropertyConfig> persisted(
        const godot::StringName &p_saved_column_name
    );
    godot::Ref<NetwPropertyConfig> on_spawn();
};

} // namespace netw

VARIANT_ENUM_CAST(netw::NetwPropertyConfig::Lane);
VARIANT_ENUM_CAST(netw::NetwPropertyConfig::Audience);
