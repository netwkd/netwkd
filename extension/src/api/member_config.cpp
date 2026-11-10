#include "netw/api/member_config.hpp"

#include "godot/class_db.hpp"
#include "godot/node.hpp"
#include "godot/vararg.hpp"
#include "netw/api/interpolate.hpp"
#include "netw/api/quantize.hpp"
#include "netw/entity/control.hpp"
#include "netw/fault_book.hpp"
#include "netw/log.hpp"
#include "netw/member_packing.hpp"
#include "netw/script/model.hpp"

using namespace godot;

namespace netw {

namespace {

Callable &member_types_reader() {
    static Callable reader;
    return reader;
}

Node *body_behind(const Variant &p_node_ref) {
    Object *holder = p_node_ref;
    if (holder == nullptr) {
        return nullptr;
    }
    return Object::cast_to<Node>(gd::live_object(holder->call("get_ref")));
}

Array declared_member_types(
    const Ref<Script> &p_script,
    const StringName &p_member,
    int64_t p_context_type,
    const Variant &p_node_ref
) {
    if (p_context_type == 2) {
        return netw::script::model::get_signal_arg_types(p_script, p_member);
    }
    if (p_context_type != 1) {
        return netw::script::model::get_method_arg_types(p_script, p_member);
    }
    Array types;
    types.push_back(
        netw::script::model::get_node_property_type(
            body_behind(p_node_ref),
            p_member
        )
    );
    return types;
}

bool same_quantizer_layout(const Array &p_left, const Array &p_right) {
    if (p_left.size() != p_right.size()) {
        return false;
    }
    for (int index = 0; index < p_left.size(); index++) {
        const Ref<NetwQuantize> left = p_left[index];
        const Ref<NetwQuantize> right = p_right[index];
        if (left.is_null() || right.is_null()) {
            if (left != right) {
                return false;
            }
            continue;
        }
        if (!left->is_same_layout(right)) {
            return false;
        }
    }
    return true;
}

bool same_interpolator_specs(const Array &p_left, const Array &p_right) {
    if (p_left.size() != p_right.size()) {
        return false;
    }
    for (int index = 0; index < p_left.size(); index++) {
        const Ref<NetwInterpolate> left = p_left[index];
        const Ref<NetwInterpolate> right = p_right[index];
        if (left.is_null() || right.is_null()) {
            if (left != right) {
                return false;
            }
            continue;
        }
        if (!left->is_same_spec(right)) {
            return false;
        }
    }
    return true;
}

} // namespace

void NetwMemberConfig::set_member_types_reader(const Callable &p_reader) {
    member_types_reader() = p_reader;
}

Array NetwMemberConfig::member_types() const {
    if (context_name == StringName()) {
        return Array();
    }
    const Callable &reader = member_types_reader();
    if (reader.is_valid()) {
        return Array(reader.call(
            get_context_script(),
            context_name,
            context_type,
            context_node_ref
        ));
    }
    return declared_member_types(
        get_context_script(),
        context_name,
        context_type,
        context_node_ref
    );
}

void NetwMemberConfig::publish_policy() {
    const Ref<Script> declaring = get_context_script();
    if (declaring.is_null() || context_type == 0) {
        return;
    }
    entity::Control::declare_policy(
        declaring.ptr(),
        context_name,
        int64_t(write_policy),
        context_type == 2
    );
}

void NetwMemberConfig::set_write_policy(Policy p_write_policy) {
    if (policy_configured && write_policy != p_write_policy) {
        fault::raise(
            nullptr,
            NETW_FAULT_DECLARED_AGAIN,
            {"The write policy", String(context_name)}
        );
    }
    write_policy = p_write_policy;
    policy_configured = true;
    publish_policy();
}

void NetwMemberConfig::set_transfer_mode(TransferMode p_transfer_mode) {
    if (transfer_configured && transfer_mode != p_transfer_mode) {
        fault::raise(
            nullptr,
            NETW_FAULT_DECLARED_AGAIN,
            {"The transfer mode", String(context_name)}
        );
    }
    transfer_mode = p_transfer_mode;
    transfer_configured = true;
}

void NetwMemberConfig::set_is_call_local(bool p_is_call_local) {
    if (context_type == 1) {
        fault::raise(
            nullptr,
            NETW_FAULT_CONFLICTING_SETTINGS,
            {"{NetwMemberConfig.call_local}",
             String(context_name),
             "a property, which always changes locally first"}
        );
    }
    if (local_configured && is_call_local != p_is_call_local) {
        fault::raise(
            nullptr,
            NETW_FAULT_DECLARED_AGAIN,
            {"{NetwMemberConfig.call_local} or call_remote",
             String(context_name)}
        );
    }
    is_call_local = p_is_call_local;
    local_configured = true;
}

Ref<NetwMemberConfig> NetwMemberConfig::authority() {
    set_write_policy(POLICY_AUTHORITY);
    return Ref<NetwMemberConfig>(this);
}

Ref<NetwMemberConfig> NetwMemberConfig::controller() {
    set_write_policy(POLICY_CONTROLLER);
    return Ref<NetwMemberConfig>(this);
}

Ref<NetwMemberConfig> NetwMemberConfig::any_peer() {
    set_write_policy(POLICY_ANY_PEER);
    return Ref<NetwMemberConfig>(this);
}

Ref<NetwMemberConfig> NetwMemberConfig::server() {
    set_write_policy(POLICY_SERVER);
    return Ref<NetwMemberConfig>(this);
}

Ref<NetwMemberConfig> NetwMemberConfig::reliable() {
    set_transfer_mode(TRANSFER_RELIABLE);
    return Ref<NetwMemberConfig>(this);
}

Ref<NetwMemberConfig> NetwMemberConfig::unreliable() {
    set_transfer_mode(TRANSFER_UNRELIABLE);
    return Ref<NetwMemberConfig>(this);
}

Ref<NetwMemberConfig> NetwMemberConfig::call_local() {
    set_is_call_local(true);
    return Ref<NetwMemberConfig>(this);
}

Ref<NetwMemberConfig> NetwMemberConfig::call_remote() {
    set_is_call_local(false);
    return Ref<NetwMemberConfig>(this);
}

Ref<NetwMemberConfig> NetwMemberConfig::defer_until(const Signal &p_signal) {
    if (!defer_signal_name.is_empty()) {
        fault::raise(
            nullptr,
            NETW_FAULT_DECLARED_AGAIN,
            {"{NetwMemberConfig.defer_until}", String(context_name)}
        );
    }
    defer_signal_name = p_signal.get_name();
    return Ref<NetwMemberConfig>(this);
}

Ref<NetwMemberConfig> NetwMemberConfig::timeout(double p_seconds) {
    const Ref<NetwMemberConfig> spawn_config
        = netw::script::model::get_spawn_config(
            get_context_script(),
            context_name
        );
    if (context_type != 0 || spawn_config.ptr() == this) {
        fault::raise(
            nullptr,
            NETW_FAULT_CONFLICTING_SETTINGS,
            {"{NetwMemberConfig.timeout}",
             String(context_name),
             "a member that waits for no reply"}
        );
        return Ref<NetwMemberConfig>(this);
    }
    if (p_seconds <= 0.0) {
        fault::raise(
            nullptr,
            NETW_FAULT_BAD_VALUE,
            {String("{NetwMemberConfig.timeout}"),
             vformat("%s seconds for '%s'",
                     String::num(p_seconds),
                     String(context_name)),
             String("a number of seconds above zero")}
        );
        return Ref<NetwMemberConfig>(this);
    }
    timeout_seconds = p_seconds;
    return Ref<NetwMemberConfig>(this);
}

Ref<NetwMemberConfig> NetwMemberConfig::quantize(const Array &p_quantizers) {
    if (p_quantizers.is_empty()) {
        fault::raise(
            nullptr,
            NETW_FAULT_BAD_VALUE,
            {String("{NetwMemberConfig.quantize}"),
             vformat("no quantizer for '%s'", String(context_name)),
             String("one quantizer for each value")}
        );
        return Ref<NetwMemberConfig>(this);
    }
    if (!declared_are_quantizers(
            p_quantizers,
            "NetwMemberConfig.quantize",
            context_name
        )) {
        return Ref<NetwMemberConfig>(this);
    }
    const Array types = member_types();
    if (!declared_count_fits_arity(
            p_quantizers.size(),
            types,
            "NetwMemberConfig.quantize",
            context_name,
            get_context_script()
        )) {
        return Ref<NetwMemberConfig>(this);
    }
    if (!quantizers.is_empty()
        && !same_quantizer_layout(quantizers, p_quantizers)) {
        fault::raise(
            nullptr,
            NETW_FAULT_DECLARED_AGAIN,
            {"{NetwMemberConfig.quantize}", String(context_name)}
        );
    }
    quantizers = p_quantizers;
    quantizers_fit_types(quantizers, types, context_name, get_context_script());
    return Ref<NetwMemberConfig>(this);
}

void NetwMemberConfig::set_interpolators(const Array &p_interpolators) {
    interpolators = p_interpolators;
    netw::script::model::advance_interpolation_epoch();
}

Ref<NetwMemberConfig> NetwMemberConfig::interpolate(
    const Array &p_interpolators
) {
    if (p_interpolators.is_empty()) {
        fault::raise(
            nullptr,
            NETW_FAULT_BAD_VALUE,
            {String("{NetwMemberConfig.interpolate}"),
             vformat("no interpolator for '%s'", String(context_name)),
             String("one interpolator for each value")}
        );
        return Ref<NetwMemberConfig>(this);
    }
    if (context_type == 1 && gd::indexed_path(context_name) != nullptr) {
        fault::raise(
            nullptr,
            NETW_FAULT_UNSUPPORTED,
            {String("{NetwMemberConfig.interpolate}"),
             vformat("'%s', a value inside a resource", String(context_name)),
             String("it on a property of the node")}
        );
        return Ref<NetwMemberConfig>(this);
    }
    if (!declared_are_interpolators(
            p_interpolators,
            "NetwMemberConfig.interpolate",
            context_name
        )) {
        return Ref<NetwMemberConfig>(this);
    }
    if (same_interpolator_specs(p_interpolators, interpolators)) {
        return Ref<NetwMemberConfig>(this);
    }
    const Array types = member_types();
    if (!declared_count_fits_arity(
            p_interpolators.size(),
            types,
            "NetwMemberConfig.interpolate",
            context_name,
            get_context_script()
        )) {
        return Ref<NetwMemberConfig>(this);
    }
    if (!interpolators.is_empty()) {
        fault::raise(
            nullptr,
            NETW_FAULT_DECLARED_AGAIN,
            {"{NetwMemberConfig.interpolate}", String(context_name)}
        );
    }
    interpolators = p_interpolators;
    netw::script::model::advance_interpolation_epoch();
    interpolators_fit_types(
        interpolators,
        types,
        context_name,
        get_context_script()
    );
    return Ref<NetwMemberConfig>(this);
}

bool NetwMemberConfig::is_interpolation_only() const {
    return !interpolators.is_empty() && !policy_configured
        && !transfer_configured && quantizers.is_empty();
}

void NetwMemberConfig::_bind_methods() {
    ClassDB::bind_method(D_METHOD("authority"), &NetwMemberConfig::authority);
    ClassDB::bind_method(D_METHOD("controller"), &NetwMemberConfig::controller);
    ClassDB::bind_method(D_METHOD("any_peer"), &NetwMemberConfig::any_peer);
    ClassDB::bind_method(D_METHOD("server"), &NetwMemberConfig::server);
    ClassDB::bind_method(D_METHOD("reliable"), &NetwMemberConfig::reliable);
    ClassDB::bind_method(D_METHOD("unreliable"), &NetwMemberConfig::unreliable);
    ClassDB::bind_method(D_METHOD("call_local"), &NetwMemberConfig::call_local);
    ClassDB::bind_method(
        D_METHOD("call_remote"),
        &NetwMemberConfig::call_remote
    );
    ClassDB::bind_method(
        D_METHOD("defer_until", "sig"),
        &NetwMemberConfig::defer_until
    );
    ClassDB::bind_method(
        D_METHOD("timeout", "seconds"),
        &NetwMemberConfig::timeout
    );
    gd::bind_vararg_method(D_METHOD("quantize"), &NetwMemberConfig::quantize);
    gd::bind_vararg_method(
        D_METHOD("interpolate"),
        &NetwMemberConfig::interpolate
    );
    ClassDB::bind_method(
        D_METHOD("is_policy_declared"),
        &NetwMemberConfig::is_policy_declared
    );
    ClassDB::bind_method(
        D_METHOD("is_transfer_declared"),
        &NetwMemberConfig::is_transfer_declared
    );
    ClassDB::bind_method(
        D_METHOD("is_interpolation_only"),
        &NetwMemberConfig::is_interpolation_only
    );

    ClassDB::bind_method(
        D_METHOD("set_context_script", "context_script"),
        &NetwMemberConfig::set_context_script
    );
    ClassDB::bind_method(
        D_METHOD("get_context_script"),
        &NetwMemberConfig::get_context_script
    );
    ADD_PROPERTY(
        PropertyInfo(
            Variant::OBJECT,
            "context_script",
            PROPERTY_HINT_RESOURCE_TYPE,
            "Script"
        ),
        "set_context_script",
        "get_context_script"
    );

    ClassDB::bind_method(
        D_METHOD("set_context_node_ref", "context_node_ref"),
        &NetwMemberConfig::set_context_node_ref
    );
    ClassDB::bind_method(
        D_METHOD("get_context_node_ref"),
        &NetwMemberConfig::get_context_node_ref
    );
    ADD_PROPERTY(
        PropertyInfo(
            Variant::NIL,
            "context_node_ref",
            PROPERTY_HINT_NONE,
            "",
            PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_NIL_IS_VARIANT
        ),
        "set_context_node_ref",
        "get_context_node_ref"
    );

    ClassDB::bind_method(
        D_METHOD("set_write_policy", "write_policy"),
        &NetwMemberConfig::set_write_policy
    );
    ClassDB::bind_method(
        D_METHOD("get_write_policy"),
        &NetwMemberConfig::get_write_policy
    );
    ADD_PROPERTY(
        PropertyInfo(
            Variant::INT,
            "write_policy",
            PROPERTY_HINT_ENUM,
            "Authority,Controller,Any Peer,Server"
        ),
        "set_write_policy",
        "get_write_policy"
    );

    ClassDB::bind_method(
        D_METHOD("set_transfer_mode", "transfer_mode"),
        &NetwMemberConfig::set_transfer_mode
    );
    ClassDB::bind_method(
        D_METHOD("get_transfer_mode"),
        &NetwMemberConfig::get_transfer_mode
    );
    ADD_PROPERTY(
        PropertyInfo(
            Variant::INT,
            "transfer_mode",
            PROPERTY_HINT_ENUM,
            "Reliable,Unreliable"
        ),
        "set_transfer_mode",
        "get_transfer_mode"
    );

#define NETW_MEMBER_CONFIG_PROPERTY(m_type, m_name) \
    ClassDB::bind_method( \
        D_METHOD("set_" #m_name, #m_name), \
        &NetwMemberConfig::set_##m_name \
    ); \
    ClassDB::bind_method( \
        D_METHOD("get_" #m_name), \
        &NetwMemberConfig::get_##m_name \
    ); \
    ADD_PROPERTY(PropertyInfo(m_type, #m_name), "set_" #m_name, "get_" #m_name)

    NETW_MEMBER_CONFIG_PROPERTY(Variant::BOOL, is_call_local);
    NETW_MEMBER_CONFIG_PROPERTY(Variant::STRING_NAME, context_name);
    NETW_MEMBER_CONFIG_PROPERTY(Variant::INT, context_type);
    NETW_MEMBER_CONFIG_PROPERTY(Variant::STRING_NAME, defer_signal_name);
    NETW_MEMBER_CONFIG_PROPERTY(Variant::ARRAY, quantizers);
    NETW_MEMBER_CONFIG_PROPERTY(Variant::ARRAY, interpolators);

#undef NETW_MEMBER_CONFIG_PROPERTY

    BIND_ENUM_CONSTANT(POLICY_AUTHORITY);
    BIND_ENUM_CONSTANT(POLICY_CONTROLLER);
    BIND_ENUM_CONSTANT(POLICY_ANY_PEER);
    BIND_ENUM_CONSTANT(POLICY_SERVER);

    BIND_ENUM_CONSTANT(TRANSFER_RELIABLE);
    BIND_ENUM_CONSTANT(TRANSFER_UNRELIABLE);
}

} // namespace netw
