#include "netw/property_set_builder.hpp"

#include <cstring>

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

uint64_t double_bits(double p_value) {
    uint64_t bits = 0;
    memcpy(&bits, &p_value, sizeof(bits));
    return bits;
}

StringName state_rate_property(
    const Dictionary &p_configs,
    const StringName &p_property,
    const StringName &p_rate
) {
    if (p_rate == StringName()) {
        return p_rate;
    }
    const Ref<NetwPropertyConfig> rate = p_configs.get(p_rate, Variant());
    if (rate.is_valid() && rate->get_in_state_set()) {
        return p_rate;
    }
    fault::raise(
        nullptr,
        NETW_FAULT_SETTING_REFUSED,
        {String("{NetwPropertyConfig.extrapolate}"),
         vformat(
             "'%s' for '%s', which is not a state property of the same node",
             String(p_rate),
             String(p_property)
         ),
         String("the name of a property declared with "
                "{NetwPropertyConfig.state} on that node")}
    );
    return StringName();
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
    set->channel = wire::CHANNEL_SYNC;
    set->policy = p_record == PropertySet::RECORD_STATE
        ? NetwMemberConfig::POLICY_AUTHORITY
        : NetwMemberConfig::POLICY_CONTROLLER;
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
    set->profile = PropertySet::PLAIN;
    set->channel = wire::CHANNEL_PROPERTY_SYNC;
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
    int64_t window = want_input ? 2 : 0;
    StringName window_owner;
    int64_t heartbeat = 0;
    StringName heartbeat_owner;
    int64_t audience = want_input ? PropertySet::AUDIENCE_SERVER_ONLY
                                  : PropertySet::AUDIENCE_PUBLIC;

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
        column.rate_property = state_rate_property(
            p_configs,
            property,
            config->get_rate_property()
        );
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
    }

    if (set->columns.is_empty()) {
        return false;
    }

    set->policy = policy;
    set->window = window;
    set->heartbeat = heartbeat;
    set->audience = audience;
    return true;
}

bool compile_node(PropertySet &r_set, Node *p_node, int64_t p_record) {
    {
        NETW_ZONE_NC("PropertySet build columns", colors::WIRE);
        if (!from_property_configs(
                r_set,
                netw::script::model::get_declared_property_configs(p_node),
                p_record
            )) {
            return false;
        }
    }
    {
        NETW_ZONE_NC("PropertySet stamp column types", colors::WIRE);
        r_set.stamp_column_types(p_node->get_script(), p_node);
    }
    {
        NETW_ZONE_NC("PropertySet seal", colors::WIRE);
        r_set.seal();
    }
    return true;
}

uint64_t declaration_key(Node *p_node) {
    const uint64_t shared
        = netw::script::model::shared_declaration_key(p_node);
    if (shared != 0) {
        return shared;
    }
    const Dictionary configs
        = netw::script::model::get_declared_property_configs(p_node);
    const Array declared = configs.keys();
    uint64_t key = 0x9E3779B97F4A7C15ULL;
    for (int index = 0; index < declared.size(); ++index) {
        const StringName property = declared[index];
        const Ref<NetwPropertyConfig> config = configs[property];
        const Array quantizers = config->get_quantizers();
        const int type = config->get_declared_type();
        uint64_t layouts = 0;
        for (int at = 0; at < quantizers.size(); ++at) {
            const Ref<NetwQuantize> quantizer = quantizers[at];
            layouts = (layouts ^ (quantizer.is_valid()
                                      ? quantizer->layout_hash()
                                      : uint64_t(at + 1)))
                * 1099511628211ULL;
        }
        const uint64_t parts[] = {
            uint64_t(property.hash()),
            type >= 0
                ? uint64_t(type)
                : uint64_t(1) << 32
                      | uint64_t(PropertySet::column_type_for(
                          p_node->get_script(),
                          p_node,
                          property
                      )),
            uint64_t(config->get_in_state_set())
                | uint64_t(config->get_in_input_set()) << 1
                | uint64_t(config->get_in_broadcast_set()) << 2
                | uint64_t(config->get_unchecked()) << 3
                | uint64_t(config->get_explicit_teleport_only()) << 4
                | uint64_t(config->is_policy_declared()) << 5
                | uint64_t(config->get_lane()) << 8
                | uint64_t(config->get_write_policy()) << 16
                | uint64_t(config->get_set_audience()) << 24,
            uint64_t(config->get_set_heartbeat_ticks()),
            uint64_t(config->get_set_window()),
            double_bits(config->get_correction_weight()),
            double_bits(config->get_correction_threshold()),
            double_bits(config->get_teleport_threshold()),
            uint64_t(config->get_rate_property().hash()),
            layouts,
        };
        for (const uint64_t part : parts) {
            key = (key ^ part) * 1099511628211ULL;
        }
    }
    return key;
}

uint64_t from_node(Node *p_node, int64_t p_record, Object *p_api) {
    NETW_ZONE_NC("PropertySet from node", colors::WIRE);
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
    if (netw::script::model::get_declared_property_configs(p_node)
            .is_empty()) {
        return repl::SetBook::NONE;
    }
    const uint64_t declaration = declaration_key(p_node);
    const NetwMultiplayer::RecordKind record
        = NetwMultiplayer::RecordKind(p_record);
    RID adopted;
    if (!core->property_set_adopted(declaration, record, adopted)) {
        PropertySet source;
        const bool compiled = compile_node(source, p_node, p_record);
        NETW_ZONE_NC("PropertySet adopt", colors::WIRE);
        adopted = core->adopt_property_set(
            declaration,
            record,
            compiled ? &source : nullptr,
            p_node
        );
    }
    return adopted.is_valid() ? core->property_set_id_of(adopted)
                              : repl::SetBook::NONE;
}

} // namespace property_set_builder

} // namespace netw
