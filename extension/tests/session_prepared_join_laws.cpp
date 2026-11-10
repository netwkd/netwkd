#include "support/fault_log.h"
#include "support/netw_test.h"

#include <optional>

#include "netw/api/loopback.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/participant.hpp"

namespace TestNetwSessionPreparedJoin {

using namespace godot;
using netw::LocalMultiplayerPeer;
using netw::NetwMultiplayer;

constexpr const char *NOTHING = "(nothing submitted)";

Ref<NetwMultiplayer> a_session() {
    Ref<NetwMultiplayer> session;
    session.instantiate();
    return session;
}

String submitted(const Ref<NetwMultiplayer> &p_session) {
    const std::optional<netw::JoinRequest> taken
        = p_session->session_take_submitted_join_under_test();
    if (!taken.has_value()) {
        return NOTHING;
    }
    return String(taken->username);
}

TEST_CASE(
    "[Networked][Session][Hosted] PJ1 a client that reaches a session submits "
    "the join it prepared exactly once, because entering spends the "
    "preparation rather than reading it, so a reconnect on the same session "
    "submits the default join with no username"
) {
    const Ref<NetwMultiplayer> session = a_session();
    NETW_CHECK_EQ(
        int(session->session_prepare_join(StringName("ana"), Array())
                ->get_result()),
        int(OK)
    );
    REQUIRE(session->session_prepared_join().has_value());
    CHECK(session->session_prepared_join()->username == StringName("ana"));

    session->session_set_role(NetwMultiplayer::ROLE_CLIENT);
    session->session_announce_entered();

    CHECK(submitted(session) == String("ana"));
    CHECK_FALSE(session->session_prepared_join().has_value());

    session->session_announce_entered();
    CHECK(submitted(session) == String());

    session->embed_dispose();
}

TEST_CASE(
    "[Networked][Session][Hosted] PJ2 a listen server holds its prepared join "
    "until its startup scenes exist and then submits it exactly once, because "
    "a host's own player cannot be seated into scenes that have not spawned"
) {
    const Ref<NetwMultiplayer> session = a_session();
    session->session_prepare_join(StringName("ana"), Array());

    session->session_set_role(NetwMultiplayer::ROLE_LISTEN_SERVER);
    session->session_announce_entered();

    CHECK(submitted(session) == NOTHING);
    CHECK(session->player_local().is_null());
    REQUIRE(session->session_prepared_join().has_value());
    CHECK(session->session_prepared_join()->username == StringName("ana"));

    session->scene_announce_startup();

    const Ref<netw::NetwPlayer> seated = session->player_local();
    REQUIRE(seated.is_valid());
    CHECK(seated->get_username() == StringName("ana"));
    CHECK_FALSE(session->session_prepared_join().has_value());

    session->scene_announce_startup();
    CHECK(session->player_local() == seated);

    session->embed_dispose();
}

TEST_CASE(
    "[Networked][Session][Hosted] PJ7 a dedicated server that prepared no "
    "join submits none, because it is not a player"
) {
    const Ref<NetwMultiplayer> session = a_session();

    session->session_set_role(NetwMultiplayer::ROLE_DEDICATED_SERVER);
    session->session_announce_entered();
    session->scene_announce_startup();

    CHECK(submitted(session) == NOTHING);
    CHECK(session->player_local().is_null());

    session->embed_dispose();
}

TEST_CASE(
    "[Networked][Session][Hosted] PJ3 falling offline discards the prepared "
    "join, so the next session this one enters submits the default join and "
    "never a stale identity"
) {
    const Ref<NetwMultiplayer> session = a_session();
    session->session_prepare_join(StringName("ana"), Array());
    CHECK(session->session_prepared_join().has_value());

    session->session_transition(NetwMultiplayer::SESSION_STATE_CONNECTING);
    session->session_transition(NetwMultiplayer::SESSION_STATE_OFFLINE);

    CHECK_FALSE(session->session_prepared_join().has_value());

    session->session_set_role(NetwMultiplayer::ROLE_CLIENT);
    session->session_announce_entered();
    CHECK(submitted(session) == String());

    session->embed_dispose();
}

TEST_CASE(
    "[Networked][Session][Hosted] PJ4 a join with no username prepares "
    "nothing, because the name IS the request and no other field could stand "
    "in for one"
) {
    netw_test::FaultWatch expected_faults;
    expected_faults.expect_error("*Netw.prepare_join*[NW0031]*", 1);

    const Ref<NetwMultiplayer> session = a_session();
    NETW_CHECK_EQ(
        int(session->session_prepare_join(StringName(), Array())->get_result()),
        int(ERR_INVALID_PARAMETER)
    );
    CHECK_FALSE(session->session_prepared_join().has_value());

    session->embed_dispose();
}

Ref<NetwMultiplayer> a_client(int p_local) {
    Ref<LocalMultiplayerPeer> peer;
    peer.instantiate();
    NETW_CHECK_EQ(int(peer->create_client(p_local)), int(OK));

    const Ref<NetwMultiplayer> session = a_session();
    session->NETW_API_VIRTUAL(set_multiplayer_peer)(peer);
    session->session_set_role(NetwMultiplayer::ROLE_CLIENT);
    return session;
}

TEST_CASE(
    "[Networked][Session][Hosted] PJ5 a join held for one resend waits for "
    "the server it was addressed to, so another peer arriving first leaves "
    "the retry armed and the server spends it"
) {
    const Ref<NetwMultiplayer> session = a_client(9);
    session->session_prepare_join(StringName("ana"), Array());
    REQUIRE(session->session_prepared_join().has_value());

    session->session_announce_entered();
    CHECK(submitted(session) == String("ana"));

    session->emit_signal(StringName("peer_connected"), int64_t(7));
    CHECK(submitted(session) == NOTHING);

    session->emit_signal(StringName("peer_connected"), int64_t(1));
    CHECK(submitted(session) == String("ana"));

    session->emit_signal(StringName("peer_connected"), int64_t(1));
    CHECK(submitted(session) == NOTHING);

    session->embed_dispose();
}

TEST_CASE(
    "[Networked][Session][Hosted] PJ6 a preparation that is replaced or "
    "cleared takes its held resend with it, so the server arriving "
    "afterwards cannot seat the request nobody is waiting on any more"
) {
    const Ref<NetwMultiplayer> session = a_client(9);
    session->session_prepare_join(StringName("ana"), Array());

    session->session_announce_entered();
    CHECK(submitted(session) == String("ana"));

    session->session_prepare_join(StringName("bo"), Array());
    session->emit_signal(StringName("peer_connected"), int64_t(1));
    CHECK(submitted(session) == NOTHING);

    session->session_announce_entered();
    CHECK(submitted(session) == String("bo"));

    session->session_clear_prepared_join();
    session->emit_signal(StringName("peer_connected"), int64_t(1));
    CHECK(submitted(session) == NOTHING);

    session->embed_dispose();
}

TEST_CASE(
    "[Networked][Session][Hosted] W6 a client that assigned a peer with no "
    "player arranged is left exactly as connected as it was"
) {
    Ref<LocalMultiplayerPeer> client;
    client.instantiate();
    NETW_CHECK_EQ(int(client->create_client(7)), int(OK));

    const Ref<NetwMultiplayer> session = a_session();
    session->NETW_API_VIRTUAL(set_multiplayer_peer)(client);
    session->session_set_role(NetwMultiplayer::ROLE_CLIENT);
    REQUIRE_FALSE(session->session_prepared_join().has_value());

    session->session_announce_entered();

    CHECK(
        session->NETW_API_VIRTUAL(get_multiplayer_peer)()
        == Ref<MultiplayerPeer>(client)
    );
    NETW_CHECK_EQ(
        int(session->session_get_role()),
        int(NetwMultiplayer::ROLE_CLIENT)
    );
    CHECK(
        int(session->session_get_state())
        != int(NetwMultiplayer::SESSION_STATE_OFFLINE)
    );
    CHECK(session->player_local().is_null());

    session->embed_dispose();
}

} // namespace TestNetwSessionPreparedJoin
