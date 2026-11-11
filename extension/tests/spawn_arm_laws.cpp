#include "support/netw_test.h"

#if defined(NETW_TIER_HOSTED)

#include <cstdint>

#include "godot/node.hpp"
#include "godot/object.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/participant.hpp"
#include "support/loopback_rig.h"

namespace TestNetwSpawnArm {

using namespace godot;
using netw::NetwEntity;
using netw::NetwMultiplayer;
using netw::NetwPlayer;

Ref<NetwPlayer> seated(const Ref<NetwMultiplayer> &p_core, int64_t p_peer) {
    Ref<NetwPlayer> owner;
    owner.instantiate();
    p_core->player_adopt(p_peer, owner);
    p_core->player_admit(p_peer);
    return owner;
}

Ref<NetwPlayer> a_stranger(const Ref<NetwMultiplayer> &p_core, int64_t p_peer) {
    Ref<NetwPlayer> stranger;
    stranger.instantiate();
    stranger->bind_to(p_core.ptr(), p_peer);
    return stranger;
}

uint64_t built_crate = 0;

Node *build_crate(int64_t p_slot) {
    Node *crate = memnew(Node);
    crate->set_name(vformat("Crate%d", p_slot));
    built_crate = uint64_t(crate->get_instance_id());
    return crate;
}

TEST_CASE(
    "[Networked][Spawn][SceneTree] SM5 a spawn for a player this session "
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

    const Ref<NetwPlayer> stranger
        = a_stranger(Ref<NetwMultiplayer>(server), 9);
    CHECK_FALSE(server->player_holds(stranger));
    const RID refused
        = server->spawn_registered(StringName("crate"), slot, stranger.ptr());

    CHECK_FALSE(refused.is_valid());
    CHECK(netw::gd::object_of(ObjectID(built_crate)) == nullptr);

    const Ref<NetwPlayer> admitted = seated(Ref<NetwMultiplayer>(server), 9);
    const RID armed
        = server->spawn_registered(StringName("crate"), slot, admitted.ptr());
    REQUIRE(armed.is_valid());
    Node *crate = server->entity_get_node(armed);
    REQUIRE(crate != nullptr);
    NETW_CHECK_EQ(NetwEntity::of(crate)->get_peer_id(), int64_t(9));
    rig.branch(-1)->add_child(crate);
    rig.pump(2);
}

} // namespace TestNetwSpawnArm

#endif
