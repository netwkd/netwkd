#include "support/fault_log.h"
#include "support/mesh_stand.h"
#include "support/netw_test.h"

#include "godot/callable.hpp"
#include "netw/api/loopback.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/session/frames.hpp"

namespace TestNetwSessionServerPeer {

using namespace godot;
using netw::NetwMultiplayer;

Ref<NetwMultiplayer> a_session() {
    Ref<NetwMultiplayer> session;
    session.instantiate();
    return session;
}

TEST_CASE(
    "[Netw][Session][Hosted] L1 a plain peer 1 arrival resolves to "
    "server role with nobody reconfiguring anything"
) {
    Ref<NetwMultiplayer> session = a_session();

    session->session_peer_assigned(true, 1);

    CHECK(session->is_host());
    CHECK(session->session_get_role() == NetwMultiplayer::ROLE_LISTEN_SERVER);
}

TEST_CASE(
    "[Netw][Session][Hosted] L4 a connected edge without peer 1 stays "
    "connecting, because a client is online only once the server is admitted"
) {
    Ref<NetwMultiplayer> session = a_session();
    session->session_set_desired_role(NetwMultiplayer::ROLE_CLIENT);

    session->session_peer_assigned(true, 9);

    CHECK(
        session->session_get_stored_state()
        == NetwMultiplayer::SESSION_STATE_CONNECTING
    );
}

#if defined(NETW_TIER_HOSTED)

int serverless_mesh_warnings(const netw_test::FaultWatch &p_faults) {
    return int(p_faults.errors_containing("no link to peer 1"));
}

TEST_CASE(
    "[Netw][Session] M1 a mesh client linked only to peers other than 1 "
    "warns once after 300 polls and stays connecting"
) {
    netw_test::FaultWatch faults;
    netw_test::MeshStand mesh;
    mesh.seat_member(2);
    mesh.seat_member(3);
    mesh.wire(2, 3);

    mesh.pump(299);
    NETW_CHECK_EQ(serverless_mesh_warnings(faults), 0);

    mesh.pump(1);
    NETW_CHECK_EQ(serverless_mesh_warnings(faults), 2);

    mesh.pump(600);
    NETW_CHECK_EQ(serverless_mesh_warnings(faults), 2);
    for (const int member : {2, 3}) {
        NETW_CHECK_EQ(
            int(mesh.session_of(member)->session_get_stored_state()),
            int(NetwMultiplayer::SESSION_STATE_CONNECTING)
        );
    }
}

TEST_CASE(
    "[Netw][Session] M2 a mesh client linked to 1 goes online once 1 is "
    "admitted and never warns, even while 1 is still authenticating"
) {
    netw_test::FaultWatch faults;
    netw_test::MeshStand mesh;
    mesh.seat_server();
    mesh.seat_member(2);
    mesh.seat_member(3);
    mesh.silence(netw::SERVER_PEER);
    mesh.wire(2, 3);
    mesh.pump(10);
    NETW_CHECK_EQ(serverless_mesh_warnings(faults), 0);

    mesh.wire(1, 2);
    mesh.wire(1, 3);
    mesh.pump(600);
    NETW_CHECK_EQ(serverless_mesh_warnings(faults), 0);
    for (const int member : {2, 3}) {
        NETW_CHECK_EQ(
            int(mesh.session_of(member)->session_get_stored_state()),
            int(NetwMultiplayer::SESSION_STATE_CONNECTING)
        );
    }

    mesh.unsilence(netw::SERVER_PEER);
    mesh.pump(600);

    NETW_CHECK_EQ(serverless_mesh_warnings(faults), 0);
    for (const int member : {2, 3}) {
        NETW_CHECK_EQ(
            int(mesh.session_of(member)->session_get_stored_state()),
            int(NetwMultiplayer::SESSION_STATE_ONLINE)
        );
    }
}

#endif

PackedByteArray pong_payload(uint64_t p_origin, uint64_t p_tick) {
    netw::session::ClockPong pong;
    pong.origin = p_origin;
    pong.tick = p_tick;
    pong.phase = 128;
    return netw::session::frame_write(pong);
}

TEST_CASE(
    "[Netw][Session][Hosted] L5 the clock's pong evidence answers the "
    "server, so a pong from any other peer is refused while the same pong "
    "from peer 1 settles"
) {
    Ref<NetwMultiplayer> session = a_session();

    session->clock_receive_pong(pong_payload(0, 10), 7);
    CHECK_FALSE(session->clock_is_synchronized());

    session->clock_receive_pong(pong_payload(0, 10), 1);
    CHECK(session->clock_is_synchronized());
}

TEST_CASE(
    "[Netw][Session][Hosted] L8 a session with no role and no connection "
    "is still its own authority, because an offline game holds the authority "
    "Godot's own spawner assumes"
) {
    Ref<NetwMultiplayer> session = a_session();

    CHECK(session->is_host());
    NETW_CHECK_EQ(session->session_get_role(), NetwMultiplayer::ROLE_NONE);
}

PackedByteArray deny_payload(const StringName &p_key) {
    netw::session::DenyKey denied;
    denied.key = p_key;
    return netw::session::frame_write(denied);
}

TEST_CASE(
    "[Netw][Session][Hosted] L9 a lag compensation denial reverts the "
    "speculative effect only when it comes from the server that served the "
    "action, so another peer cannot revert a peer's prediction"
) {
    const StringName KEY = StringName("an-effect");
    Ref<NetwMultiplayer> session = a_session();

    session->lagcomp_effect_arm(KEY, Callable(), 30);
    NETW_CHECK_EQ(int(session->lagcomp_effect_pending(KEY)), 1);

    session->handle_deny(deny_payload(KEY), 7);
    NETW_CHECK_EQ(int(session->lagcomp_effect_pending(KEY)), 1);

    session->handle_deny(deny_payload(KEY), 1);
    NETW_CHECK_EQ(int(session->lagcomp_effect_pending(KEY)), 0);
}

} // namespace TestNetwSessionServerPeer
