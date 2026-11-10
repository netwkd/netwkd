#include "support/loopback_rig.h"
#include "support/netw_recorder.h"

#if defined(NETW_TIER_HOSTED)

#include "godot/callable.hpp"
#include "godot/node.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/session.hpp"

#include <godot_cpp/classes/node2d.hpp>

namespace TestLossSettlement {

using namespace godot;
using namespace netw_test;
using netw::NetwEntity;
using netw::NetwMultiplayer;

const char *BODY_ID = "settlement_body";

Node *build_body(const Variant &p_name) {
    Node2D *made = memnew(Node2D);
    made->set_name(String(p_name));
    return made;
}

Array named(const char *p_name) {
    Array out;
    out.push_back(String(p_name));
    return out;
}

Array one_type() {
    Array out;
    out.push_back(int(Variant::STRING));
    return out;
}

void teach_constructor(LoopbackRig &p_rig, NetwMultiplayer *p_api) {
    p_rig.register_constructor(
        p_api,
        StringName(BODY_ID),
        callable_mp_static(&build_body),
        one_type()
    );
}

int seat_body(LoopbackRig &p_rig, Node *p_parent, const char *p_name) {
    teach_constructor(p_rig, p_rig.server());
    for (int at = 0; at < p_rig.count(); ++at) {
        teach_constructor(p_rig, p_rig.client(at));
    }
    const RID entity = p_rig.server()->spawn_registered(
        StringName(BODY_ID),
        named(p_name),
        p_rig.player(0).ptr()
    );
    REQUIRE_MESSAGE(entity.is_valid(), "the spawn verb minted no entity");
    Node *node = p_rig.server()->entity_get_node(entity);
    REQUIRE_MESSAGE(node != nullptr, "the spawn verb built no node");
    p_parent->add_child(node);
    p_rig.pump(8);
    return int(p_rig.server()->entity_get_route(entity));
}

Ref<NetwEntity> entity_at(LoopbackRig &p_rig, int p_route, int p_client) {
    Node *node = p_rig.route_node(p_route, p_client);
    return node == nullptr ? Ref<NetwEntity>() : NetwEntity::of(node);
}

TEST_CASE(
    "[Networked][Session][SceneTree] L1 a server that leaves takes a "
    "client's session down"
) {
    LoopbackRig rig(1);
    rig.mount();
    REQUIRE(
        rig.client(0)->session_get_state()
        == NetwMultiplayer::SESSION_STATE_ONLINE
    );

    Recorder seen(rig.client(0)->get_session().ptr(), {"exiting"});
    rig.client(0)->session_relay_peer_disconnected(1);

    NETW_CHECK_EQ(seen.count("exiting"), 1);
    CHECK(
        rig.client(0)->session_get_state()
        == NetwMultiplayer::SESSION_STATE_OFFLINE
    );
}

TEST_CASE(
    "[Networked][Session][SceneTree] L3 a controller that departs through "
    "the real transport signal reverts its entity exactly once, because "
    "settlement invokes the entity consequence directly rather than a "
    "per-entity subscription that a duplicated signal could fire twice"
) {
    LoopbackRig rig(2);
    rig.mount();
    Node *arena = rig.mirror_child("Arena");
    const int route = seat_body(rig, arena, "Steered");
    const Ref<NetwEntity> host = entity_at(rig, route, -1);
    REQUIRE(host.is_valid());
    const int64_t controller = rig.peer_id(1);
    host->set_controller(controller);
    rig.pump(8);
    NETW_CHECK_EQ(host->get_controller(), controller);

    Recorder reverted(host.ptr(), {"control_changed"});
    rig.drop_client(1);
    rig.pump(8);

    NETW_CHECK_EQ(reverted.count("control_changed"), 1);
    NETW_CHECK_EQ(host->get_controller(), int64_t(1));
}

TEST_CASE(
    "[Networked][Session][SceneTree] L4 a server's loss settles once even "
    "when the same departure reaches settlement twice through two different "
    "entries, because a duplicate report and a reentrant terminal leave "
    "describe one session ending and not two"
) {
    LoopbackRig rig(1);
    rig.mount();

    Recorder seen(rig.client(0)->get_session().ptr(), {"exiting"});
    rig.client(0)->session_relay_peer_disconnected(1);
    rig.client(0)->session_relay_peer_disconnected(1);
    rig.client(0)->peer_mark_unreachable(1);

    NETW_CHECK_EQ(seen.count("exiting"), 1);
    CHECK(
        rig.client(0)->session_get_state()
        == NetwMultiplayer::SESSION_STATE_OFFLINE
    );
}

NetwMultiplayer *&leaving_session() {
    static NetwMultiplayer *held = nullptr;
    return held;
}

void leave_on_departure(int64_t) {
    NetwMultiplayer *session = leaving_session();
    if (session != nullptr) {
        session->session_leave();
    }
}

TEST_CASE(
    "[Networked][Session][SceneTree] L5 a departure handler that starts "
    "leaving cannot cancel the server's terminal settlement, because the "
    "loss edge classifies the departure before any world consequence runs "
    "rather than re-reading the state the handler just moved"
) {
    LoopbackRig rig(1);
    rig.mount();
    REQUIRE(
        rig.client(0)->session_get_state()
        == NetwMultiplayer::SESSION_STATE_ONLINE
    );

    leaving_session() = rig.client(0);
    rig.client(0)->connect(
        StringName("peer_disconnected"),
        callable_mp_static(&leave_on_departure)
    );

    rig.client(0)->session_relay_peer_disconnected(1);
    leaving_session() = nullptr;

    CHECK(
        rig.client(0)->session_get_state()
        == NetwMultiplayer::SESSION_STATE_OFFLINE
    );
}

} // namespace TestLossSettlement

#endif
