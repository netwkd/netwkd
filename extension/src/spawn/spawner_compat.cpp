#include "netw/spawn/spawner_compat.hpp"

#include "godot/multiplayer_spawner.hpp"
#include "godot/node.hpp"
#include "godot/packed_scene.hpp"
#include "godot/resource.hpp"
#include "godot/scene_tree.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/replication_core.hpp"
#include "netw/fault_book.hpp"
#include "netw/game_call.hpp"
#include "netw/log.hpp"
#include "netw/scene_loads.hpp"
#include "netw/subsystems.hpp"
#include "netw/sync_authoring.hpp"

using namespace godot;

namespace netw::spawn {

namespace {

const char *SIG_SESSION_ENTERED = "session_entered";
const char *SIG_SESSION_ENDED = "session_ended";
const char *SIG_NODE_ADDED = "node_added";
const char *SIG_READY = "ready";
const char *SIG_SPAWNED = "spawned";
const char *SIG_DESPAWNED = "despawned";
const char *KEY_CLEAR_SESSION = "spawner-compat-clear-session";

const GameCall SPAWNER_FUNCTION
    = {"The MultiplayerSpawner.spawn_function",
       "func(data: Variant) -> Node",
       1};

Variant duplicate_arg(const Variant &p_data) {
    if (p_data.get_type() == Variant::ARRAY) {
        return Array(p_data).duplicate(true);
    }
    if (p_data.get_type() == Variant::DICTIONARY) {
        return Dictionary(p_data).duplicate(true);
    }
    return p_data;
}

} // namespace

NetwMultiplayer *SpawnerCompat::core() const {
    return Object::cast_to<NetwMultiplayer>(gd::instance_from_id(core_id));
}

Node *SpawnerCompat::session_root() const {
    NetwMultiplayer *plane = core();
    return plane != nullptr ? plane->session_get_root() : nullptr;
}

void SpawnerCompat::set_core(Object *p_core) {
    NetwMultiplayer *plane = Object::cast_to<NetwMultiplayer>(p_core);
    core_id = gd::instance_id(plane);
}

void SpawnerCompat::set_spawn_seams(
    const Callable &p_booked_reader,
    const Callable &p_arm_consumed
) {
    booked_reader = p_booked_reader;
    arm_consumed = p_arm_consumed;
}

Error SpawnerCompat::consume(Node *p_node, Object *p_spawner) {
    MultiplayerSpawner *spawner
        = Object::cast_to<MultiplayerSpawner>(p_spawner);
    if (p_node == nullptr || spawner == nullptr) {
        return ERR_INVALID_PARAMETER;
    }
    if (!booked_reader.is_valid() || !arm_consumed.is_valid()) {
        return ERR_UNCONFIGURED;
    }
    register_spawner(spawner);
    if (bool(booked_reader.call(p_node))) {
        return OK;
    }
    NetwMultiplayer *plane = core();
    if (plane != nullptr && !plane->is_host()) {
        fault::raise(
            plane,
            NETW_FAULT_SERVER_ONLY,
            {String("Adding '") + String(p_node->get_name())
             + String("' under a MultiplayerSpawner")},
            uint64_t(gd::instance_id(p_node))
        );
        p_node->queue_free();
        return ERR_UNAUTHORIZED;
    }

    const int64_t nid = int64_t(gd::instance_id(p_node));
    int scene_index = -1;
    Variant data;
    HashMap<int64_t, Variant>::Iterator captured = custom_args.find(nid);
    if (captured) {
        data = captured->value;
        custom_args.remove(captured);
    } else {
        scene_index = SpawnerRoster::scene_index_for(spawner, p_node);
        if (scene_index < 0) {
            drops_uncaptured_custom += 1;
            fault::raise(
                plane,
                NETW_FAULT_UNSUPPORTED,
                {String("'") + String(p_node->get_name()) + String("'"),
                 String("a scene outside the spawnable scenes"),
                 String("the scene in the MultiplayerSpawner's spawnable "
                        "scenes, or MultiplayerSpawner.spawn")},
                uint64_t(gd::instance_id(p_node))
            );
            return OK;
        }
    }

    arm_consumed.call(p_node, spawner, scene_index, data);
    authoring::apply_spawner(p_node, spawner);
    return OK;
}

Error SpawnerCompat::consume_remove(Node *, Object *) {
    return OK;
}

void SpawnerCompat::register_spawner(Object *p_spawner) {
    MultiplayerSpawner *spawner
        = Object::cast_to<MultiplayerSpawner>(p_spawner);
    if (spawner == nullptr) {
        return;
    }
    roster.enrol(spawner);
    wrap_spawner(spawner);
}

void SpawnerCompat::wrap_when_ready(
    MultiplayerSpawner *p_spawner,
    NetwMultiplayer *p_plane
) {
    if (!p_spawner->is_inside_tree() || gd::node_ready(p_spawner)) {
        return;
    }
    Node *scene = p_spawner->get_owner();
    Node *waiter
        = scene != nullptr && !gd::node_ready(scene) ? scene : p_spawner;
    const Callable armed
        = callable_mp(p_plane, &NetwMultiplayer::spawner_wrap_when_ready)
              .bind(p_spawner);
    if (waiter->is_connected(SIG_READY, armed)) {
        return;
    }
    waiter->connect(SIG_READY, armed, Object::CONNECT_ONE_SHOT);
}

void SpawnerCompat::wrap_enrolled() {
    const LocalVector<Object *> live = roster.live();
    for (Object *spawner : live) {
        wrap_spawner(spawner);
    }
}

void SpawnerCompat::wrap_spawner(Object *p_spawner) {
    MultiplayerSpawner *spawner
        = Object::cast_to<MultiplayerSpawner>(p_spawner);
    if (spawner == nullptr) {
        return;
    }
    NetwMultiplayer *plane = core();
    if (plane == nullptr) {
        return;
    }
    const Callable current = spawner->get_spawn_function();
    if (!current.is_valid()) {
        wrap_when_ready(spawner, plane);
        return;
    }
    if (current.get_object() == plane) {
        return;
    }
    game_call_declared(current, SPAWNER_FUNCTION);
    originals[int64_t(gd::instance_id(spawner))] = current;
    spawner->set_spawn_function(
        callable_mp(plane, &NetwMultiplayer::spawner_wrapped_spawn)
            .bind(current)
    );
}

Node *SpawnerCompat::wrapped_spawn(
    const Variant &p_data,
    const Callable &p_original
) {
    if (!p_original.is_valid()) {
        return nullptr;
    }
    Array args;
    args.push_back(p_data);
    const Variant built = game_call(p_original, args, SPAWNER_FUNCTION);
    Node *node = Object::cast_to<Node>(gd::live_object(built));
    if (node != nullptr) {
        custom_args[int64_t(gd::instance_id(node))] = duplicate_arg(p_data);
    }
    return node;
}

Node *SpawnerCompat::instantiate(
    Object *p_spawner,
    int p_scene_index,
    const Variant &p_data
) {
    MultiplayerSpawner *spawner
        = Object::cast_to<MultiplayerSpawner>(p_spawner);
    if (spawner == nullptr) {
        return nullptr;
    }
    const int limit = int(spawner->get_spawn_limit());
    if (limit > 0 && roster.produced_count(spawner) >= limit) {
        fault::raise(
            nullptr,
            NETW_FAULT_LIMIT_EXCEEDED,
            {vformat("MultiplayerSpawner '%s'", String(spawner->get_name())),
             String("the spawn"),
             String("a higher spawn_limit or fewer spawned nodes"),
             0,
             int64_t(limit) + 1,
             int64_t(limit)},
            gd::instance_id(spawner)
        );
        return nullptr;
    }
    if (p_scene_index >= 0) {
        if (p_scene_index >= int(spawner->get_spawnable_scene_count())) {
            return nullptr;
        }
        const String path
            = gd::ensure_path(spawner->get_spawnable_scene(p_scene_index));
        Ref<PackedScene> packed = SceneLoads::shared().scene(path);
        if (packed.is_null()) {
            packed = gd::load_scene(path);
        }
        return packed.is_valid() ? packed->instantiate() : nullptr;
    }
    const HashMap<int64_t, Callable>::Iterator found
        = originals.find(int64_t(gd::instance_id(spawner)));
    const Callable original
        = found ? found->value : spawner->get_spawn_function();
    if (!original.is_valid()) {
        return nullptr;
    }
    Array args;
    args.push_back(p_data);
    const Variant built = game_call(original, args, SPAWNER_FUNCTION);
    return Object::cast_to<Node>(gd::live_object(built));
}

void SpawnerCompat::note_recv(int64_t p_route, Object *p_spawner) {
    if (Object::cast_to<MultiplayerSpawner>(p_spawner) != nullptr) {
        roster.note_producer(p_route, p_spawner);
    }
}

void SpawnerCompat::emit_spawned(Object *p_spawner, Node *p_node) {
    MultiplayerSpawner *spawner
        = Object::cast_to<MultiplayerSpawner>(p_spawner);
    if (spawner != nullptr && p_node != nullptr) {
        spawner->emit_signal(SIG_SPAWNED, p_node);
    }
}

void SpawnerCompat::emit_despawned(int64_t p_route, Node *p_node) {
    MultiplayerSpawner *spawner
        = Object::cast_to<MultiplayerSpawner>(roster.take_producer(p_route));
    if (spawner != nullptr && p_node != nullptr) {
        spawner->emit_signal(SIG_DESPAWNED, p_node);
    }
}

void SpawnerCompat::on_session_entered() {
    if (Node *root = session_root()) {
        const TypedArray<Node> found
            = root->find_children("*", "MultiplayerSpawner", true, false);
        for (int at = 0; at < found.size(); ++at) {
            const Variant entry = found[at];
            register_spawner(gd::live_object(entry));
        }
    }
    SceneTree *tree = gd::scene_tree();
    if (tree == nullptr) {
        return;
    }
    NetwMultiplayer *plane = core();
    if (plane == nullptr) {
        return;
    }
    const Callable watcher
        = callable_mp(plane, &NetwMultiplayer::spawner_node_added);
    if (!tree->is_connected(SIG_NODE_ADDED, watcher)) {
        tree->connect(SIG_NODE_ADDED, watcher);
    }
}

void SpawnerCompat::on_node_added(Node *p_node) {
    if (Object::cast_to<MultiplayerSpawner>(p_node) == nullptr) {
        return;
    }
    Node *root = session_root();
    if (root != nullptr && root->is_ancestor_of(p_node)) {
        register_spawner(p_node);
    }
}

void SpawnerCompat::on_session_ended() {
    NetwMultiplayer *plane = core();
    if (plane == nullptr) {
        return;
    }
    SceneTree *tree = gd::scene_tree();
    const Callable watcher
        = callable_mp(plane, &NetwMultiplayer::spawner_node_added);
    if (tree != nullptr && tree->is_connected(SIG_NODE_ADDED, watcher)) {
        tree->disconnect(SIG_NODE_ADDED, watcher);
    }
    plane->session_defer(
        callable_mp(plane, &NetwMultiplayer::spawner_clear_session_state),
        StringName(KEY_CLEAR_SESSION)
    );
}

void SpawnerCompat::clear_session_state() {
    for (const KeyValue<int64_t, Callable> &row : originals) {
        MultiplayerSpawner *spawner = Object::cast_to<MultiplayerSpawner>(
            gd::instance_from_id(ObjectID(uint64_t(row.key)))
        );
        if (spawner != nullptr) {
            spawner->set_spawn_function(row.value);
        }
    }
    custom_args.clear();
    originals.clear();
    roster.clear();
}

Dictionary SpawnerCompat::counters() const {
    Dictionary out;
    out[StringName("drops_uncaptured_custom")] = drops_uncaptured_custom;
    return out;
}

} // namespace netw::spawn
