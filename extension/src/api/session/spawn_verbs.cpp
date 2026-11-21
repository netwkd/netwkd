#include "netw/api/netw_multiplayer.hpp"

#include "godot/node.hpp"
#include "godot/object.hpp"
#include "godot/script.hpp"
#include "godot/utility.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/member_config.hpp"
#include "netw/api/promise.hpp"
#include "netw/colors.hpp"
#include "netw/entity/stage.hpp"
#include "netw/fault_book.hpp"
#include "netw/game_call.hpp"
#include "netw/lifecycle/rule.hpp"
#include "netw/log.hpp"
#include "netw/profile.hpp"
#include "netw/script/model.hpp"
#include "netw/spawn/call.hpp"
#include "netw/spawn/pipeline.hpp"
#include "netw/spawn/record.hpp"
#include "netw/stage.hpp"
#include "netw/subsystems.hpp"

using namespace godot;

namespace netw {

namespace spawn {

void SpawnRefusal::refuse(Error p_code, const String &p_detail) {
    code = p_code;
    detail = p_detail;
}

Ref<NetwPromise> SpawnRefusal::promise() const {
    return NetwPromise::refused(code, detail);
}

void SpawnRefusal::settle(const Ref<NetwPromise> &p_promise) const {
    NetwMultiplayer::promise_refuse(p_promise, code, detail);
}

} // namespace spawn

void NetwMultiplayer::spawn_write_owner(
    Node *p_node,
    const Ref<NetwPlayer> &p_owner
) {
    if (p_owner.is_null() || p_node == nullptr || p_node->is_inside_tree()) {
        return;
    }
    const Ref<NetwEntity> entity = NetwEntity::ensure(p_node);
    if (entity.is_valid()
        && entity->get_stage() == int64_t(netw::entity::Stage::UNBOUND)) {
        entity->set_player(p_owner);
    }
}

bool NetwMultiplayer::spawn_admitted(Node *p_node, const char *p_verb) {
    const Ref<NetwEntity> entity = NetwEntity::of(p_node);
    const bool represents = entity.is_valid() && entity->has_unbound_player();
    const Ref<NetwPlayer> represented
        = represents ? entity->unbound_player() : Ref<NetwPlayer>();
    const bool held = entity.is_valid();
    lifecycle::Facts facts = lifecycle_local_facts(
        held ? entity->get_record() : nullptr,
        held ? entity->get_owner() : nullptr,
        held ? entity->synchronizers() : TypedArray<MultiplayerSynchronizer>(),
        lifecycle::Kind::SPAWN
    );
    facts.player_bound = facts.player_bound || represents;
    String refusal;
    if (!lifecycle_judge_verb(facts, String(p_verb), p_node, refusal)
             .admitted()) {
        return false;
    }
    if (represents && !player_holds(represented)) {
        NETW_INFO(
            sys::SPAWN,
            "Nothing was spawned, because player '%s' left the session.",
            represented.is_valid() ? String(represented->get_username())
                                   : String()
        );
        return false;
    }
    if (facts.peer_is_server || liveness_lease_remaining() > 0) {
        return true;
    }
    fault::raise(
        this,
        NETW_FAULT_SPAWN_REFUSED,
        {p_node != nullptr ? String("'") + String(p_node->get_name())
                    + String("'")
                           : String("<null>"),
         String("using up all the spawns the session authority granted this "
                "peer"),
         String("it again once the session authority admits more of its "
                "spawns")},
        p_node != nullptr ? uint64_t(gd::instance_id(p_node)) : 0
    );
    return false;
}

Node *NetwMultiplayer::spawn_verb_host(
    const Callable &p_fn,
    const Array &p_args,
    const char *p_verb,
    spawn::SpawnRefusal &r_refusal
) {
    Node *host = Object::cast_to<Node>(p_fn.get_object());
    if (host == nullptr) {
        fault::raise(
            this,
            NETW_FAULT_BAD_CALLABLE,
            {String(p_verb),
             String(),
             String("a method of a node that exists on every peer")}
        );
        r_refusal.refuse(
            ERR_UNCONFIGURED,
            vformat(
                "%s needs a method of a Node. Pass a method of a node that "
                "exists on every peer.",
                String(p_verb)
            )
        );
        return nullptr;
    }
    const Ref<Script> script = host->get_script();
    const StringName method = p_fn.get_method();
    if (netw::script::model::get_spawn_config(script, method).is_null()) {
        fault::raise(
            this,
            NETW_FAULT_SPAWN_RETURN,
            {String("spawn function '") + String(method) + String("'"),
             String("no registration"),
             String("{Netw.configure_spawn} on it before calling ")
                 + String(p_verb)},
            gd::instance_id(host)
        );
        r_refusal.refuse(
            ERR_UNCONFIGURED,
            vformat(
                "Spawn function '%s' is not registered. Register it with "
                "Netw.configure_spawn before calling %s.",
                method,
                String(p_verb)
            )
        );
        return nullptr;
    }
    if (!netw::script::model::validate_argument_count(
            script,
            method,
            p_args.size()
        )) {
        fault::raise(
            this,
            NETW_FAULT_SPAWN_RETURN,
            {String("spawn function '") + String(method) + String("'"),
             String("the wrong number of arguments"),
             String("the arguments it declares")},
            gd::instance_id(host)
        );
        r_refusal.refuse(
            ERR_INVALID_PARAMETER,
            vformat(
                "%s was given the wrong number of arguments for spawn "
                "function '%s'.",
                String(p_verb),
                method
            )
        );
        return nullptr;
    }
    return host;
}

Node *NetwMultiplayer::spawn_verb_finish(
    const Variant &p_built,
    const spawn::SpawnCall &p_call,
    spawn::SpawnRefusal &r_refusal
) {
    Node *node = Object::cast_to<Node>(gd::live_object(p_built));
    if (node == nullptr || node->is_inside_tree()) {
        fault::raise(
            this,
            NETW_FAULT_SPAWN_RETURN,
            {String("spawn function '") + String(p_call.method) + String("'"),
             String("no returned Node outside the tree"),
             String("a function that returns the new node without adding it")},
            p_call.host
        );
        r_refusal.refuse(
            ERR_INVALID_PARAMETER,
            vformat(
                "Spawn function '%s' did not return a Node outside the tree. "
                "Return the new node without adding it.",
                p_call.method
            )
        );
        return nullptr;
    }
    const Ref<NetwEntity> existing = NetwEntity::of(node);
    if (existing.is_valid() && existing->get_route() != 0) {
        fault::raise(
            this,
            NETW_FAULT_SPAWN_RETURN,
            {String("spawn function '") + String(p_call.method) + String("'"),
             String("a node that is already replicated"),
             String("a function that returns a new node")},
            p_call.host
        );
        r_refusal.refuse(
            ERR_INVALID_PARAMETER,
            vformat(
                "Spawn function '%s' returned a node that is already "
                "replicated. Return a new node.",
                p_call.method
            )
        );
        return nullptr;
    }
    bool admitted = false;
    {
        NETW_ZONE_NC("Spawn admit", colors::LIVENESS);
        NetwEntity::settle_parts(node, true);
        spawn_write_owner(node, p_call.owner);
        admitted = spawn_admitted(node, p_call.verb);
    }
    if (!admitted) {
        memdelete(node);
        r_refusal.refuse(
            ERR_UNAUTHORIZED,
            vformat("The session refused spawn function '%s'.", p_call.method)
        );
        return nullptr;
    }

    Node *host = Object::cast_to<Node>(gd::instance_from_id(p_call.host));
    spawn::Record record;
    record.set_recipe(spawn::Book::RECIPE_FN);
    record.bind_fn_host(host);
    record.set_fn_method(p_call.method);
    record.set_fn_args(p_call.args);
    record.set_fn_waits(p_call.waits);
    bool armed = false;
    {
        NETW_ZONE_NC("Spawn arm authoritative", colors::LIVENESS);
        armed = spawn_arm(&record, node).is_valid();
    }
    if (!armed) {
        memdelete(node);
        r_refusal.refuse(
            ERR_UNAVAILABLE,
            vformat("Spawn function '%s' could not be armed.", p_call.method)
        );
        return nullptr;
    }
    {
        NETW_ZONE_NC("Spawn wrap world", colors::SCENE);
        scene_wrap_world(node);
    }
    return node;
}

Node *NetwMultiplayer::spawn_verb(
    const Callable &p_fn,
    const Array &p_args,
    const Ref<NetwPlayer> &p_owner
) {
    NETW_STAGE(SPAWN, "Spawn construct", colors::LIVENESS);
    spawn::SpawnRefusal refusal;
    Node *host = spawn_verb_host(p_fn, p_args, "Netw.spawn", refusal);
    if (host == nullptr) {
        return nullptr;
    }
    const StringName method = p_fn.get_method();

    Variant built;
    bool called = false;
    {
        NETW_ZONE_NC("Spawn call constructor", colors::LIVENESS);
        built = game_call(p_fn, p_args, game_calls::SPAWN_FUNCTION, &called);
    }
    if (!called) {
        return nullptr;
    }
    if (is_waiting(built)) {
        free_when_returned(built);
        fault::raise(
            this,
            NETW_FAULT_SPAWN_RETURN,
            {String("spawn function '") + String(method) + String("'"),
             String("an await"),
             String("{Netw.spawn_async} for a spawn function that awaits")},
            uint64_t(gd::instance_id(host))
        );
        return nullptr;
    }
    const spawn::SpawnCall
        call{gd::instance_id(host), method, p_args, p_owner, "Netw.spawn", 0};
    return spawn_verb_finish(built, call, refusal);
}

Ref<NetwPromise> NetwMultiplayer::spawn_verb_async(
    const Callable &p_fn,
    const Array &p_args,
    const Ref<NetwPlayer> &p_owner
) {
    NETW_STAGE(SPAWN, "Spawn construct async", colors::LIVENESS);
    spawn::SpawnRefusal refusal;
    if (!is_host()) {
        fault::raise(
            this,
            NETW_FAULT_SPAWN_REFUSED,
            {String("The node"),
             String("a call from a client"),
             String("{Netw.spawn} for a node the client spawns itself")}
        );
        refusal.refuse(
            ERR_UNAUTHORIZED,
            String(
                "Netw.spawn_async runs on the server. A client spawns its own "
                "node with Netw.spawn."
            )
        );
        return refusal.promise();
    }
    Node *host = spawn_verb_host(p_fn, p_args, "Netw.spawn_async", refusal);
    if (host == nullptr) {
        return refusal.promise();
    }
    const spawn::SpawnCall call{
        gd::instance_id(host),
        p_fn.get_method(),
        p_args,
        p_owner,
        "Netw.spawn_async",
        spawn_plane()->park_now(),
        true
    };

    Variant built;
    bool called = false;
    {
        NETW_ZONE_NC("Spawn call constructor", colors::LIVENESS);
        built = game_call(p_fn, p_args, game_calls::SPAWN_FUNCTION, &called);
    }
    if (!called) {
        refusal.refuse(
            ERR_INVALID_PARAMETER,
            vformat("Spawn function '%s' could not be called.", call.method)
        );
        return refusal.promise();
    }
    if (!is_waiting(built)) {
        Node *node = spawn_verb_finish(built, call, refusal);
        return node != nullptr
            ? NetwPromise::resolved(scene_outer_of(node))
            : refusal.promise();
    }

    spawn_pending_serial += 1;
    const int64_t serial = spawn_pending_serial;
    spawn::PendingSpawn &row = spawn_pending[serial];
    row.call = call;
    row.waiting = built;
    row.promise.instantiate();
    const uint64_t plane_id = uint64_t(gd::instance_id(this));
    Object *waiting = gd::live_object(built);
    if (NetwPromise *promised = Object::cast_to<NetwPromise>(waiting)) {
        promised->then(
            callable_mp_static(&NetwMultiplayer::spawn_async_returned)
                .bind(built, plane_id, serial)
        );
        promised->catch_error(
            callable_mp_static(&NetwMultiplayer::spawn_async_failed)
                .bind(built, plane_id, serial)
        );
    } else {
        waiting->connect(
            StringName("completed"),
            callable_mp_static(&NetwMultiplayer::spawn_async_returned)
                .bind(built, plane_id, serial),
            Object::CONNECT_ONE_SHOT
        );
    }
    return row.promise;
}

bool NetwMultiplayer::spawn_pending_returned(
    const Variant &p_value,
    int64_t p_serial
) {
    const HashMap<int64_t, spawn::PendingSpawn>::Iterator found
        = spawn_pending.find(p_serial);
    if (!found) {
        return false;
    }
    const spawn::PendingSpawn row = found->value;
    spawn_pending.remove(found);
    if (gd::instance_from_id(row.call.host) == nullptr) {
        row.promise->reject(
            ERR_UNAVAILABLE,
            vformat(
                "The node holding spawn function '%s' was freed before it "
                "returned.",
                row.call.method
            )
        );
        return false;
    }
    spawn::SpawnRefusal refusal;
    Node *node = spawn_verb_finish(p_value, row.call, refusal);
    if (node == nullptr) {
        refusal.settle(row.promise);
        return true;
    }
    row.promise->resolve(scene_outer_of(node));
    return true;
}

void NetwMultiplayer::spawn_pending_failed(
    int64_t p_code,
    const String &p_detail,
    int64_t p_serial
) {
    const HashMap<int64_t, spawn::PendingSpawn>::Iterator found
        = spawn_pending.find(p_serial);
    if (!found) {
        return;
    }
    const Ref<NetwPromise> promise = found->value.promise;
    spawn_pending.remove(found);
    promise->reject(Error(p_code), p_detail);
}

void NetwMultiplayer::spawn_fail_pending(const String &p_detail) {
    if (spawn_pending.is_empty()) {
        return;
    }
    LocalVector<Ref<NetwPromise>> failed;
    for (const KeyValue<int64_t, spawn::PendingSpawn> &row : spawn_pending) {
        failed.push_back(row.value.promise);
    }
    spawn_pending.clear();
    for (const Ref<NetwPromise> &promise : failed) {
        promise->reject(ERR_UNAVAILABLE, p_detail);
    }
}

void NetwMultiplayer::spawn_sweep_pending() {
    if (spawn_pending.is_empty()) {
        return;
    }
    spawn::Pipeline *pipeline = spawn_plane();
    LocalVector<int64_t> orphaned;
    for (KeyValue<int64_t, spawn::PendingSpawn> &row : spawn_pending) {
        spawn::PendingSpawn &pending = row.value;
        if (gd::instance_from_id(pending.call.host) == nullptr) {
            orphaned.push_back(row.key);
            continue;
        }
        pipeline->warn_slow_build(
            pending.call.method,
            pending.call.started_at,
            uint64_t(pending.call.host)
        );
    }
    for (const int64_t serial : orphaned) {
        const Ref<NetwPromise> promise = spawn_pending[serial].promise;
        const StringName method = spawn_pending[serial].call.method;
        spawn_pending.erase(serial);
        promise->reject(
            ERR_UNAVAILABLE,
            vformat(
                "The node holding spawn function '%s' was freed before it "
                "returned.",
                method
            )
        );
    }
}

void NetwMultiplayer::spawn_constructor_register(
    const StringName &p_id,
    const Callable &p_fn,
    const Array &p_arg_types,
    const Array &p_quantizers
) {
    if (!p_arg_types.is_empty()) {
        GameCall site = game_calls::SPAWN_CONSTRUCTOR;
        site.argument_count = p_arg_types.size();
        game_call_declared(p_fn, site);
    }
    spawn_constructors[p_id] = p_fn;
    if (p_arg_types.is_empty() && p_quantizers.is_empty()) {
        spawn_constructor_schemas.erase(p_id);
        return;
    }
    Array schema;
    schema.push_back(p_quantizers);
    schema.push_back(p_arg_types);
    spawn_constructor_schemas[p_id] = schema;
}

Callable NetwMultiplayer::spawn_constructor_of(const StringName &p_id) const {
    const HashMap<StringName, Callable>::ConstIterator found
        = spawn_constructors.find(p_id);
    return found ? found->value : Callable();
}

bool NetwMultiplayer::spawn_constructor_known(const StringName &p_id) const {
    return spawn_constructor_of(p_id).is_valid();
}

Variant NetwMultiplayer::spawn_registry_schema(const StringName &p_id) const {
    const HashMap<StringName, Array>::ConstIterator found
        = spawn_constructor_schemas.find(p_id);
    if (found) {
        return found->value;
    }
    const Callable fn = spawn_constructor_of(p_id);
    Object *host = fn.get_object();
    Ref<Script> script;
    if (host != nullptr) {
        script = host->get_script();
    }
    return spawn_script_schema(script, fn.get_method());
}

Variant NetwMultiplayer::spawn_script_schema(
    const Ref<Script> &p_script,
    const StringName &p_method
) {
    const Ref<NetwMemberConfig> cfg
        = netw::script::model::get_spawn_config(p_script, p_method);
    if (cfg.is_null()) {
        return Variant();
    }
    Array schema;
    schema.push_back(cfg->get_quantizers());
    schema.push_back(
        netw::script::model::get_method_arg_types(p_script, p_method)
    );
    return schema;
}

Node *NetwMultiplayer::spawn_verb_registered(
    const StringName &p_id,
    const Array &p_args,
    const Ref<NetwPlayer> &p_owner
) {
    const Callable fn = spawn_constructor_of(p_id);
    if (!fn.is_valid()) {
        fault::raise(
            this,
            NETW_FAULT_SPAWN_REFUSED,
            {String("A node from spawn constructor '") + String(p_id)
                 + String("'"),
             String("an unregistered spawn constructor"),
             String("a spawn constructor registered with "
                    "{NetwMultiplayer.spawn_register_constructor}")}
        );
        return nullptr;
    }
    bool called = false;
    const Variant built
        = game_call(fn, p_args, game_calls::SPAWN_CONSTRUCTOR, &called);
    if (!called) {
        return nullptr;
    }
    if (is_waiting(built)) {
        free_when_returned(built);
        fault::raise(
            this,
            NETW_FAULT_SPAWN_RETURN,
            {String("spawn constructor '") + String(p_id) + String("'"),
             String("an await"),
             String("a constructor that returns the node without waiting")}
        );
        return nullptr;
    }
    Node *node = Object::cast_to<Node>(gd::live_object(built));
    if (node == nullptr || node->is_inside_tree()) {
        fault::raise(
            this,
            NETW_FAULT_SPAWN_RETURN,
            {String("spawn constructor '") + String(p_id) + String("'"),
             String("no returned Node outside the tree"),
             String("a constructor that returns the new node outside the "
                    "tree")}
        );
        return nullptr;
    }
    NetwEntity::settle_parts(node, true);
    spawn_write_owner(node, p_owner);
    if (!spawn_admitted(node, "spawn_registered")) {
        memdelete(node);
        return nullptr;
    }
    spawn::Record record;
    record.set_recipe(spawn::Book::RECIPE_FN_REGISTRY);
    record.set_fn_registry_id(p_id);
    record.set_fn_args(p_args);
    if (spawn_arm(&record, node).is_null()) {
        memdelete(node);
        return nullptr;
    }
    scene_wrap_world(node);
    return node;
}

} // namespace netw
