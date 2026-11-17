#include "support/directory_stubs.h"
#include "support/minted_script.h"
#include "support/netw_cells.h"

#if defined(NETW_TIER_HOSTED)

#include "godot/node.hpp"
#include "godot/scene_tree.hpp"
#include "netw/api/connection.hpp"
#include "netw/api/context.hpp"
#include "netw/api/endpoint.hpp"
#include "netw/api/loopback.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/nodes/multiplayer_viewport.hpp"
#include "netw/api/promise.hpp"
#include "netw/api/session.hpp"

namespace TestLobbyDirectory {

using namespace godot;
using netw::MultiplayerViewport;
using netw::Netw;
using netw::NetwConnection;
using netw::NetwEndpoint;
using netw::NetwMultiplayer;
using netw::NetwPromise;
using netw_test::law_broken;
using netw_test::law_held;
using netw_test::LawVerdict;
using netw_test::StubDirectory;

constexpr const char *LOBBY = "StubLobbyPeer";

constexpr const char *RECORDER = R"(extends RefCounted

var invited: Array[NetwEndpoint] = []
var invited_by: Array[String] = []
var failures: Array[int] = []
var reasons: Array[String] = []

func on_invited(endpoint: NetwEndpoint) -> void:
	invited.append(endpoint)
	invited_by.append(endpoint.invited_by if endpoint.is_invited else "")

func on_failed(error: int, reason: String) -> void:
	failures.append(error)
	reasons.append(reason)
)";

struct Stand {
    String label;
};

typedef netw_test::LawRowFor<Stand> Row;

class HostingDirectory : public StubDirectory {
public:
    void host_lobby(const Dictionary &p_settings) override {
        host_calls += 1;
        hosted_with = p_settings.duplicate();
        Ref<netw::LocalMultiplayerPeer> peer;
        peer.instantiate();
        peer->create_server();
        deliver(peer);
    }
};

struct Lobby {
    MultiplayerViewport *tree = memnew(MultiplayerViewport);
    Node *probe = memnew(Node);
    HostingDirectory *directory = memnew(HostingDirectory);
    Ref<RefCounted> seen;
    Ref<NetwConnection> connection;
    NetwMultiplayer *api = nullptr;

    Lobby() {
        tree->set_name(StringName("LobbyTree"));
        probe->set_name(StringName("Probe"));
        tree->add_child(probe);
        netw::gd::scene_root()->add_child(tree);
        api = Object::cast_to<NetwMultiplayer>(tree->get_api().ptr());
        REQUIRE(api != nullptr);
        api->service_register(directory, nullptr);

        const Ref<Script> recorder = netw_test::minted_script(RECORDER);
        REQUIRE(recorder.is_valid());
        seen = recorder->call("new");
        connection = Netw::session(probe)->get_connection();
        connection->connect(
            StringName("invited"),
            Callable(seen.ptr(), StringName("on_invited"))
        );
        connection->connect(
            StringName("failed"),
            Callable(seen.ptr(), StringName("on_failed"))
        );
    }

    ~Lobby() {
        connection->set_browsing(false);
        api->set_multiplayer_peer(Ref<MultiplayerPeer>());
        api->service_unregister(directory, nullptr);
        memdelete(directory);
        tree->get_parent()->remove_child(tree);
        memdelete(tree);
    }

    Array recorded(const char *p_name) const {
        return seen->get(StringName(p_name));
    }

    Ref<NetwPromise> join(const String &p_address) {
        const Ref<NetwPromise> joining = connection->join(
            connection->add(StringName(LOBBY), p_address, String()),
            Dictionary()
        );
        api->poll();
        return joining;
    }
};

LawVerdict directory_invite(const Stand &) {
    Lobby lobby;
    lobby.directory->answering = StubDirectory::ANSWER_NOTHING;

    lobby.directory->invite(String("77"), String("Ada"));

    const Array invited = lobby.recorded("invited");
    if (invited.size() != 1) {
        return law_broken("invited was emitted %d times", int(invited.size()));
    }
    const Ref<NetwEndpoint> endpoint = invited[0];
    if (String(lobby.recorded("invited_by")[0]) != String("Ada")
        || endpoint->get_address() != String("77")
        || endpoint->get_source() != String("Stub Lobbies")) {
        return law_broken("the invite lost its sender, address or source");
    }

    lobby.directory->rows.push_back(String("42"));
    lobby.connection->set_browsing(true);
    lobby.api->poll();
    if (!endpoint->get_is_listed()
        || lobby.connection->find(StringName(LOBBY), String("77"))
            != endpoint) {
        return law_broken("a listing without the invite dropped it");
    }

    lobby.connection->join(endpoint, Dictionary());
    lobby.api->poll();
    if (lobby.directory->join_calls != 1
        || lobby.directory->joined_address != String("77")) {
        return law_broken("joining the invite did not reach its address");
    }
    if (lobby.connection->remove(endpoint) != OK || endpoint->get_is_listed()) {
        return law_broken("the player could not dismiss the invite");
    }
    return law_held();
}

LawVerdict social_invite(const Stand &) {
    Lobby lobby;
    const StringName enet("ENetMultiplayerPeer");
    const String address("127.0.0.1:7000");

    const Ref<NetwEndpoint> first
        = lobby.connection->invite(enet, address, String("Ada"));
    const Ref<NetwEndpoint> again
        = lobby.connection->invite(enet, address, String());
    const StringName missing("Missing");
    const bool refused
        = lobby.connection->invite(enet, String(), String()).is_null()
        && lobby.connection->invite(missing, String("1"), String()).is_null();

    if (first.is_null() || again != first) {
        return law_broken("a repeated invite made a second row");
    }
    if (lobby.recorded("invited").size() != 2
        || String(lobby.recorded("invited_by")[0]) != String("Ada")) {
        return law_broken("an invite was not announced with its sender");
    }
    if (!again->get_is_invited() || !again->get_invited_by().is_empty()) {
        return law_broken("an invite with no sender name was not pending");
    }
    if (lobby.connection->endpoints().size() != 1 || !refused) {
        return law_broken("an invite with no address or transport listed");
    }
    return law_held();
}

LawVerdict refusing_directory(const Stand &) {
    Lobby lobby;
    lobby.directory->answering = StubDirectory::ANSWER_REFUSAL;
    lobby.directory->refusal = ERR_UNAUTHORIZED;

    const Ref<NetwPromise> joining = lobby.join(String("7"));
    lobby.api->poll();

    const Array failures = lobby.recorded("failures");
    if (failures.size() != 1 || int(failures[0]) != int(ERR_UNAUTHORIZED)) {
        return law_broken("failed was emitted %d times", int(failures.size()));
    }
    if (String(lobby.recorded("reasons")[0]).find("refused") < 0) {
        return law_broken("the directory's reason was lost");
    }
    if (!joining->get_is_failed() || joining->get_code() != ERR_UNAUTHORIZED) {
        return law_broken("the join settled %d", int(joining->get_code()));
    }
    return law_held();
}

LawVerdict host_through_directory(const Stand &) {
    Lobby lobby;
    Dictionary settings;
    settings["max_players"] = int64_t(6);

    const Ref<NetwPromise> hosting
        = lobby.connection->host(StringName(LOBBY), settings);
    lobby.api->poll();

    if (lobby.directory->host_calls != 1
        || int64_t(lobby.directory->hosted_with.get("max_players", 0)) != 6) {
        return law_broken("host_lobby did not get the caller's settings");
    }
    if (!hosting->get_is_completed() || hosting->get_is_failed()) {
        return law_broken("hosting through the directory did not settle");
    }
    if (lobby.api->get_multiplayer_peer().is_null()) {
        return law_broken("the delivered peer was not assigned");
    }
    return law_held();
}

LawVerdict browser_closed(const Stand &) {
    Lobby lobby;
    lobby.connection->host(StringName(LOBBY), Dictionary());
    lobby.api->poll();
    lobby.connection->set_browsing(true);
    lobby.api->poll();
    const int listed = lobby.directory->browse_calls;

    lobby.connection->set_browsing(false);
    lobby.api->poll();

    if (listed != 1 || lobby.directory->stop_calls != 1) {
        return law_broken(
            "listing ran %d times and stopped %d",
            listed,
            lobby.directory->stop_calls
        );
    }
    if (lobby.directory->leave_calls != 0) {
        return law_broken("closing the browser left the lobby");
    }
    return law_held();
}

LawVerdict session_left(const Stand &) {
    int left_by_host = 0;
    {
        Lobby lobby;
        lobby.connection->host(StringName(LOBBY), Dictionary());
        lobby.api->poll();
        lobby.api->session_leave();
        left_by_host = lobby.directory->leave_calls;
    }
    Lobby lobby;
    lobby.join(String("77"));
    if (lobby.directory->leave_calls != 0) {
        return law_broken("the client left before session_leave");
    }
    lobby.api->session_leave();

    if (left_by_host != 1 || lobby.directory->leave_calls != 1) {
        return law_broken(
            "the host left %d times and the client %d",
            left_by_host,
            lobby.directory->leave_calls
        );
    }
    return law_held();
}

const Row ROWS[] = {
    {"a directory invites the player to lobby 77 from Ada, then lists "
     "lobbies without it",
     "invited carries Ada, the invite stays listed, joins lobby 77 and can "
     "be dismissed",
     &directory_invite},
    {"a social service invites twice to one ENet address, and once with "
     "no address or transport",
     "one row is announced each time and stays invited with no sender name, "
     "and the bad invites list nothing",
     &social_invite},
    {"a join the directory refuses with ERR_UNAUTHORIZED",
     "failed and the promise carry the directory's error and reason",
     &refusing_directory},
    {"hosting through a directory with six players",
     "host_lobby gets the settings and its peer becomes the session's",
     &host_through_directory},
    {"the browser closes while the player hosts a lobby",
     "the listing stops once and the lobby is kept",
     &browser_closed},
    {"session_leave on a host and a client a directory delivered",
     "each leaves the directory's lobby once",
     &session_left},
};

TEST_CASE(
    "[Netw][Connect][SceneTree] a lobby directory delivers invites, "
    "reports a refusal, and only leaving the session leaves its lobby"
) {
    const Stand stand = {String("a session with a lobby directory")};
    for (const Row &row : ROWS) {
        NETW_CELL(row, stand);
        NETW_LAW_HOLDS(row, stand);
    }
}

} // namespace TestLobbyDirectory

#endif
