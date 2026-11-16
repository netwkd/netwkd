#include "support/netw_test.h"

#include "godot/node.hpp"
#include "netw/api/entity_record.hpp"
#include "netw/api/netw_multiplayer.hpp"

#if defined(NETW_TIER_HOSTED)

#include "support/loopback_rig.h"
#include "support/minted_script.h"

#include "godot/object.hpp"
#include "godot/script.hpp"
#include "netw/api/context.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/entity_options.hpp"
#include "netw/api/participant.hpp"
#include "netw/api/scene.hpp"
#include "netw/api/session.hpp"
#include "netw/api/session_config.hpp"
#include "netw/spawn/pipeline.hpp"

#endif

namespace TestNetwSceneDespawn {

using namespace godot;
using netw::NetwMultiplayer;

#if defined(NETW_TIER_HOSTED)

using netw::Netw;
using netw::NetwEntity;
using netw::NetwPlayer;
using netw::NetwScene;
using netw_test::LoopbackRig;

constexpr const char *HOST_ID = "sr_host";

constexpr const char *GATE_SOURCE = R"(extends Node

func admit(request: NetwJoinRequest) -> void:
	request.account = "test:" + request.username
	request.admit()
)";

constexpr const char *LEVEL_SOURCE = R"(extends Node2D

func _init() -> void:
	Netw.configure_scene(self)
)";

constexpr const char *HOST_SOURCE = R"(extends Node

static var level_script: Script

func _init() -> void:
	Netw.configure_entity(self)
	Netw.configure_spawn(make_level)
	Netw.configure_spawn(make_pawn)

func make_level(level_name: String) -> Node:
	var level: Node = level_script.new()
	level.name = level_name
	return level

func make_pawn(pawn_name: String) -> Node:
	var pawn := Node2D.new()
	pawn.name = pawn_name
	return pawn

func open(level_name: String, parent: Node) -> Node:
	var level: Node = Netw.spawn(make_level, level_name)
	parent.add_child(level)
	return level

func seat(level: Node, player: NetwPlayer, prefix: String) -> Node:
	var pawn: Node = Netw.spawn_player(player, make_pawn, prefix + player.username)
	Netw.scene(level).root.add_child(pawn)
	return pawn
)";

Script *host_script = nullptr;

Node *build_host(const Variant &p_name) {
    Node *made = Object::cast_to<Node>(host_script->call("new"));
    made->set_name(String(p_name));
    return made;
}

void collect_despawned(Node *p_node, LocalVector<ObjectID> &r_out) {
    for (int at = 0; at < p_node->get_child_count(); ++at) {
        Node *child = p_node->get_child(at);
        const Ref<NetwEntity> seen = NetwEntity::of(child);
        const bool despawning = seen.is_valid() && seen->get_owner() == child
            && seen->get_stage() == NetwEntity::STAGE_DESPAWNING;
        if (child->is_queued_for_deletion() || despawning) {
            r_out.push_back(netw::gd::instance_id(child));
        } else {
            collect_despawned(child, r_out);
        }
    }
}

void end_frame() {
    LocalVector<ObjectID> doomed;
    collect_despawned(netw::gd::scene_root(), doomed);
    for (const ObjectID id : doomed) {
        Node *node = Object::cast_to<Node>(netw::gd::instance_from_id(id));
        if (node != nullptr) {
            memdelete(node);
        }
    }
}

struct Stand {
    Ref<Script> gate = netw_test::minted_script(GATE_SOURCE);
    Ref<Script> host_kind = netw_test::minted_script(HOST_SOURCE);
    Ref<Script> level = netw_test::minted_script(LEVEL_SOURCE);
    LoopbackRig rig;
    Node *host = nullptr;

    explicit Stand(int p_clients, double p_window = 0.0) : rig(0) {
        REQUIRE(gate.is_valid());
        REQUIRE(host_kind.is_valid());
        REQUIRE(level.is_valid());
        host_script = host_kind.ptr();
        host_kind->set("level_script", level);
        if (p_window > 0.0) {
            Ref<netw::NetwSessionConfig> config;
            config.instantiate();
            config->return_window(p_window);
            NETW_REQUIRE_EQ(
                int(rig.server()->session_initialize(config)),
                int(OK)
            );
        }
        rig.mount();
        Node *admit = Object::cast_to<Node>(gate->call("new"));
        rig.branch(-1)->add_child(admit);
        REQUIRE(
            Netw::configure_admission(Callable(admit, StringName("admit")))
                .is_valid()
        );
        rig.join(-1, StringName("host"));
        const char *names[] = {"ada", "bob"};
        for (int at = 0; at < p_clients; ++at) {
            rig.arrive(StringName(names[at]));
        }
        rig.pump(8);
        Array args;
        args.push_back(String("Host"));
        Array types;
        types.push_back(int(Variant::STRING));
        const int route = rig.spawn_registered(
            StringName(HOST_ID),
            callable_mp_static(&build_host),
            args,
            types
        );
        host = rig.route_node(route);
        REQUIRE(host != nullptr);
    }

    ~Stand() {
        end_frame();
        host_script = nullptr;
    }

    NetwMultiplayer *at(int p_seat) const {
        return p_seat < 0 ? rig.server() : rig.client(p_seat);
    }

    Node *open(const char *p_name, Node *p_parent = nullptr) {
        Node *parent = p_parent != nullptr ? p_parent : rig.branch(-1);
        return Object::cast_to<Node>(
            host->call(StringName("open"), String(p_name), parent)
        );
    }

    Node *seat(Node *p_level, int p_seat, const char *p_prefix) {
        return Object::cast_to<Node>(host->call(
            StringName("seat"),
            p_level,
            rig.player(p_seat),
            String(p_prefix)
        ));
    }

    String scene_of(int p_seat) const {
        const Ref<NetwPlayer> me = at(p_seat)->player_local();
        const Ref<NetwScene> scene
            = me.is_valid() ? me->get_scene() : Ref<NetwScene>();
        return scene.is_valid() ? String(scene->get_label()) : String("none");
    }
};

int route_of(Node *p_node) {
    const Ref<NetwEntity> entity = NetwEntity::of(p_node);
    return entity.is_valid() ? int(entity->get_route()) : -1;
}

Node *parent_of(Node *p_node) {
    return p_node != nullptr ? p_node->get_parent() : nullptr;
}

int despawn(Node *p_node) {
    return int(Netw::despawn(p_node, Ref<netw::NetwDespawnOpts>()));
}

void settle(Stand &p_stand) {
    p_stand.rig.step_ticks(24);
    end_frame();
    p_stand.rig.pump(4);
}

TEST_CASE(
    "[Networked][Scene][SceneTree] SR13 a player who returns after its level "
    "moved lands in the moved level with its kept entity"
) {
    Stand stand(1, 60.0);
    Node *lobby = stand.open("Lobby");
    const int pawn_route = route_of(stand.seat(lobby, 0, "Pawn_"));
    stand.rig.pump(8);
    const int lobby_route = route_of(lobby);
    const int host_route = route_of(stand.host);
    stand.rig.drop_client(0);
    stand.rig.pump(8);

    lobby->reparent(stand.host);
    settle(stand);
    const int back = stand.rig.add_client(StringName("ada"));
    stand.rig.register_constructor(
        stand.rig.client(back),
        StringName(HOST_ID),
        callable_mp_static(&build_host),
        Array::make(int(Variant::STRING))
    );
    stand.rig.mount_late(back);
    settle(stand);

    Node *mirror = stand.rig.route_node(lobby_route, back);
    CHECK(mirror != nullptr);
    CHECK(parent_of(mirror) == stand.rig.route_node(host_route, back));
    Node *pawn = stand.rig.route_node(pawn_route, back);
    CHECK(pawn != nullptr);
    CHECK(parent_of(pawn) == mirror);
    CHECK(stand.scene_of(back) == "Lobby");
    const Ref<NetwPlayer> returned = stand.rig.player(back);
    CHECK(
        bool(returned.is_valid() && returned->get_scene() == Netw::scene(lobby))
    );
}

TEST_CASE(
    "[Networked][Scene][SceneTree] SR9 a level despawned at the call leaves "
    "a departed player's kept entity that detaches from it spawned on the "
    "server and on every client that held it"
) {
    Stand stand(2, 60.0);
    Node *lobby = stand.open("Lobby");
    Node *kept = stand.seat(lobby, 0, "Pawn_");
    stand.seat(lobby, 1, "Pawn_");
    NetwEntity::of(kept)->set_on_parent_despawn(
        NetwEntity::PARENT_DESPAWN_DETACH
    );
    stand.rig.pump(8);
    const int kept_route = route_of(kept);
    REQUIRE(stand.rig.route_node(kept_route, 1) != nullptr);
    stand.rig.drop_client(0);
    stand.rig.pump(8);
    REQUIRE(NetwEntity::of(kept)->get_peer_id() == 0);

    despawn(lobby);
    end_frame();
    stand.rig.pump(8);

    CHECK(stand.rig.route_node(kept_route) != nullptr);
    CHECK(stand.rig.spawn_plane()->spawned_record(kept_route) != nullptr);
    CHECK(stand.rig.route_node(kept_route, 1) != nullptr);
}

#endif

} // namespace TestNetwSceneDespawn
