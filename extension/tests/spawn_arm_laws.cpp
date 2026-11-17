#include "support/netw_test.h"

#if defined(NETW_TIER_HOSTED)

#include <cstdint>

#include "godot/node.hpp"
#include "godot/object.hpp"
#include "netw/api/context.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/participant.hpp"
#include "support/fault_log.h"
#include "support/internals.h"
#include "support/loopback_rig.h"
#include "support/minted_script.h"

namespace TestNetwSpawnArm {

using namespace godot;
using netw::NetwEntity;
using netw::NetwMultiplayer;
using netw::NetwPlayer;
using netw_test::Internals;

constexpr const char *MAKER = R"(extends Node

func _init() -> void:
	Netw.configure_spawn(make)

func make() -> Node:
	return Node.new()
)";

uint64_t built_crate = 0;

Node *build_crate(int64_t p_slot) {
    Node *crate = memnew(Node);
    crate->set_name(vformat("Crate%d", p_slot));
    built_crate = uint64_t(crate->get_instance_id());
    return crate;
}

TEST_CASE(
    "[Netw][Spawn][SceneTree] SM5 a spawn for a player this session "
    "does not hold answers nothing and frees the node it built, and the same "
    "spawn for a seated player goes through"
) {
    netw_test::LoopbackRig rig(0);
    rig.mount();
    NetwMultiplayer *server = rig.server();
    Array arg_types;
    arg_types.push_back(int64_t(Variant::INT));
    Array slot;
    slot.push_back(int64_t(0));
    rig.register_constructor(
        server,
        StringName("crate"),
        callable_mp_static(&build_crate),
        arg_types
    );

    const Ref<NetwPlayer> admitted = Internals::seat_player(server, 9);
    const Ref<NetwPlayer> left = Internals::seat_player(server, 10);
    const int64_t gone = left->get_id();
    server->player_release_id(gone);
    netw_test::FaultWatch faults;
    faults.expect_error("*spawn_call*[NW0032]*", 1);
    faults.expect_error("*spawn_player*[NW0032]*", 1);
    const RID refused
        = server->spawn_registered(StringName("crate"), slot, gone);

    CHECK_FALSE(refused.is_valid());
    CHECK(netw::gd::object_of(ObjectID(built_crate)) == nullptr);

    Node *maker = netw_test::minted_node(MAKER);
    REQUIRE(maker != nullptr);
    rig.branch(-1)->add_child(maker);
    const Callable make(maker, "make");
    CHECK(netw::Netw::spawn_player(left, make, Array()) == nullptr);
    NETW_CHECK_EQ(faults.errors_logged(), int64_t(0));
    CHECK_FALSE(server->spawn_call(make, Array(), gone + 1).is_valid());
    Ref<NetwMultiplayer> freed;
    freed.instantiate();
    const Ref<NetwPlayer> orphan = Internals::seat_player(freed, 9);
    REQUIRE(orphan->get_id() == admitted->get_id());
    freed.unref();
    CHECK(netw::Netw::spawn_player(orphan, make, Array()) == nullptr);
    CHECK(server->player_get_entities(admitted->get_id()).is_empty());

    const RID armed = server->spawn_registered(
        StringName("crate"),
        slot,
        admitted->get_id()
    );
    REQUIRE(armed.is_valid());
    Node *crate = server->entity_get_node(armed);
    REQUIRE(crate != nullptr);
    NETW_CHECK_EQ(NetwEntity::of(crate)->get_peer_id(), int64_t(9));
    rig.branch(-1)->add_child(crate);
    rig.pump(2);
}

} // namespace TestNetwSpawnArm

#endif
