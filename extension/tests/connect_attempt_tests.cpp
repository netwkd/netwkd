#include "support/directory_stubs.h"
#include "support/loopback_rig.h"
#include "support/minted_script.h"
#include "support/netw_cells.h"

#if defined(NETW_TIER_HOSTED)

#include "godot/node.hpp"
#include "godot/os.hpp"
#include "godot/scene_tree.hpp"
#include "godot/time.hpp"
#include "netw/api/connection.hpp"
#include "netw/api/context.hpp"
#include "netw/api/loopback.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/nodes/multiplayer_viewport.hpp"
#include "netw/api/promise.hpp"
#include "netw/api/session.hpp"

namespace TestConnectAttempt {

using namespace godot;
using netw::MultiplayerViewport;
using netw::Netw;
using netw::NetwConnection;
using netw::NetwMultiplayer;
using netw::NetwPromise;
using netw::NetwSession;
using netw_test::law_broken;
using netw_test::law_held;
using netw_test::LawVerdict;
using netw_test::LoopbackRig;

constexpr const char *SILENT_TRANSPORT = R"(extends NetwTransport

static var hint := 5.0
static var built := 0
static var cancelled := 0
static var settings := {}
static var tickets: Array[RID] = []
static var standing := 0

func _peer_class() -> StringName:
	return &"AttemptTestPeer"

func _display_name() -> String:
	return "Silent"

func _timeout_hint() -> float:
	return hint

func _client_settings() -> Dictionary:
	return {a = "built-in", b = "built-in", c = "built-in"}

func _make_peer(
	ticket: RID,
	_mode: NetwConnection.TransportMode,
	_address: String,
	p_settings: Dictionary,
) -> void:
	built += 1
	settings = p_settings
	standing = get_instance_id()
	tickets.append(ticket)

func _cancel_peer_creation(_ticket: RID) -> void:
	cancelled += 1

static func fail_first() -> void:
	var transport := instance_from_id(standing) as NetwTransport
	transport.fail(tickets[0], ERR_CANT_CONNECT, "late")
)";

constexpr const char *RECORDER = R"(extends RefCounted

var states: Array[int] = []
var failures: Array[int] = []

func on_state(state: int) -> void:
	states.append(state)

func on_failed(error: int, _reason: String) -> void:
	failures.append(error)
)";

struct Stand {
    String label;
};

typedef netw_test::LawRowFor<Stand> Row;

struct Attempt {
    MultiplayerViewport *tree = memnew(MultiplayerViewport);
    Node *probe = memnew(Node);
    Ref<Script> transport = netw_test::minted_script(SILENT_TRANSPORT);
    Ref<RefCounted> seen;
    Ref<NetwConnection> connection;

    explicit Attempt(double p_hint = 5.0) {
        tree->set_name(StringName("AttemptTree"));
        probe->set_name(StringName("Probe"));
        tree->add_child(probe);
        netw::gd::scene_root()->add_child(tree);
        REQUIRE(transport.is_valid());
        transport->set(StringName("hint"), p_hint);

        const Ref<Script> recorder = netw_test::minted_script(RECORDER);
        REQUIRE(recorder.is_valid());
        seen = recorder->call("new");
        const Ref<NetwSession> session = Netw::session(probe);
        REQUIRE(session.is_valid());
        connection = session->get_connection();
        session->connect(
            StringName("state_changed"),
            Callable(seen.ptr(), StringName("on_state"))
        );
        connection->connect(
            StringName("failed"),
            Callable(seen.ptr(), StringName("on_failed"))
        );
        REQUIRE(connection->register_transport(transport));
    }

    ~Attempt() {
        api()->set_multiplayer_peer(Ref<MultiplayerPeer>());
        tree->get_parent()->remove_child(tree);
        memdelete(tree);
    }

    Ref<MultiplayerAPI> api() const {
        return tree->get_api();
    }

    Ref<NetwPromise> join(const Dictionary &p_settings = Dictionary()) {
        return connection->join(
            connection->add(
                StringName("AttemptTestPeer"),
                String("nowhere.example"),
                String()
            ),
            p_settings
        );
    }

    int stat(const char *p_name) const {
        return int(transport->get(StringName(p_name)));
    }

    Array states() const {
        return seen->get(StringName("states"));
    }

    Array failures() const {
        return seen->get(StringName("failures"));
    }

    void poll_for(double p_seconds) const {
        const uint64_t until = Time::get_singleton()->get_ticks_usec()
            + uint64_t(p_seconds * 1000000.0);
        api()->poll();
        while (Time::get_singleton()->get_ticks_usec() < until) {
            OS::get_singleton()->delay_usec(5000);
            api()->poll();
        }
    }
};

bool settled(const Ref<NetwPromise> &p_promise, Error p_code) {
    return p_promise->get_is_failed() && p_promise->get_code() == p_code;
}

LawVerdict silent_transport(const Stand &) {
    Attempt attempt(0.05);
    const Ref<NetwPromise> outcome = attempt.join();
    attempt.poll_for(0.02);
    const int built = attempt.stat("built");
    if (built != 1) {
        return law_broken("the transport was asked %d times", built);
    }
    if (outcome->get_is_completed()) {
        return law_broken("the attempt settled before its timeout hint");
    }

    attempt.poll_for(0.15);
    const Array failures = attempt.failures();
    if (failures.size() != 1 || int(failures[0]) != int(ERR_TIMEOUT)) {
        return law_broken("failed was emitted %d times", int(failures.size()));
    }
    if (!settled(outcome, ERR_TIMEOUT)) {
        return law_broken("the promise settled %d", int(outcome->get_code()));
    }
    if (attempt.stat("cancelled") != 1) {
        return law_broken("the transport's creation was not cancelled");
    }
    const Array states = attempt.states();
    if (int(states.back()) != int(NetwMultiplayer::SESSION_STATE_OFFLINE)) {
        return law_broken("the session did not return offline");
    }
    return law_held();
}

LawVerdict cancel_while_creating(const Stand &) {
    Attempt attempt;
    const Ref<NetwPromise> outcome = attempt.join();
    attempt.connection->cancel();
    attempt.poll_for(0.02);

    const Array states = attempt.states();
    if (states.size() != 2
        || int(states[0]) != int(NetwMultiplayer::SESSION_STATE_CREATING)
        || int(states[1]) != int(NetwMultiplayer::SESSION_STATE_OFFLINE)) {
        return law_broken("state_changed walked %d", int(states.size()));
    }
    if (!attempt.failures().is_empty()) {
        return law_broken("a cancel was reported as a failure");
    }
    if (!settled(outcome, ERR_SKIP)) {
        return law_broken("the promise settled %d", int(outcome->get_code()));
    }
    if (attempt.stat("built") != 0) {
        return law_broken("a cancelled attempt still asked the transport");
    }
    return law_held();
}

LawVerdict late_report(const Stand &) {
    Attempt attempt;
    attempt.join();
    attempt.poll_for(0.02);
    attempt.connection->cancel();
    attempt.poll_for(0.02);
    const Ref<NetwPromise> outcome = attempt.join();
    attempt.poll_for(0.02);
    const int built = attempt.stat("built");
    if (built != 2) {
        return law_broken("the transport was asked %d times", built);
    }

    attempt.transport->call(StringName("fail_first"));
    attempt.poll_for(0.02);
    if (outcome->get_is_settled()) {
        return law_broken("the cancelled ticket settled the next join");
    }
    if (!attempt.failures().is_empty()) {
        return law_broken("the cancelled ticket emitted failed");
    }
    return law_held();
}

LawVerdict join_while_online(const Stand &) {
    Attempt attempt;
    Ref<netw::LocalMultiplayerPeer> hosting;
    hosting.instantiate();
    REQUIRE(hosting->create_server() == OK);
    attempt.api()->set_multiplayer_peer(hosting);
    const int walked = int(attempt.states().size());

    const Ref<NetwPromise> outcome = attempt.join();
    attempt.poll_for(0.02);

    if (!settled(outcome, ERR_ALREADY_IN_USE)) {
        return law_broken("the promise settled %d", int(outcome->get_code()));
    }
    if (attempt.api()->get_multiplayer_peer() != hosting) {
        return law_broken("the hosting peer was replaced");
    }
    if (int(attempt.states().size()) != walked || attempt.stat("built") != 0) {
        return law_broken("a refused join started an attempt");
    }
    if (!attempt.failures().is_empty()) {
        return law_broken("a refused join emitted failed");
    }
    return law_held();
}

LawVerdict settings_layers(const Stand &) {
    Attempt attempt;
    Dictionary preset;
    preset["b"] = String("defaults");
    preset["c"] = String("defaults");
    REQUIRE(
        attempt.connection->set_defaults(
            StringName("AttemptTestPeer"),
            preset
        ) == OK
    );
    Dictionary passed;
    passed["c"] = String("call");

    attempt.join(passed);
    attempt.poll_for(0.02);

    const Dictionary got = attempt.transport->get(StringName("settings"));
    if (String(got.get("a", String())) != String("built-in")
        || String(got.get("b", String())) != String("defaults")
        || String(got.get("c", String())) != String("call")) {
        return law_broken(
            "the transport got %s",
            String(Variant(got)).utf8().get_data()
        );
    }
    return law_held();
}

String &served_address() {
    static String held;
    return held;
}

class MatchDirectory : public netw_test::StubDirectory {
public:
    Ref<netw::LocalMultiplayerPeer> handing;

    void join_lobby(const String &) override {
        deliver(handing);
    }

    String join_address() override {
        return served_address();
    }
};

Vector<MatchDirectory *> &served() {
    static Vector<MatchDirectory *> held;
    return held;
}

void join_through_directory(
    NetwMultiplayer *p_api,
    const Ref<netw::LocalMultiplayerPeer> &p_peer
) {
    MatchDirectory *made = memnew(MatchDirectory);
    made->handing = p_peer;
    served().push_back(made);
    p_api->service_register(made, nullptr);
    p_api->endpoint_join(
        p_api->endpoint_add(
            p_api->transport_find(StringName("StubLobbyPeer")),
            String("matched:ticket"),
            String()
        ),
        Dictionary()
    );
    p_api->poll();
}

LawVerdict lobby_join_address(const Stand &) {
    served_address() = String("match-7");
    String at_server;
    String at_client;
    {
        LoopbackRig rig(1, false, &join_through_directory);
        rig.mount();
        rig.join(-1, StringName("host"));
        rig.pump(8);
        at_server = rig.server()->transport_get_join_address();
        at_client = rig.client(0)->transport_get_join_address();
    }
    for (MatchDirectory *directory : served()) {
        memdelete(directory);
    }
    served().clear();
    if (at_server != String("match-7") || at_client != String("match-7")) {
        return law_broken(
            "the server advertises '%s' and the client reads '%s'",
            at_server.utf8().get_data(),
            at_client.utf8().get_data()
        );
    }
    return law_held();
}

const Row ROWS[] = {
    {"join through a transport that never answers, with a 0.05 second "
     "timeout hint",
     "the promise and one failed signal carry ERR_TIMEOUT, the transport's "
     "creation is cancelled and the session goes offline",
     &silent_transport},
    {"cancel while the transport creates the peer",
     "state_changed walks creating then offline, failed stays silent and "
     "the promise settles ERR_SKIP",
     &cancel_while_creating},
    {"a transport fails the ticket of a join that was cancelled",
     "the join started after it stays pending and failed stays silent",
     &late_report},
    {"join while the session hosts",
     "the promise settles ERR_ALREADY_IN_USE and the hosting peer stays",
     &join_while_online},
    {"join with transport settings, defaults and call settings",
     "the call overrides the defaults, which override the transport",
     &settings_layers},
    {"a server that came up by joining a lobby",
     "server and client advertise the lobby's join address",
     &lobby_join_address},
};

TEST_CASE(
    "[Netw][Connect][SceneTree] a join attempt settles once, a cancel "
    "is not a failure, and a lobby server advertises its own address"
) {
    const Stand stand = {String("a mounted session")};
    for (const Row &row : ROWS) {
        NETW_CELL(row, stand);
        NETW_LAW_HOLDS(row, stand);
    }
}

} // namespace TestConnectAttempt

#endif
