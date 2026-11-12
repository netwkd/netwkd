#include "netw/api/context.hpp"

#include "godot/class_db.hpp"
#include "godot/engine.hpp"
#include "godot/object.hpp"
#include "godot/os.hpp"
#include "godot/utility.hpp"
#include "godot/vararg.hpp"
#include "netw/api/entity_record.hpp"
#include "netw/api/link_conditions.hpp"
#include "netw/api/native_api.hpp"
#include "netw/api/replication_core.hpp"
#include "netw/colors.hpp"
#include "netw/connect/transport.hpp"
#include "netw/decl_sites.hpp"
#include "netw/fault_book.hpp"
#include "netw/log.hpp"
#include "netw/profile.hpp"
#include "netw/scene_core.hpp"
#include "netw/scene_decl.hpp"
#include "netw/script/model.hpp"
#include "netw/session_decl.hpp"
#include "netw/tree_hook.hpp"

using namespace godot;

namespace netw {

namespace {

constexpr const char *META_FIRED_PREFIX = "_netw_fired_";

Ref<Script> script_of(Object *p_object) {
    if (p_object == nullptr) {
        return Ref<Script>();
    }
    return Ref<Script>(Object::cast_to<Script>(p_object->get_script()));
}

Ref<Script> named_method_script(
    const Callable &p_callable,
    const char *p_verb
) {
    const StringName method = p_callable.get_method();
    Object *target = p_callable.get_object();
    const Ref<Script> script = script_of(target);
    if (script.is_null() || method.is_empty() || !target->has_method(method)) {
        Node *host = Object::cast_to<Node>(target);
        fault::raise(
            nullptr,
            NETW_FAULT_UNKNOWN_NAME,
            {vformat("{Netw.%s}", p_verb),
             host != nullptr ? String(host->get_name()) : String("the node"),
             String(method)},
            host != nullptr ? uint64_t(gd::instance_id(host)) : 0
        );
        return Ref<Script>();
    }
    return script;
}

Node *node_behind(const Variant &p_held) {
    return Object::cast_to<Node>(gd::live_object(p_held));
}

Node *handler_scope(const Callable &p_handler, const char *p_verb) {
    if (!p_handler.is_valid()) {
        fault::raise(
            nullptr,
            NETW_FAULT_BAD_CALLABLE,
            {vformat("{Netw.%s}", p_verb),
             String(),
             String("a method of a node inside the session's branch")}
        );
        return nullptr;
    }
    Node *scope = Object::cast_to<Node>(p_handler.get_object());
    if (scope == nullptr) {
        fault::raise(
            nullptr,
            NETW_FAULT_BAD_CALLABLE,
            {vformat("{Netw.%s}", p_verb),
             String(),
             String("a method of a Node inside the session's branch")}
        );
        return nullptr;
    }
    return scope;
}

template <typename T>
Ref<T> declare_config(Node *p_node, session_decl::Kind p_kind) {
    const char *verb = session_decl::verb_of(p_kind);
    if (p_node == nullptr) {
        fault::raise(
            nullptr,
            NETW_FAULT_EMPTY_ARGUMENT,
            {vformat("{Netw.%s}", verb), String(), String("node")}
        );
        return Ref<T>();
    }
    const Ref<T> standing = session_decl::payload_on(p_node, p_kind);
    if (standing.is_valid()) {
        fault::raise(
            nullptr,
            NETW_FAULT_DECLARED_TWICE,
            {vformat("{Netw.%s}", verb), String(p_node->get_name())},
            uint64_t(gd::instance_id(p_node))
        );
        return Ref<T>();
    }
    Ref<T> draft;
    draft.instantiate();
    if (session_decl::declare(p_node, p_kind, Callable(), draft, verb) != OK) {
        return Ref<T>();
    }
    return draft;
}

NetwMultiplayer *sole_session(const char *p_verb) {
    const TypedArray<MultiplayerAPI> live = NetwMultiplayer::session_get_all();
    if (live.is_empty()) {
        fault::raise(
            nullptr,
            NETW_FAULT_SESSION_NOT_FOUND,
            {vformat("{Netw.%s}", p_verb), String(), String(), 0, 0}
        );
        return nullptr;
    }
    if (live.size() > 1) {
        fault::raise(
            nullptr,
            NETW_FAULT_SESSION_NOT_FOUND,
            {vformat("{Netw.%s}", p_verb),
             String(),
             String(),
             0,
             int64_t(live.size())}
        );
        return nullptr;
    }
    const Ref<MultiplayerAPI> only = live[0];
    return Object::cast_to<NetwMultiplayer>(only.ptr());
}

struct CallSession {
    NetwMultiplayer *api = nullptr;
    Error code = OK;
    String detail;
};

CallSession call_session(Node *p_node, const StringName &p_member) {
    CallSession out;
    if (p_node == nullptr) {
        out.code = ERR_INVALID_PARAMETER;
        out.detail = String("A Netw RPC needs a method of a Node.");
        fault::raise(
            nullptr,
            NETW_FAULT_BAD_CALLABLE,
            {String("A Netw RPC"), String(), String("a method of a Node")}
        );
        return out;
    }
    out.api = NetwMultiplayer::of(p_node);
    if (out.api != nullptr) {
        return out;
    }
    out.code = ERR_UNCONFIGURED;
    out.detail = vformat(
        "'%s' is not inside a session, so the call to '%s' was dropped. Add "
        "it to the scene tree under a MultiplayerViewport.",
        String(p_node->get_name()),
        String(p_member)
    );
    static HashSet<uint64_t> reported;
    const uint64_t node_id = uint64_t(gd::instance_id(p_node));
    if (!reported.has(node_id)) {
        reported.insert(node_id);
        fault::raise(
            nullptr,
            NETW_FAULT_NOT_IN_SESSION,
            {String("The call to '") + String(p_member) + String("'"),
             String(p_node->get_name())},
            node_id
        );
    }
    return out;
}

CallSession call_session(const Callable &p_callable) {
    return call_session(
        Object::cast_to<Node>(p_callable.get_object()),
        p_callable.get_method()
    );
}

void mark_signal_fired(const Variant &p_node, const String &p_meta_key) {
    Node *node = node_behind(p_node);
    if (node != nullptr) {
        node->set_meta(StringName(p_meta_key), true);
    }
}

void setup_defer_latches(Node *p_node) {
    const Ref<Script> script = script_of(p_node);
    if (script.is_null()) {
        return;
    }
    const Dictionary configs = netw::script::model::get_rpc_configs(script);
    const Array methods = configs.keys();
    for (int at = 0; at < methods.size(); ++at) {
        const Ref<NetwMemberConfig> config = configs[methods[at]];
        if (config.is_null()) {
            continue;
        }
        const StringName latched = config->get_defer_signal_name();
        if (String(latched).is_empty() || latched == StringName("ready")) {
            continue;
        }
        if (!p_node->has_signal(latched)) {
            continue;
        }
        const String meta_key = String(META_FIRED_PREFIX) + String(latched);
        if (p_node->has_meta(StringName(meta_key))) {
            continue;
        }
        p_node->set_meta(StringName(meta_key), false);
        p_node->connect(
            latched,
            callable_mp_static(&mark_signal_fired)
                .bind(Variant(p_node), meta_key),
            Object::CONNECT_ONE_SHOT
        );
    }
}

void register_as_component(const Variant &p_node) {
    NETW_ZONE_NC("Netw register as component", colors::SESSION);
    Node *node = node_behind(p_node);
    if (node == nullptr) {
        return;
    }
    const Ref<NetwEntity> entity = NetwEntity::of(node);
    if (entity.is_valid()) {
        entity->register_component(node);
    } else {
        fault::raise(
            nullptr,
            NETW_FAULT_NOT_ENTITY,
            {String("Networked configuration"),
             String(node->get_name()),
             String()},
            gd::instance_id(node)
        );
    }
    setup_defer_latches(node);
}

void inject_component_registration(Node *p_node) {
    if (tree::marked(p_node, tree::MARK_COMPONENT_SEEN)) {
        return;
    }
    if (p_node->is_inside_tree()) {
        tree::mark(p_node, tree::MARK_COMPONENT_SEEN);
        register_as_component(Variant(p_node));
        return;
    }
    tree::mark(p_node, tree::MARK_COMPONENT_SEEN | tree::MARK_COMPONENT);
}

void register_derived_now(const Variant &p_node) {
    Node *node = node_behind(p_node);
    if (node == nullptr) {
        return;
    }
    if (NetwMultiplayer *api = NetwMultiplayer::of(node)) {
        api->sync_register_derived(node);
    }
}

void unregister_derived_leaving(Node *p_node) {
    if (NetwMultiplayer *api = NetwMultiplayer::of_leaving(p_node)) {
        api->sync_unregister_derived(p_node);
    }
}

void schedule_derived_registration(Node *p_node) {
    if (tree::marked(p_node, tree::MARK_DERIVED_EXIT)) {
        return;
    }
    if (p_node->is_inside_tree()) {
        tree::mark(p_node, tree::MARK_DERIVED_EXIT);
        register_derived_now(Variant(p_node));
        return;
    }
    tree::mark(p_node, tree::MARK_DERIVED_EXIT | tree::MARK_DERIVED_ENTER);
}

bool is_persist_only(const Ref<NetwPropertyConfig> &p_config) {
    return p_config->is_persisted() && !p_config->is_policy_declared()
        && !p_config->is_transfer_declared()
        && p_config->get_quantizers().is_empty();
}

void lint_property_overlaps(const Variant &p_node) {
    Node *node = node_behind(p_node);
    if (node == nullptr) {
        return;
    }
    const Ref<Script> script = script_of(node);
    if (script.is_null()) {
        return;
    }
    const Dictionary configs
        = netw::script::model::get_property_configs(script);
    if (configs.is_empty()) {
        return;
    }
    const Ref<NetwEntity> entity = NetwEntity::of(node);
    if (entity.is_null() || entity->get_owner() == nullptr) {
        return;
    }
    const Array properties = configs.keys();
    for (int at = 0; at < properties.size(); ++at) {
        const StringName property = properties[at];
        const Ref<NetwMemberConfig> config = configs[properties[at]];
        if (config.is_valid() && config->is_interpolation_only()) {
            continue;
        }
        const Ref<NetwPropertyConfig> declared = config;
        if (declared.is_valid()) {
            if (declared->get_in_state_set() || declared->get_in_input_set()) {
                continue;
            }
            if (is_persist_only(declared)) {
                continue;
            }
        }
        const NodePath real = entity->property_path(node, property, nullptr);
        if (!real.is_empty() && entity->governs_property(real, nullptr)) {
            fault::raise(
                nullptr,
                NETW_FAULT_DECLARED_AGAIN,
                {String(property), String(node->get_name())},
                gd::instance_id(node)
            );
        }
    }
}

void inject_property_lint(Node *p_node) {
    if (tree::marked(p_node, tree::MARK_LINT_SEEN)) {
        return;
    }
    if (p_node->is_inside_tree()) {
        tree::mark(p_node, tree::MARK_LINT_SEEN);
        lint_property_overlaps(Variant(p_node));
        return;
    }
    tree::mark(p_node, tree::MARK_LINT_SEEN | tree::MARK_LINT);
}

} // namespace

void tree::declarations_entered(Node *p_node, uint16_t p_marks) {
    const Variant node(p_node);
    if ((p_marks & tree::MARK_LINT) != 0) {
        lint_property_overlaps(node);
    }
    if ((p_marks & tree::MARK_COMPONENT) != 0) {
        register_as_component(node);
    }
    if ((p_marks & tree::MARK_DERIVED_ENTER) != 0) {
        register_derived_now(node);
    }
}

void tree::declarations_exiting(Node *p_node) {
    unregister_derived_leaving(p_node);
}

Dictionary Netw::native_api() {
    return native_api_table();
}

Ref<NetwMultiplayer> Netw::of(Node *p_node) {
    return NetwMultiplayer::core_of(p_node);
}

Ref<NetwSession> Netw::session(Node *p_node) {
    const Ref<NetwMultiplayer> api = NetwMultiplayer::core_of(p_node);
    if (api.is_null()) {
        return Ref<NetwSession>();
    }
    return api->get_session();
}

Ref<NetwPromise> Netw::prepare_join(
    Node *p_node,
    const StringName &p_username,
    const Array &p_args
) {
    const Ref<NetwMultiplayer> api = NetwMultiplayer::core_of(p_node);
    const uint64_t node_id
        = p_node != nullptr ? uint64_t(gd::instance_id(p_node)) : 0;
    if (api.is_null()) {
        fault::raise(
            nullptr,
            NETW_FAULT_NOT_IN_SESSION,
            {String("{Netw.prepare_join}"),
             p_node != nullptr ? String(p_node->get_name()) : String("<null>")},
            node_id
        );
        return NetwPromise::resolved(ERR_UNCONFIGURED);
    }
    if (api->is_online()) {
        fault::raise(
            api.ptr(),
            NETW_FAULT_DECLARED_ONLINE,
            {String("{Netw.prepare_join}"),
             String(),
             String("the join arguments")},
            node_id
        );
        return NetwPromise::resolved(ERR_ALREADY_IN_USE);
    }
    return api->session_prepare_join(p_username, p_args);
}

Ref<NetwScene> Netw::scene(Node *p_node) {
    Node *inner = NetwMultiplayer::scene_inner_of(p_node);
    if (NetwMultiplayer::core_of(inner).is_null()) {
        return Ref<NetwScene>();
    }
    const Ref<NetwEntity> record = NetwEntity::of(inner);
    return record.is_valid() ? record->get_scene() : Ref<NetwScene>();
}

Error Netw::configure_server_info(const Callable &p_provider) {
    Node *scope = handler_scope(p_provider, "configure_server_info");
    if (scope == nullptr) {
        return ERR_INVALID_PARAMETER;
    }
    decl_sites::record_handler(p_provider);
    return session_decl::declare(
        scope,
        session_decl::KIND_SERVER_INFO,
        p_provider,
        Variant(),
        "configure_server_info"
    );
}

Ref<NetwJoinConfig> Netw::configure_join(const Callable &p_handler) {
    Node *scope = handler_scope(p_handler, "configure_join");
    if (scope == nullptr) {
        return Ref<NetwJoinConfig>();
    }
    const Ref<Script> declaring
        = netw::script::model::declaring_script(p_handler.get_object());
    if (NetwMultiplayer::join_declared_arg_types(p_handler).is_empty()
        && p_handler.get_argument_count() > 1) {
        fault::raise(
            nullptr,
            NETW_FAULT_BAD_CALLABLE,
            {String("{Netw.configure_join}"),
             String(),
             String("a named method with typed parameters for the join "
                    "arguments")},
            uint64_t(gd::instance_id(scope))
        );
        return Ref<NetwJoinConfig>();
    }
    decl_sites::record_handler(p_handler);
    Ref<NetwJoinConfig> config;
    config.instantiate();
    config->set_context_script(declaring);
    config->set_context_name(p_handler.get_method());
    if (session_decl::declare(
            scope,
            session_decl::KIND_JOIN,
            p_handler,
            config,
            "configure_join"
        )
        != OK) {
        return Ref<NetwJoinConfig>();
    }
    return config;
}

Ref<NetwAdmissionConfig> Netw::configure_admission(const Callable &p_handler) {
    Node *scope = handler_scope(p_handler, "configure_admission");
    if (scope == nullptr) {
        return Ref<NetwAdmissionConfig>();
    }
    decl_sites::record_handler(p_handler);
    Ref<NetwAdmissionConfig> config;
    config.instantiate();
    if (session_decl::declare(
            scope,
            session_decl::KIND_ADMISSION,
            p_handler,
            config,
            "configure_admission"
        )
        != OK) {
        return Ref<NetwAdmissionConfig>();
    }
    return config;
}

Error Netw::configure_transfer(const Callable &p_keep) {
    Node *scope = handler_scope(p_keep, "configure_transfer");
    if (scope == nullptr) {
        return ERR_INVALID_PARAMETER;
    }
    decl_sites::record_handler(p_keep);
    return session_decl::declare(
        scope,
        session_decl::KIND_TRANSFER,
        p_keep,
        Variant(),
        "configure_transfer"
    );
}

Ref<NetwSessionConfig> Netw::configure_session(Node *p_node) {
    return declare_config<NetwSessionConfig>(
        p_node,
        session_decl::KIND_SESSION_CONFIG
    );
}

Ref<NetwClockConfig> Netw::configure_clock(Node *p_node) {
    decl_sites::record(NETW_SITE_CLOCK, p_node);
    return declare_config<NetwClockConfig>(
        p_node,
        session_decl::KIND_CLOCK_CONFIG
    );
}

Ref<NetwLagCompensationConfig> Netw::configure_lagcomp(Node *p_node) {
    return declare_config<NetwLagCompensationConfig>(
        p_node,
        session_decl::KIND_LAGCOMP_CONFIG
    );
}

Ref<NetwPropertyConfig> Netw::configure_property(
    Node *p_node,
    const StringName &p_property,
    int64_t p_declared_type
) {
    NETW_ZONE_NC("Netw configure property", colors::SESSION);
    if (p_node == nullptr) {
        fault::raise(
            nullptr,
            NETW_FAULT_EMPTY_ARGUMENT,
            {String("{Netw.configure_property}"), String(), String("node")}
        );
        return Ref<NetwPropertyConfig>();
    }
    const uint64_t node_id = uint64_t(gd::instance_id(p_node));
    if (p_node->is_inside_tree()) {
        fault::raise(
            nullptr,
            NETW_FAULT_TOO_LATE,
            {String("{Netw.configure_property}"),
             String("'") + String(p_node->get_name()) + String("'"),
             String("the node enters the tree")},
            node_id
        );
        return Ref<NetwPropertyConfig>();
    }
    const bool indexed = gd::declare_indexed_path(p_property) != nullptr;
    const gd::PropertyResolution resolved
        = gd::resolve_property(p_node, p_property, gd::property_list(p_node));
    if (!resolved.present) {
        fault::raise(
            nullptr,
            NETW_FAULT_UNKNOWN_NAME,
            {String("{Netw.configure_property}"),
             String(p_node->get_name()),
             String(p_property)},
            node_id
        );
        return Ref<NetwPropertyConfig>();
    }
    if (p_declared_type < -1 || p_declared_type >= Variant::VARIANT_MAX
        || (resolved.type >= 0 && p_declared_type >= 0
            && resolved.type != p_declared_type)) {
        fault::raise(
            nullptr,
            NETW_FAULT_BAD_VALUE,
            {String("{Netw.configure_property}"),
             vformat("a declared type of %d for '%s'",
                     p_declared_type,
                     String(p_property)),
             String("the Variant type of the property, or -1")},
            node_id
        );
        return Ref<NetwPropertyConfig>();
    }
    const int declared_type = p_declared_type >= 0 ? int(p_declared_type)
        : indexed                                  ? -1
                                                   : resolved.type;
    if (declared_type < 0 && !indexed) {
        fault::raise(
            nullptr,
            NETW_FAULT_BAD_VALUE,
            {String("{Netw.configure_property}"),
             vformat("no declared type for '%s'", String(p_property)),
             String("a declared_type, because the property has no fixed "
                    "type")},
            node_id
        );
        return Ref<NetwPropertyConfig>();
    }
    decl_sites::record(NETW_SITE_COLUMN, p_node, p_property);
    const Ref<Script> script = script_of(p_node);
    if (script.is_null()) {
        const Ref<NetwPropertyConfig> config
            = netw::script::model::configure_node_property(p_node, p_property);
        if (config.is_valid()) {
            config->set_declared_type(declared_type);
        }
        return config;
    }
    inject_property_lint(p_node);
    inject_component_registration(p_node);
    schedule_derived_registration(p_node);
    const Ref<NetwPropertyConfig> existing
        = netw::script::model::get_property_config(script, p_property);
    if (existing.is_valid()) {
        if (existing->get_declared_type() != declared_type) {
            fault::raise(
                nullptr,
                NETW_FAULT_DECLARED_TWICE,
                {String("{Netw.configure_property}"),
                 String(p_node->get_name()),
                 String(p_property)},
                node_id
            );
            return Ref<NetwPropertyConfig>();
        }
        return existing;
    }
    Ref<NetwPropertyConfig> config;
    config.instantiate();
    config->set_declared_type(declared_type);
    config->set_context_script(script);
    config->set_context_name(p_property);
    config->set_context_type(1);
    config->set_context_node_ref(gd::weak_ref(p_node));
    netw::script::model::declare_property_config(script, p_property, config);
    return config;
}

Ref<NetwMemberConfig> Netw::configure_rpc(const Callable &p_callable) {
    const Ref<Script> script = named_method_script(p_callable, "configure_rpc");
    if (script.is_null()) {
        return Ref<NetwMemberConfig>();
    }
    const StringName method = p_callable.get_method();
    Node *host = Object::cast_to<Node>(p_callable.get_object());
    if (host != nullptr) {
        if (host->is_inside_tree()) {
            fault::raise(
                nullptr,
                NETW_FAULT_TOO_LATE,
                {String("{Netw.configure_rpc}"),
                 String("'") + String(host->get_name()) + String("'"),
                 String("the node enters the tree")},
                uint64_t(gd::instance_id(host))
            );
            return Ref<NetwMemberConfig>();
        }
        inject_component_registration(host);
    }
    decl_sites::record(NETW_SITE_RPC, p_callable.get_object(), method);
    const Ref<NetwMemberConfig> existing
        = netw::script::model::get_rpc_options(script, method);
    if (existing.is_valid()) {
        return existing;
    }
    Ref<NetwMemberConfig> config;
    config.instantiate();
    config->set_context_script(script);
    config->set_context_name(method);
    config->set_context_type(0);
    if (netw::script::model::get_method_call_local(script, method)) {
        config->set_is_call_local(true);
    }
    netw::script::model::declare_rpc_config(script, method, config);
    return config;
}

Ref<NetwMemberConfig> Netw::configure_signal(const Signal &p_signal) {
    Node *node = Object::cast_to<Node>(p_signal.get_object());
    if (node == nullptr) {
        fault::raise(
            nullptr,
            NETW_FAULT_EMPTY_ARGUMENT,
            {String("{Netw.configure_signal}"), String(), String("signal")}
        );
        return Ref<NetwMemberConfig>();
    }
    const StringName name = p_signal.get_name();
    const Ref<Script> script = script_of(node);
    if (script.is_null() || name.is_empty() || !node->has_signal(name)) {
        fault::raise(
            nullptr,
            NETW_FAULT_UNKNOWN_NAME,
            {String("{Netw.configure_signal}"),
             String(node->get_name()),
             String(name)},
            uint64_t(gd::instance_id(node))
        );
        return Ref<NetwMemberConfig>();
    }
    const Ref<NetwMemberConfig> existing
        = netw::script::model::get_signal_config(script, name);
    if (existing.is_valid()) {
        return existing;
    }
    Ref<NetwMemberConfig> config;
    config.instantiate();
    config->default_call_local(true);
    config->set_context_script(script);
    config->set_context_name(name);
    config->set_context_type(2);
    netw::script::model::declare_signal_config(script, name, config);
    return config;
}

Ref<NetwMemberConfig> Netw::configure_spawn(const Callable &p_callable) {
    const Ref<Script> script
        = named_method_script(p_callable, "configure_spawn");
    if (script.is_null()) {
        return Ref<NetwMemberConfig>();
    }
    const StringName method = p_callable.get_method();
    decl_sites::record_handler(p_callable);
    const Ref<NetwMemberConfig> existing
        = netw::script::model::get_spawn_config(script, method);
    if (existing.is_valid()) {
        return existing;
    }
    Ref<NetwMemberConfig> config;
    config.instantiate();
    config->set_context_script(script);
    config->set_context_name(method);
    config->set_context_type(0);
    netw::script::model::declare_spawn_config(script, method, config);
    return config;
}

Ref<NetwEntity> Netw::configure_entity(Node *p_node) {
    NETW_ZONE_NC("Netw configure entity", colors::SESSION);
    decl_sites::record(NETW_SITE_KIND, p_node);
    const Ref<NetwEntity> entity = NetwEntity::declare(p_node);
    if (p_node != nullptr && p_node->is_inside_tree()
        && (entity.is_null() || entity->get_owner() != p_node)) {
        fault::raise(
            nullptr,
            NETW_FAULT_TOO_LATE,
            {String("{Netw.configure_entity}"),
             String("'") + String(p_node->get_name()) + String("'"),
             String("the node enters the tree")},
            uint64_t(gd::instance_id(p_node))
        );
        return Ref<NetwEntity>();
    }
    return entity;
}

Ref<NetwDespawnConfig> Netw::configure_despawn(Node *p_node) {
    const Ref<Script> script = script_of(p_node);
    if (script.is_null()) {
        fault::raise(
            nullptr,
            NETW_FAULT_BAD_SCRIPT,
            {String("{Netw.configure_despawn}"),
             p_node != nullptr ? vformat("'%s'", String(p_node->get_name()))
                               : String("no node"),
             String("a node with a script")},
            p_node != nullptr ? uint64_t(gd::instance_id(p_node)) : 0
        );
        return Ref<NetwDespawnConfig>();
    }
    const Ref<NetwDespawnConfig> existing
        = netw::script::model::get_own_despawn_config(script);
    if (existing.is_valid()) {
        return existing;
    }
    Ref<NetwDespawnConfig> config;
    config.instantiate();
    netw::script::model::declare_despawn_config(script, config);
    return config;
}

Ref<NetwPersistenceConfig> Netw::configure_persistence(Node *p_node) {
    if (p_node == nullptr) {
        fault::raise(
            nullptr,
            NETW_FAULT_EMPTY_ARGUMENT,
            {String("{Netw.configure_persistence}"), String(), String("node")}
        );
        return Ref<NetwPersistenceConfig>();
    }
    return netw::script::model::configure_node_persistence(p_node);
}

Ref<NetwSchema> Netw::configure_schema(const StringName &p_name) {
    return NetwSchema::declare(p_name);
}

void Netw::rpc(const Callable &p_callable, const Array &p_args) {
    rpc_id(0, p_callable, p_args);
}

void Netw::rpc_id(
    int64_t p_peer_id,
    const Callable &p_callable,
    const Array &p_args
) {
    const CallSession session = call_session(p_callable);
    if (session.api != nullptr) {
        session.api->rpc_call(p_callable, p_args, p_peer_id);
    }
}

void Netw::rpc_controller(const Callable &p_callable, const Array &p_args) {
    const CallSession session = call_session(p_callable);
    if (session.api != nullptr) {
        session.api->rpc_call_controller(p_callable, p_args);
    }
}

Ref<NetwPromise> Netw::request_id(
    int64_t p_peer_id,
    const Callable &p_callable,
    const Array &p_args
) {
    if (p_peer_id == 0) {
        const String detail = String(
            "Netw.request_id got peer id 0, which names no single peer. Use "
            "Netw.request_all to ask every peer."
        );
        Node *asker = Object::cast_to<Node>(p_callable.get_object());
        fault::raise(
            asker != nullptr ? NetwMultiplayer::of(asker) : nullptr,
            NETW_FAULT_BAD_VALUE,
            {String("Netw.request_id"),
             String("peer id 0"),
             String("a single peer id, or {Netw.request_all} to ask every "
                    "peer")},
            asker != nullptr ? uint64_t(gd::instance_id(asker)) : 0
        );
        return NetwPromise::refused(ERR_INVALID_PARAMETER, detail);
    }
    const CallSession session = call_session(p_callable);
    if (session.api == nullptr) {
        return NetwPromise::refused(session.code, session.detail);
    }
    return session.api->rpc_request_call(p_peer_id, p_callable, p_args);
}

Ref<NetwPromise> Netw::request(
    const Callable &p_callable,
    const Array &p_args
) {
    return request_id(SERVER_PEER, p_callable, p_args);
}

Ref<NetwGroupPromise> Netw::request_all(
    const Callable &p_callable,
    const Array &p_args
) {
    const CallSession session = call_session(p_callable);
    if (session.api == nullptr) {
        return NetwGroupPromise::refused(session.code, session.detail);
    }
    return session.api->rpc_request_call_group(p_callable, p_args);
}

Ref<NetwPromise> Netw::request_controller(
    const Callable &p_callable,
    const Array &p_args
) {
    const CallSession session = call_session(p_callable);
    if (session.api == nullptr) {
        return NetwPromise::refused(session.code, session.detail);
    }
    return session.api->rpc_request_controller(p_callable, p_args);
}

void Netw::sync_property(Node *p_node, const StringName &p_property) {
    if (p_node == nullptr) {
        return;
    }
    NetwMultiplayer *api = call_session(p_node, p_property).api;
    if (api == nullptr) {
        return;
    }
    ReplicationCore *plane = api->get_replication_plane();
    if (plane != nullptr) {
        plane->send_property(p_node, p_property);
    }
}

void Netw::emit_entity_signal(const Signal &p_signal, const Array &p_args) {
    Node *node = Object::cast_to<Node>(p_signal.get_object());
    if (node == nullptr) {
        return;
    }
    NetwMultiplayer *api = call_session(node, p_signal.get_name()).api;
    if (api == nullptr) {
        return;
    }
    ReplicationCore *plane = api->get_replication_plane();
    if (plane != nullptr) {
        plane->send_signal(node, p_signal.get_name(), p_args);
    }
}

Ref<NetwChannel> Netw::channel(Node *p_node, int64_t p_channel_id) {
    return NetwChannel::of(p_node, p_channel_id);
}

Ref<NetwDatabaseConfig> Netw::configure_database(
    Node *p_node,
    const StringName &p_name
) {
    if (p_node == nullptr) {
        fault::raise(
            nullptr,
            NETW_FAULT_EMPTY_ARGUMENT,
            {String("{Netw.configure_database}"), String(), String("node")}
        );
        return Ref<NetwDatabaseConfig>();
    }
    if (String(p_name).is_empty()) {
        fault::raise(
            nullptr,
            NETW_FAULT_EMPTY_ARGUMENT,
            {String("{Netw.configure_database}"),
             String(),
             String("database name")},
            uint64_t(gd::instance_id(p_node))
        );
        return Ref<NetwDatabaseConfig>();
    }
    Ref<NetwDatabaseConfig> draft;
    draft.instantiate();
    if (session_decl::declare_database(p_node, p_name, draft) != OK) {
        return Ref<NetwDatabaseConfig>();
    }
    return draft;
}

Node *Netw::spawn(const Callable &p_fn, const Array &p_args) {
    return spawn_player(Ref<NetwPlayer>(), p_fn, p_args);
}

Node *Netw::spawn_player(
    const Ref<NetwPlayer> &p_player,
    const Callable &p_fn,
    const Array &p_args
) {
    Node *host = Object::cast_to<Node>(p_fn.get_object());
    if (host == nullptr) {
        fault::raise(
            nullptr,
            NETW_FAULT_BAD_CALLABLE,
            {String("{Netw.spawn}"), String(), String("a method of a Node")}
        );
        return nullptr;
    }
    NetwMultiplayer *api = NetwMultiplayer::of(host);
    if (api == nullptr) {
        fault::raise(
            nullptr,
            NETW_FAULT_NOT_IN_SESSION,
            {String("{Netw.spawn}"), String(host->get_name())},
            uint64_t(gd::instance_id(host))
        );
        return nullptr;
    }
    ReplicationCore *plane = api->get_replication_plane();
    return plane != nullptr ? plane->spawn(p_fn, p_args, p_player) : nullptr;
}

Ref<NetwPromise> Netw::spawn_async(const Callable &p_fn, const Array &p_args) {
    Node *host = Object::cast_to<Node>(p_fn.get_object());
    NetwMultiplayer *api
        = host != nullptr ? NetwMultiplayer::of(host) : nullptr;
    ReplicationCore *plane
        = api != nullptr ? api->get_replication_plane() : nullptr;
    if (plane == nullptr) {
        const String refusal(
            "Netw.spawn_async needs a spawn function on a node inside a "
            "session. Pass a method of a node under a MultiplayerViewport."
        );
        fault::raise(
            api,
            NETW_FAULT_SPAWN_REFUSED,
            {String("The node"),
             String("a spawn function outside a session"),
             String("a spawn function on a node under a {MultiplayerViewport}")
            },
            host != nullptr ? uint64_t(gd::instance_id(host)) : 0
        );
        return NetwPromise::refused(ERR_UNCONFIGURED, refusal);
    }
    return plane->get_spawn_pipeline()
        ->spawn_async(p_fn, p_args, Ref<NetwPlayer>());
}

Error Netw::despawn(Node *p_node, const Ref<NetwDespawnOpts> &p_opts) {
    const Ref<NetwEntity> entity
        = NetwEntity::of(NetwMultiplayer::scene_inner_of(p_node));
    if (entity.is_null()) {
        fault::raise(
            nullptr,
            NETW_FAULT_NOT_ENTITY,
            {String("{Netw.despawn}"),
             p_node != nullptr ? String(p_node->get_name()) : String("<null>")},
            p_node != nullptr ? uint64_t(gd::instance_id(p_node)) : 0
        );
        return ERR_DOES_NOT_EXIST;
    }
    NetwMultiplayer *api = NetwEntity::session_core_for(p_node);
    if (api == nullptr) {
        fault::raise(
            nullptr,
            NETW_FAULT_NOT_IN_SESSION,
            {String("{Netw.despawn}"), String(p_node->get_name())},
            uint64_t(gd::instance_id(p_node))
        );
        return ERR_UNAVAILABLE;
    }
    return api->entity_despawn(entity->get_rid_handle(), p_opts);
}

Ref<NetwPromise> Netw::reparent(Node *p_node, Node *p_new_parent) {
    const Ref<NetwEntity> entity
        = NetwEntity::of(NetwMultiplayer::scene_inner_of(p_node));
    if (entity.is_null()) {
        fault::raise(
            nullptr,
            NETW_FAULT_NOT_ENTITY,
            {String("{Netw.reparent}"),
             p_node != nullptr ? String(p_node->get_name()) : String("<null>")},
            p_node != nullptr ? uint64_t(gd::instance_id(p_node)) : 0
        );
        return NetwPromise::rejected(
            ERR_DOES_NOT_EXIST,
            String("Netw.reparent: the node is no entity of any session")
        );
    }
    NetwMultiplayer *api = NetwEntity::session_core_for(p_node);
    if (api == nullptr) {
        fault::raise(
            nullptr,
            NETW_FAULT_NOT_IN_SESSION,
            {String("{Netw.reparent}"), String(p_node->get_name())},
            uint64_t(gd::instance_id(p_node))
        );
        return NetwPromise::rejected(
            ERR_UNAVAILABLE,
            String("Netw.reparent: no session governs the node")
        );
    }
    return api->entity_reparent(entity->get_rid_handle(), p_new_parent);
}

Ref<NetwAction> Netw::action(const Callable &p_authority) {
    Node *host = Object::cast_to<Node>(p_authority.get_object());
    if (host == nullptr) {
        fault::raise(
            nullptr,
            NETW_FAULT_BAD_CALLABLE,
            {String("{Netw.action}"), String(), String("a method of a Node")}
        );
        return Ref<NetwAction>();
    }
    NetwMultiplayer *api = NetwMultiplayer::of(host);
    if (api == nullptr) {
        fault::raise(
            nullptr,
            NETW_FAULT_NOT_IN_SESSION,
            {String("{Netw.action}"), String(host->get_name())},
            uint64_t(gd::instance_id(host))
        );
        return Ref<NetwAction>();
    }
    return api->lagcomp_action(p_authority);
}

Ref<DictionaryRecord> Netw::sample(
    const Ref<NetwEntity> &p_entity,
    int64_t p_tick
) {
    if (p_entity.is_null()) {
        fault::raise(
            nullptr,
            NETW_FAULT_EMPTY_ARGUMENT,
            {String("{Netw.sample}"), String(), String("entity")}
        );
        return Ref<DictionaryRecord>();
    }
    NetwMultiplayer *api = NetwEntity::session_core_for(p_entity->get_owner());
    if (api == nullptr) {
        Node *owner = p_entity->get_owner();
        fault::raise(
            nullptr,
            NETW_FAULT_NOT_IN_SESSION,
            {String("{Netw.sample}"),
             owner != nullptr ? String(owner->get_name()) : String("<null>")},
            owner != nullptr ? uint64_t(gd::instance_id(owner)) : 0
        );
        return Ref<DictionaryRecord>();
    }
    return api->lagcomp_sample_of(p_entity, p_tick);
}

Variant Netw::rewind(
    const TypedArray<NetwEntity> &p_entities,
    int64_t p_tick,
    const Callable &p_body
) {
    if (p_entities.is_empty()) {
        fault::raise(
            nullptr,
            NETW_FAULT_EMPTY_ARGUMENT,
            {String("{Netw.rewind}"), String(), String("entities")}
        );
        return Variant();
    }
    NetwMultiplayer *api = nullptr;
    for (int at = 0; at < p_entities.size() && api == nullptr; ++at) {
        const Ref<NetwEntity> entity = p_entities[at];
        if (entity.is_valid()) {
            api = NetwEntity::session_core_for(entity->get_owner());
        }
    }
    if (api == nullptr) {
        fault::raise(
            nullptr,
            NETW_FAULT_NOT_IN_SESSION,
            {String("{Netw.rewind}"), String("its entities")}
        );
        return Variant();
    }
    return api->lagcomp_rewind_of(p_entities, p_tick, p_body);
}

Ref<NetwSceneConfig> Netw::configure_scene(Node *p_node) {
    if (p_node == nullptr) {
        fault::raise(
            nullptr,
            NETW_FAULT_EMPTY_ARGUMENT,
            {String("{Netw.configure_scene}"),
             String(),
             String("root node of the scene")}
        );
        return Ref<NetwSceneConfig>();
    }
    const Ref<Script> script = script_of(p_node);
    if (script.is_null()) {
        fault::raise(
            nullptr,
            NETW_FAULT_BAD_SCRIPT,
            {String("{Netw.configure_scene}"),
             vformat("'%s'", String(p_node->get_name())),
             String("the script on the root of the scene")},
            uint64_t(gd::instance_id(p_node))
        );
        return Ref<NetwSceneConfig>();
    }

    SceneDecl decl = netw::script::model::get_scene_decl(script);
    if (!decl.declared) {
        decl.declared = true;
        netw::script::model::declare_scene(script, decl);
    }

    const Ref<NetwEntity> entity = NetwEntity::resolve(p_node);
    if (entity.is_valid()) {
        entity->set_declares_scene(true);
    }

    Ref<NetwSceneConfig> config;
    config.instantiate();
    config->declare_against(p_node, script);
    return config;
}

void Netw::_bind_methods() {
    ClassDB::bind_static_method(
        "Netw",
        D_METHOD("_native_api"),
        &Netw::native_api
    );
    ClassDB::bind_static_method("Netw", D_METHOD("of", "node"), &Netw::of);
    ClassDB::bind_static_method(
        "Netw",
        D_METHOD("session", "node"),
        &Netw::session
    );
    gd::bind_static_vararg(
        "Netw",
        D_METHOD("prepare_join", "node", "username"),
        &Netw::prepare_join
    );
    ClassDB::bind_static_method(
        "Netw",
        D_METHOD("scene", "node"),
        &Netw::scene
    );

    ClassDB::bind_static_method(
        "Netw",
        D_METHOD("configure_server_info", "provider"),
        &Netw::configure_server_info
    );
    ClassDB::bind_static_method(
        "Netw",
        D_METHOD("configure_join", "handler"),
        &Netw::configure_join
    );
    ClassDB::bind_static_method(
        "Netw",
        D_METHOD("configure_admission", "handler"),
        &Netw::configure_admission
    );
    ClassDB::bind_static_method(
        "Netw",
        D_METHOD("configure_transfer", "keep"),
        &Netw::configure_transfer
    );
    ClassDB::bind_static_method(
        "Netw",
        D_METHOD("configure_session", "node"),
        &Netw::configure_session
    );
    ClassDB::bind_static_method(
        "Netw",
        D_METHOD("configure_clock", "node"),
        &Netw::configure_clock
    );
    ClassDB::bind_static_method(
        "Netw",
        D_METHOD("configure_lagcomp", "node"),
        &Netw::configure_lagcomp
    );

    ClassDB::bind_static_method(
        "Netw",
        D_METHOD("configure_property", "node", "property", "declared_type"),
        &Netw::configure_property,
        DEFVAL(-1)
    );
    ClassDB::bind_static_method(
        "Netw",
        D_METHOD("configure_rpc", "callable"),
        &Netw::configure_rpc
    );
    ClassDB::bind_static_method(
        "Netw",
        D_METHOD("configure_signal", "sig"),
        &Netw::configure_signal
    );
    ClassDB::bind_static_method(
        "Netw",
        D_METHOD("configure_spawn", "callable"),
        &Netw::configure_spawn
    );
    ClassDB::bind_static_method(
        "Netw",
        D_METHOD("configure_entity", "node"),
        &Netw::configure_entity
    );
    ClassDB::bind_static_method(
        "Netw",
        D_METHOD("configure_despawn", "node"),
        &Netw::configure_despawn
    );
    ClassDB::bind_static_method(
        "Netw",
        D_METHOD("configure_persistence", "node"),
        &Netw::configure_persistence
    );
    ClassDB::bind_static_method(
        "Netw",
        D_METHOD("configure_schema", "name"),
        &Netw::configure_schema
    );

    gd::bind_static_vararg("Netw", D_METHOD("rpc", "callable"), &Netw::rpc);
    gd::bind_static_vararg(
        "Netw",
        D_METHOD("rpc_id", "peer_id", "callable"),
        &Netw::rpc_id
    );
    gd::bind_static_vararg(
        "Netw",
        D_METHOD("rpc_controller", "callable"),
        &Netw::rpc_controller
    );
    gd::bind_static_vararg(
        "Netw",
        D_METHOD("request", "callable"),
        &Netw::request
    );
    gd::bind_static_vararg(
        "Netw",
        D_METHOD("request_id", "peer_id", "callable"),
        &Netw::request_id
    );
    gd::bind_static_vararg(
        "Netw",
        D_METHOD("request_all", "callable"),
        &Netw::request_all
    );
    gd::bind_static_vararg(
        "Netw",
        D_METHOD("request_controller", "callable"),
        &Netw::request_controller
    );
    ClassDB::bind_static_method(
        "Netw",
        D_METHOD("sync_property", "node", "property"),
        &Netw::sync_property
    );
    gd::bind_static_vararg(
        "Netw",
        D_METHOD("emit_entity_signal", "sig"),
        &Netw::emit_entity_signal
    );
    ClassDB::bind_static_method(
        "Netw",
        D_METHOD("channel", "node", "channel_id"),
        &Netw::channel
    );
    ClassDB::bind_static_method(
        "Netw",
        D_METHOD("configure_database", "node", "name"),
        &Netw::configure_database
    );

    gd::bind_static_vararg("Netw", D_METHOD("spawn", "fn"), &Netw::spawn);
    gd::bind_static_vararg(
        "Netw",
        D_METHOD("spawn_player", "player", "fn"),
        &Netw::spawn_player
    );
    gd::bind_static_vararg(
        "Netw",
        D_METHOD("spawn_async", "fn"),
        &Netw::spawn_async
    );
    ClassDB::bind_static_method(
        "Netw",
        D_METHOD("despawn", "node", "opts"),
        &Netw::despawn,
        DEFVAL(Ref<NetwDespawnOpts>())
    );
    ClassDB::bind_static_method(
        "Netw",
        D_METHOD("reparent", "node", "new_parent"),
        &Netw::reparent
    );

    ClassDB::bind_static_method(
        "Netw",
        D_METHOD("action", "authority"),
        &Netw::action
    );
    ClassDB::bind_static_method(
        "Netw",
        D_METHOD("sample", "entity", "tick"),
        &Netw::sample
    );
    ClassDB::bind_static_method(
        "Netw",
        D_METHOD("rewind", "entities", "tick", "body"),
        &Netw::rewind
    );

    ClassDB::bind_static_method(
        "Netw",
        D_METHOD("configure_scene", "node"),
        &Netw::configure_scene
    );
}

} // namespace netw
