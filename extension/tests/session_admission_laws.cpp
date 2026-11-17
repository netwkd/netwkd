#include "support/directory_stubs.h"
#include "support/internals.h"
#include "support/netw_call_log.h"
#include "support/netw_test.h"

#include "godot/class_db.hpp"
#include "godot/multiplayer.hpp"
#include "godot/scene_tree.hpp"
#include "godot/script.hpp"
#include "netw/api/admission_config.hpp"
#include "netw/api/connection.hpp"
#include "netw/api/context.hpp"
#include "netw/api/join_request.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/participant.hpp"
#include "netw/api/session.hpp"
#include "netw/auth_protocol.hpp"
#include "netw/session_core.hpp"

#if defined(NETW_TIER_HOSTED)

namespace TestNetwSessionAdmissionLaws {

using namespace godot;
using netw::NetwMultiplayer;
using netw_test::CallLog;

constexpr const char *CHALLENGE = "0123456789abcdef0123456789abcdef";

Ref<Script> shape_of(const char *p_source) {
    const Ref<Script> shape
        = netw::gd::instantiate_class(StringName("GDScript"));
    shape->set_source_code(String(p_source));
    shape->reload();
    return shape;
}

Node *mounted_branch(const char *p_name) {
    Node *branch = memnew(Node);
    branch->set_name(StringName(p_name));
    netw::gd::scene_root()->add_child(branch);
    return branch;
}

Ref<NetwMultiplayer> branch_session(Node *p_branch) {
    Ref<SceneMultiplayer> inner;
    inner.instantiate();
    inner->set_root_path(p_branch->get_path());
    Ref<NetwMultiplayer> made;
    made.instantiate();
    made->session_set_inner(inner);
    made->session_set_role(NetwMultiplayer::ROLE_LISTEN_SERVER);
    netw::gd::scene_tree()->set_multiplayer(made, p_branch->get_path());
    return made;
}

void release_branch(const Ref<NetwMultiplayer> &p_api, Node *p_branch) {
    p_api->embed_dispose();
    netw::gd::scene_tree()->set_multiplayer(
        Ref<MultiplayerAPI>(),
        p_branch->get_path()
    );
    netw::gd::scene_root()->remove_child(p_branch);
    memdelete(p_branch);
}

Node *gate_on(Node *p_branch, const char *p_source) {
    const Ref<Script> shape = shape_of(p_source);
    Node *written = Object::cast_to<Node>(shape->call("new"));
    p_branch->add_child(written);
    return written;
}

void join_as(
    const Ref<NetwMultiplayer> &p_api,
    const char *p_username,
    int64_t p_peer
) {
    netw::JoinRequest claim;
    claim.username = StringName(p_username);
    claim.app_tag = p_api->auth_app_tag_of();
    claim.wire_identity = netw::SessionCore::compute_wire_identity();
    p_api->session_receive_join(claim.serialize(), p_peer);
}

class IdentityDirectory : public netw_test::StubDirectory {
public:
    String account_of(int64_t p_peer) override {
        return p_peer == 1 ? String("test:host")
                           : String("test:") + String::num_int64(p_peer);
    }
    String member_name(int64_t) override {
        return String("Verified Name");
    }
    void host_lobby(const Dictionary &) override {
        Ref<netw::LocalMultiplayerPeer> peer;
        peer.instantiate();
        peer->create_server();
        deliver(peer);
    }
};

struct HostedBranch {
    Node *branch = nullptr;
    Ref<NetwMultiplayer> host;
    IdentityDirectory *directory = nullptr;

    explicit HostedBranch(bool p_directory) {
        branch = mounted_branch("AccountsHost");
        host = branch_session(branch);
        if (p_directory) {
            directory = memnew(IdentityDirectory);
            host->service_register(directory, nullptr);
        }
        Dictionary settings;
        settings["port"] = 0;
        const Ref<netw::NetwPromise> hosting = host->get_connection()->host(
            StringName(p_directory ? "StubLobbyPeer" : "ENetMultiplayerPeer"),
            settings
        );
        netw_test::Internals::connect_plane(host).on_poll(0.0);
        CHECK(hosting->get_is_completed());
    }

    ~HostedBranch() {
        host->NETW_API_VIRTUAL(set_multiplayer_peer)(Ref<MultiplayerPeer>());
        if (directory != nullptr) {
            host->service_unregister(directory, nullptr);
            memdelete(directory);
        }
        release_branch(host, branch);
    }
};

TEST_CASE(
    "[Netw][Session][SceneTree] a server served by a directory binds "
    "the challenge to the directory's account for peer 1, and a server "
    "whose transport identifies nobody leaves it unbound"
) {
    for (const bool directory : {false, true}) {
        CAPTURE(directory);
        HostedBranch hosted(directory);
        bool bound = false;
        const String made
            = hosted.host->session_bind_challenge(String(CHALLENGE), 9, bound);
        CHECK(bound == directory);
        CHECK(
            bool(
                made
                == netw::auth::bind_challenge(
                    String(CHALLENGE),
                    directory ? String("test:host") : String()
                )
            )
        );
    }
}

TEST_CASE(
    "[Netw][Session][SceneTree] a client that joined through a "
    "directory binds the challenge to the same account for peer 1 as the "
    "server does"
) {
    Node *branch = mounted_branch("BindingClient");
    const Ref<NetwMultiplayer> client = branch_session(branch);
    client->session_set_role(NetwMultiplayer::ROLE_CLIENT);
    IdentityDirectory *directory = memnew(IdentityDirectory);
    client->service_register(directory, nullptr);
    const Ref<netw::NetwConnection> connection = client->get_connection();
    const Ref<netw::NetwEndpoint> endpoint
        = connection->add(StringName("StubLobbyPeer"), String("77"), String());
    connection->join(endpoint, Dictionary());
    netw_test::Internals::connect_plane(client).on_poll(0.0);

    bool bound = false;
    const String made
        = client->session_bind_challenge(String(CHALLENGE), 1, bound);
    CHECK(bound);
    CHECK(
        bool(
            made
            == netw::auth::bind_challenge(
                String(CHALLENGE),
                String("test:host")
            )
        )
    );

    client->NETW_API_VIRTUAL(set_multiplayer_peer)(Ref<MultiplayerPeer>());
    client->service_unregister(directory, nullptr);
    memdelete(directory);
    release_branch(client, branch);
}

constexpr const char *ACCOUNT_GATE = R"(extends Node

var seats := 0
var kick_signed_in := false

func admit(request: NetwJoinRequest) -> void:
    request.account = "test:" + request.username
    request.account_name = String(request.username).to_upper()
    if kick_signed_in:
        var session: NetwSession = Netw.session(self)
        var signed_in: NetwPlayer = session.find_player(request.account)
        if signed_in != null:
            session.kick(signed_in, "Signed in somewhere else.")
    request.admit()

func seat(_player: NetwPlayer) -> void:
    seats += 1
)";

Node *account_gate_on(Node *p_branch) {
    Node *gate = gate_on(p_branch, ACCOUNT_GATE);
    REQUIRE(
        netw::Netw::configure_join(Callable(gate, StringName("seat")))
            .is_valid()
    );
    REQUIRE(
        netw::Netw::configure_admission(Callable(gate, StringName("admit")))
            .is_valid()
    );
    return gate;
}

struct SecondLogin {
    const char *arm;
    int64_t first_peer;
    bool replaced;
    int left_reason;
};

const SecondLogin SECOND_LOGINS[] = {
    {"kicked", 9, true, netw::NetwPlayer::LEAVE_KICKED},
    {"dropped", 9, true, netw::NetwPlayer::LEAVE_DROPPED},
    {"server", 1, false, -1},
};

TEST_CASE(
    "[Netw][Session][SceneTree] a second join of an account already "
    "playing replaces the first player, who leaves with its reason before "
    "the second is seated, and the server's own player is never replaced"
) {
    for (const SecondLogin &row : SECOND_LOGINS) {
        NETW_FORMAT_TEXT(arm_text, row.arm);
        CAPTURE(arm_text);
        const String path(row.arm);
        Node *branch = mounted_branch("AccountInUse");
        const Ref<NetwMultiplayer> host = branch_session(branch);
        Node *gate = account_gate_on(branch);
        gate->set(StringName("kick_signed_in"), path == "kicked");
        const CallLog heard;
        const Ref<netw::NetwSession> door = host->get_session();
        door->connect(StringName("player_left"), heard.callable("left"));
        door->connect(StringName("player_joined"), heard.callable("joined"));

        join_as(host, "ana", row.first_peer);
        const Ref<netw::NetwPlayer> first
            = host->player_joined_of(row.first_peer);
        REQUIRE(first.is_valid());
        if (path == "dropped") {
            host->session_settle_peer_loss(row.first_peer);
        }
        join_as(host, "ana", 11);

        const Ref<netw::NetwPlayer> second = host->player_joined_of(11);
        CHECK(second.is_valid() == row.replaced);
        CHECK(first->get_is_active() == !row.replaced);
        NETW_CHECK_EQ(
            host->player_find_account("test:ana"),
            (row.replaced ? second : first)->get_id()
        );
        NETW_CHECK_EQ(
            int(gate->get(StringName("seats"))),
            row.replaced ? 2 : 1
        );
        if (!row.replaced) {
            CHECK(
                String(host->session_refusal(11)).contains("already playing")
            );
            NETW_CHECK_EQ(heard.count(StringName("left")), 0);
            release_branch(host, branch);
            continue;
        }
        NETW_REQUIRE_EQ(heard.count(StringName("left")), 1);
        CHECK(
            bool(
                Ref<netw::NetwPlayer>(heard.args(StringName("left"))[0])
                == first
            )
        );
        NETW_CHECK_EQ(int(heard.args(StringName("left"))[1]), row.left_reason);
        const Vector<StringName> order = heard.order();
        NETW_REQUIRE_EQ(int(order.size()), 3);
        CHECK(bool(order[1] == StringName("left")));
        CHECK(bool(order[2] == StringName("joined")));
        release_branch(host, branch);
    }
}

} // namespace TestNetwSessionAdmissionLaws

#endif
