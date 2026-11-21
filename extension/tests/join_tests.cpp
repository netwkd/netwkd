#include "support/fault_log.h"
#include "support/loopback_rig.h"
#include "support/minted_script.h"
#include "support/netw_cells.h"
#include "support/netw_recorder.h"
#include "support/netw_test.h"

#include "godot/scene_tree.hpp"
#include "netw/api/context.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/loopback.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/nodes/multiplayer_viewport.hpp"
#include "netw/api/participant.hpp"
#include "netw/api/session.hpp"

#if defined(NETW_TIER_HOSTED)

namespace TestNetwJoin {

using namespace godot;
using netw::Netw;
using netw::NetwEntity;
using netw::NetwMultiplayer;
using netw::NetwPlayer;
using netw_test::law_broken;
using netw_test::law_held;
using netw_test::LawVerdict;
using netw_test::LoopbackRig;

constexpr const char *SEAT = R"(extends Node

var carried: Array = []

func seat(_who) -> void:
	carried.append([])
)";

constexpr const char *SEAT_TEAM = R"(extends Node

var carried: Array = []

func seat(_who, team: StringName, slot: int) -> void:
	carried.append([team, slot])
)";

constexpr const char *SEAT_SLOT = R"(extends Node

var carried: Array = []

func seat(_who, slot: int) -> void:
	carried.append([slot])
)";

constexpr const char *ACCOUNT_GATE = R"(extends Node

func admit(request: NetwJoinRequest) -> void:
	request.account = "test:" + request.username
	request.account_name = String(request.username).to_upper()
	request.admit()
)";

struct Stand {
    String label;
};

typedef netw_test::LawRowFor<Stand> Row;

Node *placed(Node *p_branch, const char *p_source) {
    Node *made = netw_test::minted_node(p_source);
    REQUIRE(made != nullptr);
    p_branch->add_child(made);
    return made;
}

Node *handler_on(Node *p_branch, const char *p_source) {
    Node *made = placed(p_branch, p_source);
    const Callable seat(made, StringName("seat"));
    REQUIRE(Netw::configure_join(seat).is_valid());
    return made;
}

Array carried(Node *p_handler) {
    return p_handler->get(StringName("carried"));
}

Array args_of(const Variant &p_first, const Variant &p_second = Variant()) {
    Array out;
    out.push_back(p_first);
    if (p_second.get_type() != Variant::NIL) {
        out.push_back(p_second);
    }
    return out;
}

int mounted_client(LoopbackRig &p_rig, const Array &p_args) {
    const int at = p_rig.add_client(StringName("ana"), p_args);
    p_rig.mount_late(at, false);
    return at;
}

Ref<NetwPlayer> local_of(NetwMultiplayer *p_api) {
    return p_api->player_local();
}

LawVerdict one_name_twice(const Stand &) {
    LoopbackRig rig(0);
    const int first = rig.arrive(StringName("ana"));
    const int second = rig.arrive(StringName("ana"));

    for (const int at : {first, second}) {
        const Ref<NetwPlayer> mine = local_of(rig.client(at));
        if (mine.is_null() || mine->get_username() != StringName("ana")) {
            return law_broken("client %d is not seated as ana", at);
        }
        const Ref<NetwPlayer> seen
            = rig.server()->player_joined_of(rig.peer_id(at));
        if (seen.is_null() || !seen->get_is_active()
            || seen->get_username() != StringName("ana")) {
            return law_broken("the server holds no active ana for %d", at);
        }
        if (!mine->get_entities().is_empty()) {
            return law_broken("client %d was placed with no handler", at);
        }
    }
    return law_held();
}

LawVerdict nameless_join(const Stand &) {
    LoopbackRig rig(0);
    rig.mount();
    Node *handler = handler_on(rig.branch(-1), SEAT);
    const Ref<NetwPlayer> host = rig.join(-1, StringName("host"));
    rig.add_client();
    rig.add_client();
    rig.pump(6);

    if (host.is_null() || host != local_of(rig.server())) {
        return law_broken("the host is not its own local player");
    }
    for (int at = -1; at < 2; ++at) {
        NetwMultiplayer *api = at < 0 ? rig.server() : rig.client(at);
        const Ref<NetwPlayer> mine = local_of(api);
        if (at >= 0
            && (mine.is_null() || !String(mine->get_username()).is_empty())) {
            return law_broken("client %d holds no unnamed player", at);
        }
        const int64_t own = api->player_get_local();
        if (own != mine->get_id()
            || int64_t(mine->get(StringName("id"))) != own) {
            return law_broken("peer %d reads another id as its own", at);
        }
        const PackedInt32Array listed = api->player_list();
        if (listed.size() != 3 || !listed.has(own)) {
            return law_broken("peer %d does not list three players", at);
        }
    }
    const int ran = carried(handler).size();
    if (ran != 3) {
        return law_broken("the handler ran %d times", ran);
    }
    return law_held();
}

LawVerdict typed_arguments(const Stand &) {
    LoopbackRig rig(0);
    rig.mount();
    Node *served = handler_on(rig.branch(-1), SEAT_TEAM);
    const int at
        = mounted_client(rig, args_of(StringName("north"), int64_t(3)));
    Node *joiner = handler_on(rig.branch(at), SEAT_TEAM);
    rig.pump(6);

    if (local_of(rig.client(at)).is_null()) {
        return law_broken("the client was not seated");
    }
    const Array heard = carried(served);
    if (heard.size() != 1) {
        return law_broken("the server's handler ran %d times", heard.size());
    }
    const Array values = heard[0];
    if (values.size() != 2 || values[0].get_type() != Variant::STRING_NAME
        || StringName(values[0]) != StringName("north")
        || values[1].get_type() != Variant::INT || int(values[1]) != 3) {
        const String read = Variant(values);
        return law_broken("the handler read %s", read.utf8().get_data());
    }
    if (!carried(joiner).is_empty()) {
        return law_broken("the client's own handler ran");
    }
    return law_held();
}

LawVerdict refused_join(LoopbackRig &p_rig, int p_at, const char *p_reason) {
    const Ref<netw::NetwSession> session = Netw::session(p_rig.branch(p_at));
    REQUIRE(session.is_valid());
    netw_test::Recorder heard(session.ptr(), {StringName("join_failed")});
    p_rig.pump(6);

    const StringName failed("join_failed");
    if (heard.count(failed) != 1) {
        return law_broken("join_failed fired %d times", heard.count(failed));
    }
    const Array told = heard.args(failed);
    if (int(told[0]) != int(ERR_UNCONFIGURED)
        || !String(told[1]).contains(p_reason)) {
        const String reason = told[1];
        return law_broken("the client heard %s", reason.utf8().get_data());
    }
    if (local_of(p_rig.client(p_at)).is_valid()
        || p_rig.server()->player_joined_of(p_rig.peer_id(p_at)).is_valid()) {
        return law_broken("the refused client holds a player");
    }
    return law_held();
}

LawVerdict differing_signature(const Stand &) {
    LoopbackRig rig(0);
    rig.mount();
    Node *served = handler_on(rig.branch(-1), SEAT_TEAM);
    const int at = mounted_client(rig, args_of(int64_t(3)));
    handler_on(rig.branch(at), SEAT_SLOT);

    const LawVerdict verdict = refused_join(rig, at, "takes 2 arguments");
    if (verdict.held && !carried(served).is_empty()) {
        return law_broken("the server's handler ran");
    }
    return verdict;
}

LawVerdict missing_arguments(const Stand &) {
    netw_test::FaultWatch faults;
    faults.expect_warning("*[NW0027]*", 1);

    LoopbackRig rig(0);
    rig.mount();
    Node *served = handler_on(rig.branch(-1), SEAT_SLOT);
    const int at = rig.add_client();
    rig.mount_late(at, false);

    const LawVerdict verdict = refused_join(rig, at, "carried 0");
    if (verdict.held && !carried(served).is_empty()) {
        return law_broken("the server's handler ran");
    }
    return verdict;
}

LawVerdict rival_handlers(const Stand &) {
    netw_test::FaultWatch faults;
    faults.expect_error("*Netw.configure_join*[NW0034]*", 1);

    LoopbackRig rig(0);
    rig.mount();
    Node *first = placed(rig.branch(-1), SEAT);
    Node *second = placed(rig.branch(-1), SEAT);
    Netw::configure_join(Callable(first, StringName("seat")));
    Netw::configure_join(Callable(second, StringName("seat")));

    const int refused = rig.arrive(StringName("ana"));
    if (rig.server()->player_joined_of(rig.peer_id(refused)).is_valid()) {
        return law_broken("a join was seated under two handlers");
    }

    rig.branch(-1)->remove_child(second);
    const int seated = rig.arrive(StringName("bo"));
    memdelete(second);

    if (local_of(rig.client(seated)).is_null()) {
        return law_broken("the remaining handler seated nobody");
    }
    const int ran = carried(first).size();
    if (ran != 1) {
        return law_broken("the remaining handler ran %d times", ran);
    }
    return law_held();
}

LawVerdict account_privacy(const Stand &) {
    LoopbackRig rig(0);
    rig.mount();
    Node *gate = placed(rig.branch(-1), ACCOUNT_GATE);
    const Callable admit(gate, StringName("admit"));
    REQUIRE(Netw::configure_admission(admit).is_valid());
    const int ana = rig.peer_id(rig.arrive(StringName("ana")));
    const int bo = rig.peer_id(rig.arrive(StringName("bo")));

    NetwMultiplayer *host = rig.server();
    NetwMultiplayer *first = rig.client(0);
    NetwMultiplayer *second = rig.client(1);
    if (host->player_joined_of(ana)->get_account() != String("test:ana")
        || host->player_joined_of(bo)->get_account() != String("test:bo")) {
        return law_broken("the server does not hold both accounts");
    }
    if (first->player_joined_of(ana)->get_account() != String("test:ana")
        || second->player_joined_of(bo)->get_account() != String("test:bo")) {
        return law_broken("a player does not hold its own account");
    }
    const Ref<NetwPlayer> accepted = first->player_joined_of(bo);
    const Ref<NetwPlayer> rostered = second->player_joined_of(ana);
    if (accepted.is_null() || rostered.is_null()) {
        return law_broken("a client does not see the other player");
    }
    if (!accepted->get_account().is_empty()
        || !rostered->get_account().is_empty()) {
        return law_broken("a client sees another player's account");
    }
    if (accepted->get_account_name() != String("BO")
        || rostered->get_account_name() != String("ANA")) {
        return law_broken("a client does not see the account name");
    }
    return law_held();
}

LawVerdict rejoin_is_new(const Stand &) {
    LoopbackRig rig(0);
    const int left = rig.arrive(StringName("ana"));
    const Ref<NetwPlayer> before
        = rig.server()->player_joined_of(rig.peer_id(left));
    REQUIRE(before.is_valid());

    rig.drop_client(left);
    rig.pump(4);
    const int back = rig.arrive(StringName("ana"));

    const Ref<NetwPlayer> after
        = rig.server()->player_joined_of(rig.peer_id(back));
    if (after.is_null() || after == before || !after->get_is_active()) {
        return law_broken("the returning client is not a new active player");
    }
    if (before->get_is_active()) {
        return law_broken("the handle from before still reports active");
    }
    return law_held();
}

int prepared(Node *p_node, const StringName &p_name) {
    return int(Netw::prepare_join(p_node, p_name, Array())->get_result());
}

LawVerdict prepare_join_door(const Stand &) {
    netw_test::FaultWatch faults;
    faults.expect_error("*Netw.prepare_join*[NW0031]*", 1);
    faults.expect_error("*Netw.prepare_join*[NW0019]*", 1);

    LoopbackRig rig(0);
    netw::MultiplayerViewport *seat = memnew(netw::MultiplayerViewport);
    seat->set_name("JoinOffline");
    netw::gd::scene_root()->add_child(seat);
    Node *inside = memnew(Node);
    seat->add_child(inside);
    Ref<NetwMultiplayer> api = seat->get_api();

    const int empty = prepared(inside, StringName());
    const int offline = prepared(inside, StringName("ana"));
    api->set("multiplayer_peer", rig.session()->create_client_peer());
    for (int round = 0; round < 8; ++round) {
        rig.pump();
        api->poll();
    }
    const Ref<NetwPlayer> seated = local_of(api.ptr());
    const int online = prepared(inside, StringName("bo"));
    const Ref<NetwPlayer> still = local_of(api.ptr());
    const StringName seated_as
        = seated.is_valid() ? seated->get_username() : StringName();
    Node *pawn = memnew(Node);
    const Ref<NetwEntity> drafted = Netw::configure_entity(pawn);
    drafted->set_player(seated);

    api->set("multiplayer_peer", Variant());
    LoopbackRig next(0);
    api->set("multiplayer_peer", next.session()->create_client_peer());
    for (int round = 0; round < 8; ++round) {
        next.pump();
        api->poll();
    }
    const Ref<NetwPlayer> rejoined = local_of(api.ptr());
    const bool aliased = seated.is_valid()
        && (seated->get_is_active() || seated->get_id() != 0
            || drafted->get_player().is_valid());

    memdelete(pawn);
    api->set("multiplayer_peer", Variant());
    netw::gd::scene_root()->remove_child(seat);
    memdelete(seat);
    api.unref();

    if (empty != ERR_INVALID_PARAMETER) {
        return law_broken("an empty name answered %d", empty);
    }
    if (offline != OK) {
        return law_broken("an offline session answered %d", offline);
    }
    if (seated_as != StringName("ana")) {
        return law_broken("the prepared join was not seated as ana");
    }
    if (rejoined.is_null() || aliased) {
        return law_broken(
            "the player from the first server still read as joined"
        );
    }
    if (seated->get_username() != StringName()) {
        return law_broken("the player still answered a name once freed");
    }
    if (online != ERR_ALREADY_IN_USE || still != seated) {
        return law_broken("an online session answered %d", online);
    }
    return law_held();
}

LawVerdict relay_becomes_server(const Stand &) {
    Ref<netw::LocalMultiplayerPeer> peer;
    peer.instantiate();
    REQUIRE(peer->create_client(5) == OK);
    Ref<NetwMultiplayer> session;
    session.instantiate();
    session->set("multiplayer_peer", peer);
    session->poll();
    const int waiting = int(session->session_get_state());

    REQUIRE(peer->create_server(1) == OK);
    session->poll();
    const int state = int(session->session_get_state());
    const int role = int(session->session_get_role());
    session->set("multiplayer_peer", Variant());

    if (waiting != NetwMultiplayer::SESSION_STATE_CONNECTING) {
        return law_broken("a client with no server read state %d", waiting);
    }
    if (state != NetwMultiplayer::SESSION_STATE_ONLINE
        || role != NetwMultiplayer::ROLE_LISTEN_SERVER) {
        return law_broken("peer 1 read state %d and role %d", state, role);
    }
    return law_held();
}

const Row ROWS[] = {
    {"two clients joining under one name",
     "each is seated as that name and the server holds both as active "
     "players, with no entity placed",
     &one_name_twice},
    {"a host joining itself and two clients that prepared no join",
     "the host is its own local player, each client holds an unnamed one, "
     "every peer lists all three by id, and the join handler ran for each",
     &nameless_join},
    {"a client joining with typed arguments and the server's handler",
     "the server's handler reads the values with their types, and the "
     "client's own handler never runs",
     &typed_arguments},
    {"a client whose join handler differs from the server's",
     "the client hears join_failed with ERR_UNCONFIGURED and nobody is "
     "seated",
     &differing_signature},
    {"a server handler taking an argument and a client carrying none",
     "the client hears join_failed with ERR_UNCONFIGURED, the handler never "
     "runs and nobody is seated",
     &missing_arguments},
    {"two live join handlers on one session, then one leaves",
     "a join under both is not seated, and the next join is seated by the "
     "one that stays",
     &rival_handlers},
    {"an admission handler that sets an account",
     "the server and the player itself read the account, and another client "
     "reads only the account name",
     &account_privacy},
    {"a client that leaves and joins again",
     "it is a new active player and the handle from before reports inactive",
     &rejoin_is_new},
    {"Netw.prepare_join with no name, offline and online",
     "no name and an online session are refused, and an offline one is "
     "seated under the prepared name, and that player reads inactive with "
     "id 0 after the client joins another server, an entity given it then "
     "reads no player, and it answers nothing once freed",
     &prepare_join_door},
    {"a relay peer that becomes peer 1 while connecting",
     "the session goes online as the listen server on the next poll",
     &relay_becomes_server},
};

TEST_CASE(
    "[Netw][Session] a joiner keeps its name, its arguments and its "
    "account, and a join the server cannot read is refused"
) {
    const Stand stand = {String("a loopback session")};
    for (const Row &row : ROWS) {
        NETW_CELL(row, stand);
        NETW_LAW_HOLDS(row, stand);
    }
}

} // namespace TestNetwJoin

#endif
