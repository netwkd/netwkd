#include "support/netw_test.h"

#if defined(NETW_TIER_HOSTED)

#include "support/fault_log.h"
#include "support/loopback_rig.h"
#include "support/minted_script.h"
#include "support/netw_cells.h"
#include "support/stock_probes.h"
#include "support/stock_stand.h"
#include "support/value_flow_stand.h"

#include "godot/node.hpp"
#include "godot/object.hpp"
#include "netw/api/context.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/participant.hpp"

#include <godot_cpp/classes/node2d.hpp>

namespace TestSpawn {

using namespace godot;
using netw::Netw;
using netw::NetwEntity;
using netw::NetwMultiplayer;
using netw::NetwPlayer;
using netw_test::FaultWatch;
using netw_test::law_broken;
using netw_test::law_held;
using netw_test::LawVerdict;
using netw_test::LoopbackRig;

constexpr int TICKRATE = 30;
constexpr int PARK_EXPIRED = 170;
constexpr int SERVER = -1;
constexpr int CLIENT = 0;

constexpr const char *PLACED_ENTITY = R"(extends Node2D

func _init() -> void:
	Netw.configure_entity(self)
)";

constexpr const char *SPAWNER_SOURCE = R"(extends Node

signal go

var waits := false
var promised := false
var pending: NetwPromise
var late_id := 0

func _init() -> void:
	Netw.configure_entity(self)
	Netw.configure_spawn(make)

func make():
	if promised:
		return promise()
	if waits:
		await go
	return fresh()

func construct(_name: String):
	if promised:
		return promise()
	if waits:
		await go
	return fresh()

func promise() -> NetwPromise:
	pending = NetwPromise.new()
	return pending

func fresh() -> Node:
	var made := Node.new()
	late_id = made.get_instance_id()
	return made

func finish() -> void:
	if pending != null:
		pending.resolve(fresh())
	else:
		go.emit()

func spawn_one() -> Node:
	var made: Node = Netw.spawn(make)
	if made != null:
		get_parent().add_child(made)
	return made
)";

constexpr const char *FN_FIX
    = "Netw.spawn_async for a spawn function that awaits";
constexpr const char *REGISTRY_FIX
    = "a constructor that returns the node without waiting";
constexpr const char *WAITING_ID = "waiting";

enum Wait {
    NO_WAIT,
    COROUTINE,
    PROMISE,
    REGISTRY,
};

struct Stand {
    String label;
    int waiter = SERVER;
    Wait wait = NO_WAIT;
};

typedef netw_test::LawRowFor<Stand> Row;

Array one_string(const char *p_value) {
    Array out;
    out.push_back(String(p_value));
    return out;
}

Array string_type() {
    Array out;
    out.push_back(int(Variant::STRING));
    return out;
}

Node *build_named(const String &p_name) {
    Node *made = memnew(Node);
    made->set_name(p_name);
    return made;
}

Node *spawn_named(NetwMultiplayer *p_server, const char *p_name) {
    const RID spawned = p_server->spawn_registered(
        StringName("named"),
        one_string(p_name),
        nullptr
    );
    return p_server->entity_get_node(spawned);
}

LawVerdict late_into_tree(const Stand &) {
    FaultWatch faults;
    faults.expect_warning("*'Late'*[NW0022]*", 1);
    LoopbackRig rig(0);
    rig.mount();
    NetwMultiplayer *server = rig.server();
    rig.register_constructor(
        server,
        StringName("named"),
        callable_mp_static(&build_named),
        string_type()
    );

    Node *late = spawn_named(server, "Late");
    Node *kept = spawn_named(server, "Kept");
    if (late == nullptr || kept == nullptr) {
        return law_broken("the spawn built no node");
    }
    rig.branch(SERVER)->add_child(kept);
    rig.step_ticks(4);
    rig.branch(SERVER)->add_child(late);
    rig.step_ticks(2);
    return law_held();
}

bool part_declares = false;

Node *build_pawn_with_part(const Variant &) {
    Node *pawn = memnew(Node);
    pawn->set_name("Pawn");
    Netw::configure_entity(pawn);
    Node *physics = memnew(Node);
    physics->set_name("Physics");
    if (part_declares) {
        Netw::configure_entity(physics);
    }
    pawn->add_child(physics);
    physics->set_owner(pawn);
    return pawn;
}

LawVerdict part_reads_player(bool p_declares) {
    LoopbackRig rig(1);
    rig.mount();
    const Ref<NetwPlayer> player = rig.player(CLIENT);
    if (player.is_null()) {
        return law_broken("the client holds no player");
    }
    part_declares = p_declares;
    const int route = rig.spawn_registered(
        StringName("pawn_with_part"),
        callable_mp_static(&build_pawn_with_part),
        one_string("pawn"),
        string_type(),
        nullptr,
        Variant(player)
    );
    part_declares = false;

    for (int peer = SERVER; peer < rig.count(); ++peer) {
        Node *pawn = rig.route_node(route, peer);
        if (pawn == nullptr) {
            return law_broken("peer %d holds no pawn", peer);
        }
        const Ref<NetwEntity> entity
            = NetwEntity::of(pawn->get_node_or_null(NodePath("Physics")));
        if (entity.is_null() || entity != NetwEntity::of(pawn)) {
            return law_broken("peer %d's part is not the pawn's entity", peer);
        }
        const Ref<NetwPlayer> seen = entity->get_player();
        if (seen.is_null() || seen->get_peer_id() != rig.peer_id(CLIENT)) {
            return law_broken("peer %d's part reads the wrong player", peer);
        }
        if (entity->get_is_controlled_locally() != (peer == CLIENT)) {
            return law_broken("peer %d reads the wrong local control", peer);
        }
    }
    return law_held();
}

LawVerdict declaring_part(const Stand &) {
    return part_reads_player(true);
}

LawVerdict plain_part(const Stand &) {
    return part_reads_player(false);
}

Node *shared_probe(const String &p_name) {
    Node *root
        = netw_test::scripted_root(netw_test::gdsrc::STOCK_SYNC_PROBE, p_name);
    Vector<netw_test::SyncProp> props;
    netw_test::SyncProp row;
    row.path = NodePath(".:synced_value");
    row.mode = SceneReplicationConfig::REPLICATION_MODE_ALWAYS;
    props.push_back(row);
    netw_test::add_stock_sync(root, "Sync", props);
    return root;
}

void clocked(LoopbackRig &p_rig) {
    p_rig.mount();
    netw_test::flow_clocks(p_rig, TICKRATE);
}

LawVerdict late_client_node(const Stand &) {
    LoopbackRig rig(1);
    clocked(rig);
    Node *server_side = shared_probe("Shared");
    rig.branch(SERVER)->add_child(server_side);
    rig.step_ticks(8);

    Node *client_side = shared_probe("Shared");
    rig.branch(CLIENT)->add_child(client_side);
    rig.step_ticks(8);
    if (NetwEntity::of(client_side).is_null()) {
        return law_broken("the late node never bound");
    }

    server_side->set(StringName("synced_value"), 61);
    const int64_t seen = netw_test::pump_until_value(
        rig,
        client_side,
        StringName("synced_value"),
        61
    );
    if (seen != 61) {
        return law_broken("the client read %d", int(seen));
    }
    return law_held();
}

LawVerdict client_never_builds(const Stand &) {
    FaultWatch faults;
    faults.expect_warning("*'Shared'*[NW0026]*", 1);
    LoopbackRig rig(1);
    clocked(rig);
    rig.branch(SERVER)->add_child(shared_probe("Shared"));
    rig.step_ticks(PARK_EXPIRED);
    return law_held();
}

Node *placed_level(LoopbackRig &p_rig, int p_side, bool p_with_crate) {
    Node *level = netw_test::scripted_root(PLACED_ENTITY, "Level");
    Node2D *props = memnew(Node2D);
    props->set_name("Props");
    level->add_child(props);
    props->set_owner(level);
    if (p_with_crate) {
        Node *crate = netw_test::scripted_root(PLACED_ENTITY, "Crate");
        crate->set_scene_file_path("res://crate.tscn");
        props->add_child(crate);
        crate->set_owner(level);
    }
    p_rig.branch(p_side)->add_child(level);
    level->set_owner(p_rig.branch(p_side));
    return level;
}

LawVerdict placed_entity_missing(const Stand &) {
    FaultWatch faults;
    faults.expect_warning("*'Crate' at Level/*[NW0026]*", 1);
    LoopbackRig rig(1);
    clocked(rig);
    Node *hosted = placed_level(rig, SERVER, true);
    Node *seated = placed_level(rig, CLIENT, false);
    rig.step_ticks(PARK_EXPIRED);

    NetwMultiplayer *server = rig.server();
    NetwMultiplayer *client = rig.client(CLIENT);
    const int64_t route = server->entity_get_route(server->entity_of(hosted));
    if (route <= 0) {
        return law_broken("the server's level holds no route");
    }
    if (client->entity_get_route(client->entity_of(seated)) != route) {
        return law_broken("the client's level did not bind");
    }
    return law_held();
}

LawVerdict root_returns(const Stand &) {
    LoopbackRig rig(1);
    clocked(rig);
    Node *holder = memnew(Node);
    holder->set_name("Holder");
    Node *target = netw_test::scripted_root(
        netw_test::gdsrc::STOCK_SYNC_PROBE,
        "Target"
    );
    holder->add_child(target);

    MultiplayerSynchronizer *sync = memnew(MultiplayerSynchronizer);
    sync->set_name("Sync");
    sync->set_root_path(NodePath("../Target"));
    Ref<SceneReplicationConfig> config;
    config.instantiate();
    config->add_property(NodePath(".:synced_value"));
    config->property_set_replication_mode(
        NodePath(".:synced_value"),
        SceneReplicationConfig::REPLICATION_MODE_ALWAYS
    );
    sync->set_replication_config(config);
    holder->add_child(sync);

    rig.branch(SERVER)->add_child(holder);
    holder->remove_child(target);
    rig.step_ticks(4);
    holder->add_child(target);
    rig.step_ticks(8);
    if (NetwEntity::of(target).is_null()) {
        return law_broken("the returned root never became an entity");
    }
    return law_held();
}

Script *spawner_script = nullptr;

Node *build_spawner(const Variant &p_name) {
    Node *made = Object::cast_to<Node>(spawner_script->call("new"));
    made->set_name(String(p_name));
    return made;
}

void register_waiting(
    LoopbackRig &p_rig,
    NetwMultiplayer *p_api,
    Node *p_host
) {
    p_rig.register_constructor(
        p_api,
        StringName(WAITING_ID),
        Callable(p_host, StringName("construct")),
        string_type()
    );
}

Node *spawn_waiting(LoopbackRig &p_rig, Node *p_arena) {
    NetwMultiplayer *server = p_rig.server();
    const RID made = server->spawn_registered(
        StringName(WAITING_ID),
        one_string("Waited"),
        nullptr
    );
    Node *node = made.is_valid() ? server->entity_get_node(made) : nullptr;
    if (node != nullptr) {
        p_arena->add_child(node);
    }
    return node;
}

LawVerdict waiting_build(const Stand &p_stand) {
    const Ref<Script> spawner = netw_test::minted_script(SPAWNER_SOURCE);
    if (spawner.is_null()) {
        return law_broken("the spawner script did not compile");
    }
    spawner_script = spawner.ptr();
    LoopbackRig rig(1);
    clocked(rig);
    Node *arena = rig.mirror_child("Arena");
    const int host_route = rig.spawn_registered(
        StringName("spawn_wait_spawner"),
        callable_mp_static(&build_spawner),
        one_string("Spawner"),
        string_type(),
        arena
    );
    rig.step_ticks(8);
    spawner_script = nullptr;
    Node *host = rig.route_node(host_route, SERVER);
    Node *client_host = rig.route_node(host_route, CLIENT);
    if (host == nullptr || client_host == nullptr) {
        return law_broken("the spawner did not reach both peers");
    }
    register_waiting(rig, rig.server(), host);
    register_waiting(rig, rig.client(CLIENT), client_host);
    Node *waiter = p_stand.waiter == SERVER ? host : client_host;
    waiter->set(StringName("waits"), true);
    waiter->set(StringName("promised"), p_stand.wait == PROMISE);

    const FaultWatch faults;
    Node *spawned = p_stand.wait == REGISTRY
        ? spawn_waiting(rig, arena)
        : Object::cast_to<Node>(host->call(StringName("spawn_one")));
    rig.step_ticks(8);

    if ((spawned != nullptr) != (p_stand.waiter != SERVER)) {
        return law_broken("the server's spawn answered the wrong node");
    }
    if (spawned != nullptr) {
        NetwMultiplayer *server = rig.server();
        const int64_t route
            = server->entity_get_route(server->entity_of(spawned));
        if (rig.route_node(int(route), CLIENT) != nullptr) {
            return law_broken("the client kept a node it waited for");
        }
    }
    const String said = p_stand.wait == REGISTRY
        ? String("spawn constructor 'waiting'")
        : String("spawn function 'make'");
    const String fix = p_stand.wait == REGISTRY ? REGISTRY_FIX : FN_FIX;
    for (const String &message : {said, fix}) {
        if (faults.errors_containing(message) != 1) {
            return law_broken("the error did not say it waited");
        }
    }

    waiter->call(StringName("finish"));
    const ObjectID late
        = ObjectID(uint64_t(int64_t(waiter->get(StringName("late_id")))));
    if (!late.is_valid() || netw::gd::instance_from_id(late) != nullptr) {
        return law_broken("the node returned after the wait was kept");
    }
    return law_held();
}

const Row ROWS[] = {
    {"a spawned node still out of the tree a tick later, beside one added "
     "in time",
     "the late one is reported once and the other is not",
     &late_into_tree},
    {"a player's pawn whose part declares an entity in _init",
     "the part reads the pawn's entity and player on every peer, and only "
     "the player's peer controls it",
     &declaring_part},
    {"a player's pawn whose part declares nothing",
     "the part reads the pawn's entity and player on every peer, and only "
     "the player's peer controls it",
     &plain_part},
    {"a node with a stock synchronizer the client builds after the server",
     "the client's node binds when it arrives and reads the server's values",
     &late_client_node},
    {"a node with a stock synchronizer the client never builds",
     "the client reports it by name once the wait runs out",
     &client_never_builds},
    {"a placed entity the client's level lacks",
     "it is reported by its name and path, and the rest of the level binds",
     &placed_entity_missing},
    {"a synchronizer whose sibling root leaves the tree and returns",
     "the root becomes an entity when it returns",
     &root_returns},
};

const Row WAITING = {
    "a spawn function or constructor that waits",
    "the server refuses it and a client drops it, the error says it waited, "
    "and the node it returns later is freed",
    &waiting_build,
};

struct Waiter {
    const char *name;
    int waiter;
    Wait wait;
};

const Waiter WAITS[] = {
    {"a server coroutine", SERVER, COROUTINE},
    {"a client coroutine", CLIENT, COROUTINE},
    {"a server promise", SERVER, PROMISE},
    {"a client promise", CLIENT, PROMISE},
    {"a server constructor", SERVER, REGISTRY},
    {"a client constructor", CLIENT, REGISTRY},
};

TEST_CASE(
    "[Networked][Spawn][SceneTree] a spawned body and its parts read the "
    "right entity, and a slow, missing or waiting build is reported and "
    "strands nothing"
) {
    const Stand stand = {String("a loopback session")};
    for (const Row &row : ROWS) {
        NETW_CELL(row, stand);
        NETW_LAW_HOLDS(row, stand);
    }
    for (const Waiter &waiter : WAITS) {
        const Stand wait = {String(waiter.name), waiter.waiter, waiter.wait};
        NETW_CELL(WAITING, wait);
        NETW_LAW_HOLDS(WAITING, wait);
    }
}

} // namespace TestSpawn

#endif
