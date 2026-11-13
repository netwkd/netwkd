#include "support/netw_test.h"

#include "netw/api/connection.hpp"
#include "netw/api/context.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/participant.hpp"
#include "netw/api/server_info.hpp"
#include "netw/api/session_config.hpp"
#include "support/loopback_rig.h"

#if defined(NETW_TIER_HOSTED)

namespace TestNetwJoinPrivacyLaws {

using namespace godot;
using netw::NetwMultiplayer;
using netw_test::LoopbackRig;

TEST_CASE(
    "[Networked][Session] JV2 one membership answers one participant object on "
    "every route that mints one, so a client that learned a peer from the "
    "accept and then from the roster holds a single object for it"
) {
    LoopbackRig rig(0);
    rig.pump(4);

    rig.arrive(StringName("ana"));
    rig.arrive(StringName("bo"));
    rig.pump(4);

    const int first = rig.peer_id(0);
    NetwMultiplayer *watcher = rig.client(1);

    const Ref<netw::NetwPlayer> seen = watcher->player_joined_of(first);
    REQUIRE(seen.is_valid());
    CHECK(bool(watcher->player_joined_of(first) == seen));

    const int watching = rig.peer_id(1);
    watcher->session_receive_roster(
        rig.server()->session_roster_bytes(watching),
        1
    );
    CHECK(bool(watcher->player_joined_of(first) == seen));

    watcher->session_receive_accept(
        rig.server()->session_accept_bytes(first, watching),
        1
    );
    CHECK(bool(watcher->player_joined_of(first) == seen));
}

TEST_CASE(
    "[Networked][Session] JV5 a client reads the join address and the player "
    "limit the server advertises once admitted, and follows the server when "
    "it advertises another address without joining a second time"
) {
    LoopbackRig rig(0);
    rig.mount();
    rig.pump(4);
    Ref<netw::NetwServerInfo> info;
    info.instantiate();
    info->set_max_players(8);
    netw::Netw::configure_session(rig.branch(-1))->server(info);
    rig.server()->config_settle();

    const Ref<netw::NetwConnection> served = rig.server()->get_connection();
    served->set_join_address(String("203.0.113.4:21253"));
    NETW_CHECK_EQ(served->get_max_players(), int64_t(8));

    const Ref<netw::NetwConnection> first
        = rig.client(rig.arrive(StringName("ana")))->get_connection();
    REQUIRE(rig.client(0)->player_local().is_valid());
    CHECK(bool(first->get_join_address() == "203.0.113.4:21253"));
    NETW_CHECK_EQ(first->get_max_players(), int64_t(8));

    const Ref<netw::NetwPlayer> seated = rig.client(0)->player_local();
    served->set_join_address(String("203.0.113.9:21253"));
    rig.pump(4);
    CHECK(bool(first->get_join_address() == "203.0.113.9:21253"));
    CHECK(bool(rig.client(0)->player_local() == seated));
}

} // namespace TestNetwJoinPrivacyLaws

#endif
