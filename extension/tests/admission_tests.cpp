#include "support/directory_stubs.h"
#include "support/fault_log.h"
#include "support/frame_drive.h"
#include "support/internals.h"
#include "support/netw_cells.h"
#include "support/netw_test.h"

#include "godot/class_db.hpp"
#include "godot/multiplayer.hpp"
#include "godot/scene_tree.hpp"
#include "godot/script.hpp"
#include "godot/time.hpp"
#include "godot/utility.hpp"
#include "netw/api/account_provider.hpp"
#include "netw/api/admission_config.hpp"
#include "netw/api/connection.hpp"
#include "netw/api/context.hpp"
#include "netw/api/join_request.hpp"
#include "netw/api/loopback.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/participant.hpp"
#include "netw/api/session.hpp"
#include "netw/auth_protocol.hpp"
#include "netw/session/frames.hpp"
#include "netw/session_core.hpp"

#include <initializer_list>
#include <vector>

#if defined(NETW_TIER_HOSTED)

namespace TestNetwAdmission {

using namespace godot;
using netw::NetwMultiplayer;
using netw_test::law_broken;
using netw_test::law_held;
using netw_test::LawVerdict;

namespace auth = netw::auth;

constexpr const char *GATE = R"(extends Node

var calls := 0
var seen_peer := 0
var seen_name := StringName()
var seen_team := StringName()
var verdict := OK

func admit(request: NetwJoinRequest) -> void:
	calls += 1
	seen_peer = request.peer_id
	seen_name = request.username
	if not request.args.is_empty():
		seen_team = request.args[0]
	if verdict == OK:
		request.admit()
	else:
		request.deny("", verdict)

func seat(_who, _team: StringName) -> void:
	pass
)";

constexpr const char *GATE_NOT_ANSWERING = R"(extends Node

func admit(_request: NetwJoinRequest):
	return OK
)";

constexpr const char *GATE_TAKING_PEER_ID = R"(extends Node

func admit(_peer_id: int, _username: StringName) -> void:
	pass
)";

constexpr const char *ASYNC_GATE = R"(extends Node

signal decided
var calls := 0
var delay_admit := false
var request: NetwJoinRequest
var account_seen := ""
var name_seen := ""

func admit(value: NetwJoinRequest) -> void:
    calls += 1
    request = value
    account_seen = value.account
    name_seen = value.account_name
    if delay_admit and value.peer_id != 1:
        await decided
    value.admit()
)";

constexpr const char *PROMISE_GATE = R"(extends Node

var results: Array[NetwPromise] = []

func admit(request: NetwJoinRequest) -> void:
    var result := NetwPromise.new()
    results.append(result)
    await result.wait()
    request.admit()
)";

constexpr const char *SCRIPTED_ACCOUNT = R"(extends NetwAccountProvider

signal checked

func _verify(request: NetwJoinRequest, _proof: Variant) -> void:
    await checked
    request.deny("Wrong passcode.")
)";

constexpr const char *CHALLENGE = "0123456789abcdef0123456789abcdef";

class TestAccount : public netw::NetwAccountProvider {
    Ref<netw::NetwPromise> hold() {
        Ref<netw::NetwPromise> later;
        later.instantiate();
        held.push_back(later);
        return later;
    }

public:
    String label;
    String account;
    String name;
    String padding;
    bool answers_challenge = true;
    bool by_username = false;
    Error verdict = OK;
    bool hold_prove = false;
    bool hold_verify = false;
    PackedStringArray heard;
    Ref<netw::NetwSession> proved_in;
    Ref<netw::NetwSession> verified_in;
    LocalVector<Ref<netw::NetwPromise>> held;

    String answer_to(const String &p_challenge) const {
        return label + String(":") + p_challenge + padding;
    }

    Variant prove(
        const String &p_challenge,
        const Ref<netw::NetwSession> &p_session
    ) override {
        heard.push_back(p_challenge);
        proved_in = p_session;
        if (account.is_empty()) {
            return Variant();
        }
        return hold_prove ? Variant(hold()) : Variant(answer_to(p_challenge));
    }

    Variant verify(
        const Ref<netw::NetwJoinRequest> &p_request,
        const Variant &p_proof
    ) override {
        verified_in = p_request->get_session();
        if (!account.is_empty() && p_proof.get_type() == Variant::STRING
            && String(p_proof) == answer_to(p_request->get_challenge())) {
            p_request->add_account(
                by_username
                    ? account + String(":") + String(p_request->get_username())
                    : account,
                name,
                answers_challenge
            );
        }
        if (verdict != OK) {
            p_request->deny(String(), verdict);
        }
        return hold_verify ? Variant(hold()) : Variant();
    }

    void release_prove() {
        held[held.size() - 1]->resolve(answer_to(heard[heard.size() - 1]));
    }
};

Ref<TestAccount> account_row(
    const char *p_label,
    const char *p_account = "",
    const char *p_name = ""
) {
    Ref<TestAccount> made(memnew(TestAccount));
    made->label = String(p_label);
    made->account = String(p_account);
    made->name = String(p_name);
    return made;
}

Ref<netw::NetwAccountProvider> as_provider(const Ref<TestAccount> &p_row) {
    return Ref<netw::NetwAccountProvider>(p_row.ptr());
}

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

Ref<netw::NetwAdmissionConfig> admit_through(Node *p_gate) {
    const Ref<netw::NetwAdmissionConfig> config
        = netw::Netw::configure_admission(
            Callable(p_gate, StringName("admit"))
        );
    REQUIRE(config.is_valid());
    return config;
}

struct Host {
    Node *branch;
    Ref<NetwMultiplayer> session;

    explicit Host(const char *p_name)
        : branch(mounted_branch(p_name)), session(branch_session(branch)) {
    }

    ~Host() {
        release_branch(session, branch);
    }

    Node *gate(const char *p_source) const {
        return gate_on(branch, p_source);
    }

    bool seated(int64_t p_peer) const {
        return session->player_joined_of(p_peer).is_valid();
    }

    String refusal(int64_t p_peer) const {
        return session->session_refusal(p_peer);
    }
};

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

void join_proving(
    const Ref<NetwMultiplayer> &p_api,
    int64_t p_peer,
    const Variant &p_proofs
) {
    p_api->auth_open_challenge_under_test(p_peer, String(CHALLENGE));
    netw::JoinRequest claim;
    claim.username = StringName("ana");
    claim.app_tag = p_api->auth_app_tag_of();
    claim.wire_identity = netw::SessionCore::compute_wire_identity();
    claim.proof_bytes = netw::gd::var_to_bytes(p_proofs);
    p_api->session_receive_join(claim.serialize(), p_peer);
}

Array proofs_for(
    std::initializer_list<const TestAccount *> p_rows,
    const String &p_challenge
) {
    Array proofs;
    for (const TestAccount *row : p_rows) {
        proofs.push_back(
            row->account.is_empty() ? Variant()
                                    : Variant(row->answer_to(p_challenge))
        );
    }
    return proofs;
}

PackedByteArray bytes_of(const std::initializer_list<int> &p_values) {
    PackedByteArray out;
    for (const int value : p_values) {
        out.push_back(uint8_t(value));
    }
    return out;
}

struct Admission {
    String label;
};

typedef netw_test::LawRowFor<Admission> Row;

LawVerdict handler_ok(const Admission &) {
    const Host host("AdmitOk");
    admit_through(host.gate(GATE));

    join_as(host.session, "ana", 9);

    if (!host.seated(9)) {
        return law_broken("peer 9 has no player");
    }
    if (!host.refusal(9).is_empty()) {
        return law_broken("peer 9 carries a refusal");
    }
    return law_held();
}

LawVerdict handler_error(const Admission &) {
    const Host host("AdmitError");
    Node *gate = host.gate(GATE);
    gate->set(StringName("verdict"), int(ERR_UNAUTHORIZED));
    admit_through(gate);

    join_as(host.session, "ana", 9);

    if (host.seated(9)) {
        return law_broken("peer 9 was seated");
    }
    if (!host.refusal(9).contains(
            String(netw::gd::error_name(ERR_UNAUTHORIZED))
        )) {
        return law_broken("the refusal does not name the Error");
    }
    return law_held();
}

LawVerdict refusal_spends_no_player(const Admission &) {
    const Host host("AdmitSpends");
    Node *gate = host.gate(GATE);
    gate->set(StringName("verdict"), int(ERR_UNAUTHORIZED));
    admit_through(gate);

    join_as(host.session, "ana", 9);
    gate->set(StringName("verdict"), int(OK));
    join_as(host.session, "bo", 11);

    const Ref<netw::NetwPlayer> seated = host.session->player_joined_of(11);
    if (seated.is_null()) {
        return law_broken("peer 11 has no player");
    }
    if (seated->get_id() != 1) {
        return law_broken(
            "peer 11 holds player %lld",
            (long long)seated->get_id()
        );
    }
    return law_held();
}

LawVerdict handler_sees_join(const Admission &) {
    const Host host("AdmitSees");
    Node *gate = host.gate(GATE);
    REQUIRE(
        netw::Netw::configure_join(Callable(gate, StringName("seat")))
            .is_valid()
    );
    admit_through(gate);
    Array carried;
    carried.push_back(StringName("red"));

    host.session->session_submit_join(StringName("ana"), carried);

    if (int64_t(gate->get(StringName("seen_peer")))
        != host.session->NETW_API_VIRTUAL(get_unique_id)()) {
        return law_broken("the handler saw another peer");
    }
    if (StringName(gate->get(StringName("seen_name"))) != StringName("ana")) {
        return law_broken("the handler saw another username");
    }
    if (StringName(gate->get(StringName("seen_team"))) != StringName("red")) {
        return law_broken("the handler saw other join arguments");
    }
    return law_held();
}

LawVerdict host_refused(const Admission &) {
    netw_test::FaultWatch expected_faults;
    expected_faults.expect_error("*Unauthorized*[NW0020]*", 1);
    const Host host("AdmitHostRefused");
    Node *gate = host.gate(GATE);
    gate->set(StringName("verdict"), int(ERR_UNAUTHORIZED));
    admit_through(gate);

    host.session->session_submit_join(StringName("host"), Array());

    if (host.session->player_local().is_valid()) {
        return law_broken("the host was seated");
    }
    return law_held();
}

LawVerdict handler_gives_no_answer(const Admission &) {
    netw_test::FaultWatch expected_faults;
    expected_faults.expect_error("*NetwJoinRequest.admit*[NW1042]*", 1);
    const Host host("AdmitNoAnswer");
    admit_through(host.gate(GATE_NOT_ANSWERING));

    join_as(host.session, "ana", 9);

    if (host.seated(9)) {
        return law_broken("peer 9 was seated");
    }
    if (!host.refusal(9).contains(String("Unauthorized"))) {
        return law_broken("the refusal does not say why");
    }
    return law_held();
}

LawVerdict handler_wrong_signature(const Admission &) {
    netw_test::FaultWatch expected_faults;
    expected_faults.expect_error("*The admission handler*[NW1002]*", 1);
    expected_faults.expect_error("*cannot take a NetwJoinRequest*[NW0020]*", 1);
    const Host host("AdmitSignature");
    admit_through(host.gate(GATE_TAKING_PEER_ID));

    join_as(host.session, "ana", 9);
    host.session->session_submit_join(StringName("host"), Array());

    if (host.seated(9)) {
        return law_broken("peer 9 was seated");
    }
    if (host.session->player_local().is_valid()) {
        return law_broken("the host was seated");
    }
    return law_held();
}

enum AwaitedStep {
    AWAIT_ADMIT,
    AWAIT_VERIFY,
    AWAIT_PROVE,
};

template <AwaitedStep STEP> LawVerdict awaited(const Admission &) {
    for (const bool timed_out : {false, true}) {
        const Host host("AwaitAdmission");
        Node *gate = host.gate(ASYNC_GATE);
        gate->set(StringName("delay_admit"), STEP == AWAIT_ADMIT);
        const Ref<netw::NetwAdmissionConfig> config = admit_through(gate);
        const Ref<TestAccount> held = account_row("held", "held:ana");
        held->hold_verify = STEP == AWAIT_VERIFY;
        held->hold_prove = STEP == AWAIT_PROVE;
        if (STEP != AWAIT_ADMIT) {
            config->accept(as_provider(held));
        }
        const auto seated = [&]() {
            return STEP == AWAIT_PROVE ? host.session->player_local().is_valid()
                                       : host.seated(9);
        };

        if (STEP == AWAIT_ADMIT) {
            join_as(host.session, "ana", 9);
        } else if (STEP == AWAIT_VERIFY) {
            join_proving(
                host.session,
                9,
                proofs_for({held.ptr()}, String(CHALLENGE))
            );
        } else {
            host.session->session_submit_join(StringName("host"), Array());
        }
        if (seated()) {
            return law_broken("seated before the step decided");
        }
        if (timed_out) {
            host.session->session_poll_admissions(
                int64_t(Time::get_singleton()->get_ticks_msec()) + 10000
            );
        }
        if (STEP == AWAIT_ADMIT) {
            gate->emit_signal(StringName("decided"));
        } else {
            REQUIRE(held->held.size() == 1);
            if (STEP == AWAIT_VERIFY) {
                held->held[0]->resolve(Variant());
            } else {
                held->release_prove();
            }
        }

        if (seated() == timed_out) {
            return law_broken("timed_out=%d seated=%d", timed_out, seated());
        }
    }
    return law_held();
}

LawVerdict own_join_outlives_timeout(const Admission &) {
    const Host host("OwnJoinTimeout");
    const Ref<TestAccount> held = account_row("held", "held:host");
    held->hold_prove = true;
    admit_through(host.gate(ASYNC_GATE))
        ->accept(as_provider(held))
        ->timeout(60.0);

    host.session->session_submit_join(StringName("host"), Array());
    host.session->session_poll_admissions(
        int64_t(Time::get_singleton()->get_ticks_msec())
        + int64_t(host.session->get_auth_timeout() * 1000.0) + 10000
    );
    held->release_prove();

    if (host.session->player_local().is_null()) {
        return law_broken("the host was not seated");
    }
    return law_held();
}

LawVerdict missing_proof(const Admission &) {
    const Host host("LocalProof");
    Node *gate = host.gate(ASYNC_GATE);
    const Ref<TestAccount> held = account_row("held", "held:host");
    held->hold_prove = true;
    admit_through(gate)->accept(as_provider(held));

    join_as(host.session, "missing", 9);
    join_as(host.session, "", 10);
    for (const int64_t peer : {int64_t(9), int64_t(10)}) {
        if (host.seated(peer)) {
            return law_broken("peer %lld was seated", (long long)peer);
        }
        const String refusal = host.refusal(peer);
        if (!refusal.contains(String("admission proof is missing"))) {
            return law_broken("peer %d was refused another way", int(peer));
        }
    }
    if (int(gate->get("calls")) != 0) {
        return law_broken("the handler ran for a join with no proof");
    }

    host.session->session_submit_join(StringName("host"), Array());
    if (host.session->player_local().is_valid()) {
        return law_broken("the host was seated before it proved");
    }
    held->release_prove();
    if (host.session->player_local().is_null()) {
        return law_broken("the host was not seated after it proved");
    }
    const Ref<netw::NetwJoinRequest> request = gate->get("request");
    REQUIRE(request.is_valid());
    if (!request->get_args().is_empty()) {
        return law_broken("the proof reached the join arguments");
    }
    if (request->get_accounts() != PackedStringArray({"held:host"})
        || !request->is_bound(String("held:host"))) {
        return law_broken("the host's account is not proven and bound");
    }
    return law_held();
}

LawVerdict challenge_needs_provider(const Admission &) {
    for (const bool providing : {false, true}) {
        const Host host("NoChallenge");
        Node *gate = host.gate(ASYNC_GATE);
        const Ref<netw::NetwAdmissionConfig> config = admit_through(gate);
        if (providing) {
            config->accept(as_provider(account_row("only", "only:1")));
        }

        host.session->session_submit_join(StringName("host"), Array());

        const Ref<netw::NetwJoinRequest> request = gate->get("request");
        REQUIRE(request.is_valid());
        if (request->get_challenge().is_empty() == providing) {
            return law_broken("providing=%d sent another challenge", providing);
        }
        if (host.session->player_local().is_null()) {
            return law_broken("providing=%d seated no host", providing);
        }
    }
    return law_held();
}

LawVerdict proof_count_differs(const Admission &) {
    struct Shape {
        int sent;
        bool array;
    };
    for (const Shape &shape :
         {Shape{1, true}, Shape{3, true}, Shape{0, false}}) {
        const Host host("ProofCount");
        const Ref<TestAccount> first = account_row("first", "first:1");
        const Ref<TestAccount> second = account_row("second", "second:2");
        admit_through(host.gate(ASYNC_GATE))
            ->accept(as_provider(first))
            ->accept(as_provider(second));
        Array sent;
        for (int at = 0; at < shape.sent; ++at) {
            sent.push_back(first->answer_to(String(CHALLENGE)));
        }

        join_proving(
            host.session,
            9,
            shape.array ? Variant(sent) : Variant(String("forged"))
        );

        if (host.seated(9)) {
            return law_broken("%d proofs seated peer 9", shape.sent);
        }
        const String refusal = host.refusal(9);
        if (!refusal.contains(String("accepts 2 accounts"))
            || !refusal.contains(vformat("proved %d", shape.sent))) {
            return law_broken("%d proofs were refused another way", shape.sent);
        }
    }
    return law_held();
}

LawVerdict scripted_verify_denies(const Admission &) {
    const Host host("ScriptedVerify");
    Node *gate = host.gate(ASYNC_GATE);
    Ref<netw::NetwAccountProvider> scripted;
    scripted.instantiate();
    scripted->set_script(shape_of(SCRIPTED_ACCOUNT));
    admit_through(gate)->accept(scripted);
    Array proofs;
    proofs.push_back(String("ticket"));

    join_proving(host.session, 9, proofs);
    scripted->emit_signal(StringName("checked"));

    if (host.seated(9)) {
        return law_broken("peer 9 was seated");
    }
    if (int(gate->get("calls")) != 0) {
        return law_broken("the handler ran for a refused join");
    }
    if (!host.refusal(9).contains(String("Wrong passcode."))) {
        return law_broken("the refusal does not carry the reason");
    }
    return law_held();
}

LawVerdict verify_error(const Admission &) {
    const Host host("VerifyRefuses");
    Node *gate = host.gate(ASYNC_GATE);
    const Ref<TestAccount> first = account_row("first", "first:1");
    const Ref<TestAccount> second = account_row("second", "second:2");
    first->verdict = ERR_UNAUTHORIZED;
    admit_through(gate)
        ->accept(as_provider(first))
        ->accept(as_provider(second));

    join_proving(
        host.session,
        9,
        proofs_for({first.ptr(), second.ptr()}, String(CHALLENGE))
    );

    if (host.seated(9)) {
        return law_broken("peer 9 was seated");
    }
    if (int(gate->get("calls")) != 0) {
        return law_broken("the handler ran for a refused join");
    }
    if (!host.refusal(9).contains(
            String(netw::gd::error_name(ERR_UNAUTHORIZED))
        )) {
        return law_broken("the refusal does not name the Error");
    }
    return law_held();
}

LawVerdict late_promise(const Admission &) {
    for (const bool verifying : {false, true}) {
        const Host host("ReplacedAdmission");
        Node *gate = host.gate(verifying ? ASYNC_GATE : PROMISE_GATE);
        const Ref<netw::NetwAdmissionConfig> config = admit_through(gate);
        const Ref<TestAccount> held = account_row("held", "held:ana");
        held->hold_verify = true;
        if (verifying) {
            config->accept(as_provider(held));
        }
        const Array proofs = proofs_for({held.ptr()}, String(CHALLENGE));
        const auto join = [&]() {
            if (verifying) {
                join_proving(host.session, 9, proofs);
            } else {
                join_as(host.session, "ana", 9);
            }
        };
        const auto promise_at = [&](int p_at) -> Ref<netw::NetwPromise> {
            if (verifying) {
                return held->held[p_at];
            }
            const Array results = gate->get("results");
            return results[p_at];
        };

        join();
        const Ref<netw::NetwPromise> first = promise_at(0);
        host.session->session_clear_disconnected_peer(9);
        join();
        const Ref<netw::NetwPromise> second = promise_at(1);

        first->resolve(Variant());
        if (host.seated(9)) {
            return law_broken("verifying=%d seated on the late one", verifying);
        }
        second->resolve(Variant());
        if (!host.seated(9)) {
            return law_broken("verifying=%d did not seat at all", verifying);
        }
    }
    return law_held();
}

LawVerdict accounts_in_order(const Admission &) {
    struct Provider {
        const char *label;
        const char *account;
        const char *name;
        bool answers_challenge;
    };
    struct Arm {
        bool directory;
        std::vector<Provider> providers;
        PackedStringArray accounts;
        const char *first_name;
        bool first_bound;
    };
    const Arm arms[] = {
        {false,
         {{"a", "a:1", "Ann", true}, {"b", "b:2", "Bea", true}},
         {"a:1", "b:2"},
         "Ann",
         false},
        {false,
         {{"a", "", "", true}, {"b", "b:2", "Bea", true}},
         {"b:2"},
         "Bea",
         false},
        {true, {{"a", "a:1", "Ann", true}}, {"a:1", "test:9"}, "Ann", true},
        {true,
         {{"t", "test:9", "", false}},
         {"test:9"},
         "Verified Name",
         true},
        {true, {}, {"test:9"}, "Verified Name", true},
        {false, {}, {}, "", false},
    };
    int at = 0;
    for (const Arm &arm : arms) {
        HostedBranch hosted(arm.directory);
        Node *gate = gate_on(hosted.branch, ASYNC_GATE);
        const Ref<netw::NetwAdmissionConfig> config = admit_through(gate);
        LocalVector<Ref<TestAccount>> made;
        Array proofs;
        const String challenge = auth::bind_challenge(
            String(CHALLENGE),
            arm.directory ? String("test:host") : String()
        );
        for (const Provider &provider : arm.providers) {
            const Ref<TestAccount> account
                = account_row(provider.label, provider.account, provider.name);
            account->answers_challenge = provider.answers_challenge;
            config->accept(as_provider(account));
            proofs.push_back(
                account->account.is_empty()
                    ? Variant()
                    : Variant(account->answer_to(challenge))
            );
            made.push_back(account);
        }

        if (made.is_empty()) {
            join_as(hosted.host, "ana", 9);
        } else {
            join_proving(hosted.host, 9, proofs);
        }

        const Ref<netw::NetwJoinRequest> request = gate->get("request");
        REQUIRE(request.is_valid());
        const String first
            = arm.accounts.is_empty() ? String() : arm.accounts[0];
        if (request->get_accounts() != arm.accounts) {
            return law_broken("arm %d listed other accounts", at);
        }
        if (String(gate->get("account_seen")) != first
            || String(gate->get("name_seen")) != String(arm.first_name)) {
            return law_broken("arm %d started from another account", at);
        }
        if (request->is_bound(first) != arm.first_bound) {
            return law_broken("arm %d bound the first account wrongly", at);
        }
        if (hosted.host->player_joined_of(9).is_null()) {
            return law_broken("arm %d seated nobody", at);
        }
        at += 1;
    }
    return law_held();
}

void hold_auth(int64_t, const PackedByteArray &) {
}

LawVerdict client_holds_unannounced_link(const Admission &) {
    const netw_test::FaultWatch faults;
    constexpr int64_t APP_TAG = 0x5a5a5a5a;
    constexpr int64_t OTHER = 42;
    Ref<NetwMultiplayer> session;
    session.instantiate();
    session->auth_set_app_tag(APP_TAG);
    Ref<netw::LocalMultiplayerPeer> local;
    Ref<netw::LocalMultiplayerPeer> remote;
    local.instantiate();
    remote.instantiate();
    local->create_client(9);
    remote->create_server(OTHER);
    local->force_connect_peer(OTHER, remote.ptr());
    remote->force_connect_peer(9, local.ptr());
    session->set_auth_callback(callable_mp_static(&hold_auth));
    session->auth_arm();
    session->session_get_inner()->set_multiplayer_peer(local);
    session->session_get_inner()->poll();
    session->set_auth_callback(Callable());
    session->session_set_desired_role(NetwMultiplayer::ROLE_CLIENT);
    session->session_peer_assigned(true, 9);

    session->auth_receive_hello(OTHER, auth::encode_client_hello(APP_TAG, 0));
    const bool early = session->auth_link_is_completed(OTHER);
    netw::session::AcceptFrame announced;
    announced.peer_id = OTHER;
    announced.username = StringName("ana");
    announced.player_id = 2;
    session->session_admit(announced);
    const bool announced_link = session->auth_link_is_completed(OTHER);

    session->embed_dispose();
    local->NETW_PEER_VIRTUAL(close)();
    remote->NETW_PEER_VIRTUAL(close)();
    if (early) {
        return law_broken("the link completed before the announcement");
    }
    if (!announced_link) {
        return law_broken("the link stayed in auth after the announcement");
    }
    if (faults.errors() != 0) {
        return law_broken("%d errors", int(faults.errors()));
    }
    return law_held();
}

LawVerdict packet_kind(const Admission &) {
    if (auth::classify(auth::encode_client_hello(0, 0)) != auth::Kind::HELLO) {
        return law_broken("a hello was not read as one");
    }
    if (auth::classify(auth::encode_probe_request(0)) != auth::Kind::PROBE) {
        return law_broken("a probe was not read as one");
    }
    if (auth::classify(bytes_of({0, 1, 2, 3, 4, 5})) != auth::Kind::UNKNOWN) {
        return law_broken("foreign bytes were read as ours");
    }
    if (auth::classify(bytes_of({0x4E, 0x48})) != auth::Kind::UNKNOWN) {
        return law_broken("a cut header was read as ours");
    }
    return law_held();
}

LawVerdict hello_refused_by_name(const Admission &) {
    auth::Hello decoded = auth::decode_client_hello(
        auth::encode_client_hello(0x11111111, 0),
        0x22222222
    );
    if (decoded.ok() || decoded.refusal != auth::Refusal::APP) {
        return law_broken("another build was not refused as one");
    }
    decoded = auth::decode_client_hello(auth::encode_probe_request(0), 0);
    if (decoded.ok() || decoded.refusal != auth::Refusal::FRAMING) {
        return law_broken("a probe was not refused as a hello");
    }
    if (auth::decode_probe_request(auth::encode_client_hello(0, 0)).ok) {
        return law_broken("a hello was read as a probe");
    }
    PackedByteArray stale = auth::magic_hello();
    stale.push_back(0xFF);
    while (stale.size() < auth::HELLO_HEADER_LEN) {
        stale.push_back(0x00);
    }
    decoded = auth::decode_client_hello(stale, 0);
    if (decoded.ok() || decoded.refusal != auth::Refusal::VERSION) {
        return law_broken("another version was not refused as one");
    }
    return law_held();
}

LawVerdict relayed_proof(const Admission &) {
    const String challenge = String("ab").repeat(16);
    const String target = auth::bind_challenge(challenge, "sha-256 aa:bb");
    const String relay = auth::bind_challenge(challenge, "sha-256 cc:dd");
    if (target == relay || target == challenge) {
        return law_broken("two servers bind one challenge alike");
    }
    return law_held();
}

const Row ROWS[] = {
    {"a join the admission handler answers OK for",
     "the peer is seated with no refusal",
     &handler_ok},
    {"a join the admission handler answers an Error for",
     "the peer is not seated and the refusal names that Error",
     &handler_error},
    {"a refused join followed by an admitted one",
     "the admitted peer holds the player id the refused one would have",
     &refusal_spends_no_player},
    {"a join carrying a username and join arguments",
     "the handler is handed the peer, the username and the arguments",
     &handler_sees_join},
    {"a handler that refuses the host's own join",
     "the host has no player and one error says so",
     &host_refused},
    {"a handler that returns OK and never admits or denies",
     "the join is refused as unauthorized and one error says so",
     &handler_gives_no_answer},
    {"a handler that cannot take a NetwJoinRequest",
     "every join is refused, the host's too, with one error each",
     &handler_wrong_signature},
    {"an awaited admission",
     "it seats once it decides, and seats nobody after auth_timeout",
     &awaited<AWAIT_ADMIT>},
    {"an awaited verify",
     "it seats once it decides, and seats nobody after auth_timeout",
     &awaited<AWAIT_VERIFY>},
    {"an awaited prove on the host",
     "it seats once it decides, and seats nobody after auth_timeout",
     &awaited<AWAIT_PROVE>},
    {"the host's own proof outliving auth_timeout",
     "the host is still seated under the admission timeout",
     &own_join_outlives_timeout},
    {"a join with no proof where one is required",
     "it is refused before the handler, and the host waits on its own proof",
     &missing_proof},
    {"a config with no provider, then one",
     "a challenge is sent only when a provider is accepted",
     &challenge_needs_provider},
    {"a peer proving a different number of accounts than accepted",
     "it is refused and the refusal names both counts",
     &proof_count_differs},
    {"a provider whose verify answers an Error",
     "the join is refused with that Error and the handler never runs",
     &verify_error},
    {"a scripted provider whose _verify awaits and then denies",
     "the join is refused with its reason and the handler never runs",
     &scripted_verify_denies},
    {"a late answer for a join the peer left",
     "it cannot seat or refuse the peer's next join",
     &late_promise},
    {"providers and a transport that identifies accounts",
     "accounts list in accept order, then the transport's, from the first",
     &accounts_in_order},
    {"another client's hello on a client",
     "its link stays in auth until the server announces that player",
     &client_holds_unannounced_link},
    {"a hello, a probe, foreign bytes and a cut header",
     "each is read as what it is, and foreign bytes as nothing of ours",
     &packet_kind},
    {"a hello from another build, framing or version",
     "it is refused by name",
     &hello_refused_by_name},
    {"one challenge bound for two servers",
     "the two bound values differ, so a relayed proof answers nothing",
     &relayed_proof},
};

TEST_CASE("[Networked][Session][SceneTree] who a server admits and why") {
    const Admission admission = {String("a server deciding a join")};
    for (const Row &row : ROWS) {
        NETW_CELL(row, admission);
        NETW_LAW_HOLDS(row, admission);
    }
}

struct ProofEvidence {
    bool driven = false;
    bool waited_for_proof = false;
    bool outlasted_timeout_hint = false;
    bool waited_for_admission = false;
    bool admitted = false;
    bool account_bound = false;
    bool prove_heard_challenge = false;
    bool proved_in_own_session = false;
    bool verified_in_server_session = false;
    int errors = 0;
    int failure = OK;
    String failure_reason;
};

ProofEvidence &proof_evidence() {
    static ProofEvidence evidence;
    return evidence;
}

ProofEvidence &abandoned_proof_evidence() {
    static ProofEvidence evidence;
    return evidence;
}

ProofEvidence &webrtc_proof_evidence() {
    static ProofEvidence evidence;
    return evidence;
}

constexpr const char *PAIRED_SIGNALER = R"(extends NetwWebRTCSignaler

static var inbox := {}
var me := 0

func _open(_room_id: String, local_peer_id: int) -> Error:
    me = local_peer_id
    inbox[me] = []
    return OK

func _room_id() -> String:
    return "PAIRED"

func _send(
        to_peer_id: int, _to: String, kind: String, payload: Dictionary
) -> void:
    if not inbox.has(to_peer_id):
        inbox[to_peer_id] = []
    inbox[to_peer_id].append([me, kind, payload])

func _poll(_delta: float) -> void:
    report_ready()
    var mail: Array = inbox.get(me, [])
    inbox[me] = []
    for sent in mail:
        receive(sent[0], str(sent[0]), sent[1], sent[2])
)";

class ProofScenario : public netw_test::FrameScenario {
    ProofEvidence &(*evidence_of)();
    const char *peer_class;
    bool server_gives_up;
    Ref<Script> signaler;
    Node *host_branch = nullptr;
    Node *client_branch = nullptr;
    Node *host_gate = nullptr;
    Node *client_gate = nullptr;
    Ref<TestAccount> host_account;
    Ref<TestAccount> client_account;
    Ref<NetwMultiplayer> host;
    Ref<NetwMultiplayer> client;
    Ref<netw::NetwPromise> hosting;
    Ref<netw::NetwPromise> joining;
    Ref<netw_test::FaultLog> diagnostics;
    int phase = 0;
    int64_t started_msec = 0;

    Dictionary settings() const {
        Dictionary made;
        if (signaler.is_valid()) {
            made["signaler"] = signaler->call("new");
        } else {
            made["port"] = 0;
        }
        return made;
    }

public:
    ProofScenario(
        ProofEvidence &(*p_evidence_of)(),
        const char *p_peer_class,
        bool p_server_gives_up = false
    )
        : evidence_of(p_evidence_of), peer_class(p_peer_class),
          server_gives_up(p_server_gives_up) {
    }

    bool advance() override {
        ProofEvidence &evidence = evidence_of();
        const int64_t now = int64_t(Time::get_singleton()->get_ticks_msec());
        if (phase == 0) {
            started_msec = now;
            if (String(peer_class) == "WebRTCMultiplayerPeer") {
                signaler = shape_of(PAIRED_SIGNALER);
            }
            diagnostics.instantiate();
            netw_test::FaultLog::attach(diagnostics);
            host_branch = mounted_branch("ProofHost");
            client_branch = mounted_branch("ProofClient");
            host = branch_session(host_branch);
            client = branch_session(client_branch);
            client->session_set_role(NetwMultiplayer::ROLE_CLIENT);
            if (server_gives_up) {
                host->set_auth_timeout(0.1);
            }
            host_gate = gate_on(host_branch, ASYNC_GATE);
            client_gate = gate_on(client_branch, ASYNC_GATE);
            host_gate->set(StringName("delay_admit"), true);
            const String padding = String("x").repeat(2048);
            host_account = account_row("club", "club:member");
            client_account = account_row("club", "club:member");
            host_account->padding = padding;
            client_account->padding = padding;
            host_account->by_username = true;
            client_account->hold_prove = true;
            netw::Netw::configure_admission(
                Callable(host_gate, StringName("admit"))
            )
                ->accept(as_provider(host_account));
            netw::Netw::configure_admission(
                Callable(client_gate, StringName("admit"))
            )
                ->accept(as_provider(client_account));
            host->session_prepare_join(StringName("host"), Array());
            client->session_prepare_join(StringName("client"), Array());
            hosting = host->get_connection()->host(
                StringName(peer_class),
                settings()
            );
            phase = 1;
            return true;
        }
        if (host.is_valid()) {
            host->NETW_API_VIRTUAL(poll)();
        }
        if (client.is_valid()) {
            client->NETW_API_VIRTUAL(poll)();
        }
        if (now - started_msec > 15000) {
            phase = 5;
        }
        switch (phase) {
            case 1: {
                if (hosting->get_is_failed()) {
                    phase = 5;
                    break;
                }
                if (!hosting->get_is_completed()) {
                    break;
                }
                const String address = host->transport_get_join_address();
                const Ref<netw::NetwEndpoint> endpoint
                    = client->get_connection()->add(
                        StringName(peer_class),
                        signaler.is_valid() ? address
                                            : String("127.0.0.1:")
                                + address.get_slice(String(":"), 1),
                        String()
                    );
                joining = client->get_connection()->join(endpoint, settings());
                phase = 2;
                break;
            }
            case 2: {
                if (client_account->heard.is_empty()) {
                    break;
                }
                evidence.waited_for_proof = !client->session_is_online();
                netw_test::Internals::connect_plane(client).on_poll(60.0);
                evidence.outlasted_timeout_hint
                    = netw_test::Internals::connect_plane(client)
                          .attempt_state()
                    == int(netw::connect::ATTEMPT_CONNECTING);
                if (server_gives_up) {
                    phase = 6;
                    break;
                }
                client_account->release_prove();
                phase = 3;
                break;
            }
            case 6: {
                if (!joining->get_is_failed()) {
                    break;
                }
                evidence.failure = int(joining->get_code());
                evidence.failure_reason = joining->get_detail();
                phase = 5;
                break;
            }
            case 3: {
                if (int(host_gate->get("calls")) < 2) {
                    break;
                }
                evidence.waited_for_admission = client->player_local().is_null()
                    && host->peer_get_player(client->get_unique_id()) == 0;
                const Ref<netw::NetwJoinRequest> request
                    = host_gate->get("request");
                evidence.account_bound = request.is_valid()
                    && request->is_bound(String("club:member:client"));
                evidence.prove_heard_challenge = request.is_valid()
                    && client_account->heard[0] == request->get_challenge();
                evidence.proved_in_own_session
                    = client_account->proved_in.is_valid()
                    && client_account->proved_in == client->get_session()
                    && host_account->proved_in == host->get_session();
                evidence.verified_in_server_session = request.is_valid()
                    && request->get_session().is_valid()
                    && request->get_session() == host->get_session()
                    && host_account->verified_in == host->get_session();
                host_gate->emit_signal(StringName("decided"));
                phase = 4;
                break;
            }
            case 4: {
                const int64_t joined = client->get_unique_id();
                if (client->player_local().is_null()
                    || host->player_joined_of(joined).is_null()) {
                    break;
                }
                evidence.admitted = true;
                phase = 5;
                break;
            }
            default: {
                hosting.unref();
                joining.unref();
                client->NETW_API_VIRTUAL(set_multiplayer_peer)(
                    Ref<MultiplayerPeer>()
                );
                host->NETW_API_VIRTUAL(set_multiplayer_peer)(
                    Ref<MultiplayerPeer>()
                );
                release_branch(client, client_branch);
                release_branch(host, host_branch);
                client.unref();
                host.unref();
                host_account.unref();
                client_account.unref();
                evidence.errors = int(diagnostics->error_count());
                netw_test::FaultLog::detach(diagnostics);
                diagnostics.unref();
                signaler.unref();
                evidence.driven = true;
                return false;
            }
        }
        return true;
    }
};

NETW_FRAME_SCENARIO(
    ProofScenario,
    proof_scenario,
    &proof_evidence,
    "ENetMultiplayerPeer"
);
NETW_FRAME_SCENARIO(
    ProofScenario,
    webrtc_proof_scenario,
    &webrtc_proof_evidence,
    "WebRTCMultiplayerPeer"
);
NETW_FRAME_SCENARIO(
    ProofScenario,
    abandoned_proof_scenario,
    &abandoned_proof_evidence,
    "ENetMultiplayerPeer",
    true
);

struct Proving {
    String label;
};

typedef netw_test::LawRowFor<Proving> ProofRow;

LawVerdict deadline_passes(const Proving &) {
    const ProofEvidence &evidence = abandoned_proof_evidence();
    if (!evidence.driven || !evidence.waited_for_proof) {
        return law_broken("the client never waited on its proof");
    }
    if (evidence.failure != int(ERR_TIMEOUT)) {
        return law_broken("the join failed with %d", evidence.failure);
    }
    if (!evidence.failure_reason.contains("time")) {
        return law_broken("the failure carries no reason");
    }
    if (evidence.errors != 0) {
        return law_broken("%d errors", evidence.errors);
    }
    return law_held();
}

LawVerdict announced_after_proof(const Proving &) {
    struct Arm {
        const ProofEvidence &evidence;
        bool bound;
    };
    for (const Arm &arm : {
             Arm{proof_evidence(), false},
             Arm{webrtc_proof_evidence(), true},
         }) {
        const ProofEvidence &evidence = arm.evidence;
        if (!evidence.driven || !evidence.waited_for_proof
            || !evidence.outlasted_timeout_hint) {
            return law_broken("bound=%d did not wait on its proof", arm.bound);
        }
        if (!evidence.waited_for_admission || !evidence.admitted) {
            return law_broken("bound=%d was not announced after", arm.bound);
        }
        if (!evidence.prove_heard_challenge) {
            return law_broken("bound=%d proved another challenge", arm.bound);
        }
        if (evidence.account_bound != arm.bound) {
            return law_broken("bound=%d bound wrongly", arm.bound);
        }
        if (evidence.errors != 0) {
            return law_broken(
                "bound=%d raised %d errors",
                arm.bound,
                evidence.errors
            );
        }
    }
    return law_held();
}

LawVerdict sessions_in_one_process(const Proving &) {
    for (const ProofEvidence *evidence :
         {&proof_evidence(), &webrtc_proof_evidence()}) {
        if (!evidence->driven || !evidence->proved_in_own_session) {
            return law_broken("a provider proved in another session");
        }
        if (!evidence->verified_in_server_session) {
            return law_broken("a provider verified outside the server");
        }
    }
    return law_held();
}

const ProofRow PROOF_ROWS[] = {
    {"a client still proving when the join deadline passes",
     "the join fails with ERR_TIMEOUT and a reason",
     &deadline_passes},
    {"a client joining over ENet or WebRTC",
     "it is announced only after its proof and an awaited admission, and "
     "only WebRTC binds the challenge to the server",
     &announced_after_proof},
    {"a host and a client in one process",
     "each proves in its own session and verifies in the server's",
     &sessions_in_one_process},
};

TEST_CASE("[Networked][Session][Frame] a client proving over a real link") {
    const Proving proving = {String("a host and a client over a real link")};
    for (const ProofRow &row : PROOF_ROWS) {
        NETW_CELL(row, proving);
        NETW_LAW_HOLDS(row, proving);
    }
}

} // namespace TestNetwAdmission

#endif
