#include "netw/property_set_builder.hpp"

#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/property_config.hpp"
#include "netw/api/schema_core.hpp"
#include "netw/colors.hpp"
#include "netw/fault_book.hpp"
#include "netw/log.hpp"
#include "netw/profile.hpp"
#include "netw/script/model.hpp"
#include "netw/subsystems.hpp"
#include "netw/wire/registry.hpp"

using namespace godot;

namespace netw {

using repl::PropertyColumn;
using repl::PropertySet;

namespace {

uint8_t declared_channel(const char *p_name) {
    static const wire::WireRegistry declared
        = wire::WireRegistry::create_default();
    const wire::ChannelDecl *found
        = declared.find_channel_by_name(StringName(p_name));
    return found != nullptr ? found->id : 0;
}

String record_kind_name(int64_t p_record) {
    if (p_record == PropertySet::RECORD_INPUT) {
        return String("input");
    }
    if (p_record == PropertySet::RECORD_BROADCAST) {
        return String("broadcast");
    }
    return String("state");
}

void warn_set_conflict(
    int64_t p_record,
    const char *p_axis,
    const StringName &p_first,
    const StringName &p_second
) {
    fault::raise(
        nullptr,
        NETW_FAULT_DECLARED_AGAIN,
        {vformat(
             "The %s of the %s properties",
             String(p_axis),
             record_kind_name(p_record)
         ),
         vformat("%s' and '%s", String(p_first), String(p_second))}
    );
}

void warn_broadcast_ignored(const char *p_axis, const StringName &p_property) {
    fault::raise(
        nullptr,
        NETW_FAULT_CONFLICTING_SETTINGS,
        {vformat("{NetwPropertyConfig.%s}", String(p_axis)),
         String(p_property),
         "broadcast"}
    );
}

void warn_broadcast_exclusivity(const StringName &p_property) {
    fault::raise(
        nullptr,
        NETW_FAULT_CONFLICTING_SETTINGS,
        {"broadcast", String(p_property), "state or input"}
    );
}

} // namespace

namespace property_set_builder {

void for_record(PropertySet &r_set, int64_t p_record) {
    PropertySet *set = &r_set;
    set->record = p_record;
    set->profile = PropertySet::STAMPED;
    set->stamp = p_record == PropertySet::RECORD_STATE
        ? PropertySet::STAMP_TICK_ACK
        : PropertySet::STAMP_TICK;
    set->cadence = PropertySet::TICK;
    set->channel = declared_channel("SYNC");
    set->policy = p_record == PropertySet::RECORD_STATE
        ? NetwMemberConfig::POLICY_AUTHORITY
        : NetwMemberConfig::POLICY_CONTROLLER;
    set->trigger = PropertySet::TRIGGER_ON_CHANGE;
    set->window = p_record == PropertySet::RECORD_INPUT ? 2 : 0;
    set->audience = p_record == PropertySet::RECORD_INPUT
        ? PropertySet::AUDIENCE_SERVER_ONLY
        : PropertySet::AUDIENCE_PUBLIC;
}

void from_property_config(
    PropertySet &r_set,
    const StringName &p_property,
    const Ref<NetwMemberConfig> &p_config
) {
    PropertySet *set = &r_set;
    set->cadence = PropertySet::ON_DEMAND;
    set->trigger = PropertySet::TRIGGER_ON_DEMAND;
    set->profile = PropertySet::PLAIN;
    set->channel = declared_channel("PROPERTY_SYNC");
    Ref<NetwQuantize> quantizer;
    if (p_config.is_valid()) {
        set->policy = p_config->get_write_policy();
        set->reliable = p_config->get_transfer_mode()
            == NetwMemberConfig::TRANSFER_RELIABLE;
        const Array declared = p_config->get_quantizers();
        if (!declared.is_empty()) {
            quantizer = Ref<NetwQuantize>(declared[0]);
        }
    }
    set->bind_column(
        PropertyColumn::create(
            p_property,
            quantizer,
            false,
            NetwMultiplayer::ColumnType(SchemaCore::VARIANT)
        )
    );
}

bool from_property_configs(
    PropertySet &r_set,
    const Dictionary &p_configs,
    int64_t p_record
) {
    const bool want_input = p_record == PropertySet::RECORD_INPUT;
    const bool want_broadcast = p_record == PropertySet::RECORD_BROADCAST;
    for_record(r_set, p_record);
    PropertySet *set = &r_set;

    NetwMemberConfig::Policy policy = p_record == PropertySet::RECORD_STATE
        ? NetwMemberConfig::POLICY_AUTHORITY
        : NetwMemberConfig::POLICY_CONTROLLER;
    StringName policy_owner;
    int64_t trigger = PropertySet::TRIGGER_ON_CHANGE;
    StringName trigger_owner;
    int64_t window = want_input ? 2 : 0;
    StringName window_owner;
    int64_t heartbeat = 0;
    StringName heartbeat_owner;
    int64_t audience = want_input ? PropertySet::AUDIENCE_SERVER_ONLY
                                  : PropertySet::AUDIENCE_PUBLIC;
    bool masked = false;

    const Array declared = p_configs.keys();
    for (int index = 0; index < declared.size(); ++index) {
        const StringName property = declared[index];
        const Ref<NetwPropertyConfig> config
            = Ref<NetwPropertyConfig>(p_configs[property]);
        if (config.is_null()) {
            continue;
        }
        const bool marked = want_input ? config->get_in_input_set()
            : want_broadcast           ? config->get_in_broadcast_set()
                                       : config->get_in_state_set();
        if (!marked) {
            continue;
        }
        if (want_broadcast
            && (config->get_in_state_set() || config->get_in_input_set())) {
            warn_broadcast_exclusivity(property);
            continue;
        }

        Ref<NetwQuantize> quantizer;
        const Array quantizers = config->get_quantizers();
        if (!quantizers.is_empty()) {
            quantizer = Ref<NetwQuantize>(quantizers[0]);
        }
        PropertyColumn column = PropertyColumn::create(
            property,
            quantizer,
            config->get_lane() == PropertySet::RETAINED,
            NetwMultiplayer::ColumnType(SchemaCore::VARIANT)
        );
        column.unchecked = config->get_unchecked();
        column.correction_weight = config->get_correction_weight();
        column.rate_property = config->get_rate_property();
        column.explicit_teleport_only = config->get_explicit_teleport_only();
        column.correction_threshold = config->get_correction_threshold();
        column.teleport_threshold = config->get_teleport_threshold();
        set->bind_column(column);

        if (config->is_policy_declared()) {
            if (!String(policy_owner).is_empty()
                && config->get_write_policy() != policy) {
                warn_set_conflict(p_record, "policy", policy_owner, property);
            }
            policy = config->get_write_policy();
            policy_owner = property;
        }
        if (config->get_set_trigger() != NetwPropertyConfig::UNSET) {
            if (!String(trigger_owner).is_empty()
                && config->get_set_trigger() != trigger) {
                warn_set_conflict(p_record, "trigger", trigger_owner, property);
            }
            trigger = config->get_set_trigger();
            trigger_owner = property;
        }
        if (config->get_set_heartbeat_ticks() != NetwPropertyConfig::UNSET) {
            if (!String(heartbeat_owner).is_empty()
                && config->get_set_heartbeat_ticks() != heartbeat) {
                warn_set_conflict(
                    p_record,
                    "heartbeat",
                    heartbeat_owner,
                    property
                );
            }
            heartbeat = config->get_set_heartbeat_ticks();
            heartbeat_owner = property;
        }
        if (config->get_set_window() != NetwPropertyConfig::UNSET) {
            if (want_broadcast) {
                warn_broadcast_ignored("windowed", property);
            } else {
                if (!String(window_owner).is_empty()
                    && config->get_set_window() != window) {
                    warn_set_conflict(
                        p_record,
                        "window",
                        window_owner,
                        property
                    );
                }
                window = config->get_set_window();
                window_owner = property;
            }
        }
        if (config->get_set_audience() == PropertySet::AUDIENCE_SERVER_ONLY) {
            if (want_broadcast) {
                warn_broadcast_ignored("audience", property);
            } else if (!want_input) {
                audience = PropertySet::AUDIENCE_SERVER_ONLY;
            }
        }
        if (config->get_set_masked()) {
            masked = true;
        }
    }

    if (set->columns.is_empty()) {
        return false;
    }

    if (masked && window > 0) {
        fault::raise(
            nullptr,
            NETW_FAULT_CONFLICTING_SETTINGS,
            {"{NetwPropertyConfig.masked}",
             record_kind_name(p_record),
             "{NetwPropertyConfig.windowed}"}
        );
        masked = false;
    }

    set->policy = policy;
    set->trigger = trigger;
    set->window = window;
    set->heartbeat = heartbeat;
    set->audience = audience;
    set->masked = masked;
    return true;
}

bool compile_script(
    PropertySet &r_set,
    const Ref<Script> &p_script,
    int64_t p_record,
    Node *p_node
) {
    {
        NETW_ZONE_NC("PropertySet build columns", colors::WIRE);
        if (!from_property_configs(
                r_set,
                netw::script::model::get_property_configs(p_script),
                p_record
            )) {
            return false;
        }
    }
    {
        NETW_ZONE_NC("PropertySet stamp column types", colors::WIRE);
        r_set.stamp_column_types(p_script, p_node);
    }
    {
        NETW_ZONE_NC("PropertySet seal", colors::WIRE);
        r_set.seal();
    }
    return true;
}

uint64_t from_script(
    const Ref<Script> &p_script,
    int64_t p_record,
    Object *p_api,
    Node *p_node
) {
    NETW_ZONE_NC("PropertySet from script", colors::WIRE);
    NetwMultiplayer *core = nullptr;
    if (p_api != nullptr) {
        core = godot::Object::cast_to<NetwMultiplayer>(p_api);
        if (core == nullptr) {
            const Variant held = p_api->get(StringName("_native_core"));
            core = godot::Object::cast_to<NetwMultiplayer>(
                static_cast<Object *>(held)
            );
        }
    }
    if (core == nullptr) {
        return repl::SetBook::NONE;
    }
    const RID adopted = core->adopted_property_set(
        p_script,
        NetwMultiplayer::RecordKind(p_record)
    );
    if (adopted.is_valid()) {
        return core->property_set_id_of(adopted);
    }
    PropertySet source;
    if (!compile_script(source, p_script, p_record, p_node)) {
        return repl::SetBook::NONE;
    }
    {
        NETW_ZONE_NC("PropertySet adopt", colors::WIRE);
        return core->property_set_id_of(core->adopt_property_set(
            p_script,
            NetwMultiplayer::RecordKind(p_record),
            source,
            p_node
        ));
    }
}

} // namespace property_set_builder

} // namespace netw
