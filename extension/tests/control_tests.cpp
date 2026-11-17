#include "support/netw_test.h"

#if defined(NETW_TIER_HOSTED)

#include "support/fault_log.h"
#include "support/internals.h"
#include "support/loopback_rig.h"
#include "support/minted_script.h"
#include "support/netw_cells.h"
#include "support/value_flow_stand.h"

#include "godot/node.hpp"
#include "godot/script.hpp"
#include "godot/variant.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/promise.hpp"

namespace TestControl {

using namespace godot;
using netw::NetwEntity;
using netw::NetwPromise;
using netw_test::FaultWatch;
using netw_test::Internals;
using netw_test::law_broken;
using netw_test::law_held;
using netw_test::LawVerdict;
using netw_test::LoopbackRig;

constexpr int TICKRATE = 30;
constexpr int OVERSIZED_NOTES = 1100;
constexpr const char *BOARD_ID = "control_board";

constexpr const char *BOARD_SOURCE = R"(extends Node2D

var strokes := 0

func _init() -> void:
	var e := Netw.configure_entity(self)
	e.authority = NetwEntity.AUTHORITY_CONTROLLER
	e.transfer = NetwEntity.TRANSFER_IMMEDIATE
	Netw.configure_property(self, &"strokes").broadcast().on_spawn()
)";

constexpr const char *NOTED_BOARD_SOURCE = R"(extends Node2D

var strokes := 0
var notes := PackedByteArray()

func _init() -> void:
	var e := Netw.configure_entity(self)
	e.authority = NetwEntity.AUTHORITY_CONTROLLER
	e.transfer = NetwEntity.TRANSFER_IMMEDIATE
	Netw.configure_property(self, &"strokes").broadcast().on_spawn()
	Netw.configure_property(self, &"notes").broadcast()
)";

Script *building = nullptr;
int64_t first_strokes = -1;

Node *build(const Variant &p_name) {
    if (building == nullptr) {
        return nullptr;
    }
    Node *made = Object::cast_to<Node>(building->call("new"));
    if (made != nullptr) {
        made->set_name(String(p_name));
        if (first_strokes >= 0) {
            made->set("strokes", first_strokes);
        }
    }
    return made;
}

struct Board {
    LoopbackRig rig;
    Ref<Script> script;
    int route = 0;

    explicit Board(const char *p_source, int64_t p_strokes = -1) : rig(2) {
        script = netw_test::minted_script(p_source);
        building = script.ptr();
        first_strokes = p_strokes;
        rig.mount();
        netw_test::flow_clocks(rig, TICKRATE);
        Node *arena = rig.mirror_child("Arena");
        Array types;
        types.push_back(int(Variant::STRING));
        Array args;
        args.push_back(String(BOARD_ID));
        route = rig.spawn_registered(
            StringName(BOARD_ID),
            callable_mp_static(&build),
            args,
            types,
            arena
        );
        rig.pump(4);
    }

    ~Board() {
        building = nullptr;
        first_strokes = -1;
    }

    Node *node(int p_client) const {
        return rig.route_node(route, p_client);
    }

    Ref<NetwEntity> entity(int p_client) const {
        Node *held = node(p_client);
        return held == nullptr ? Ref<NetwEntity>() : NetwEntity::of(held);
    }

    int64_t strokes(int p_client) const {
        Node *held = node(p_client);
        return held == nullptr ? -1 : int64_t(held->get("strokes"));
    }

    void draw(int p_client, int64_t p_strokes) const {
        Node *held = node(p_client);
        if (held != nullptr) {
            held->set("strokes", p_strokes);
        }
    }

    void author_with(int p_client, int64_t p_strokes) {
        entity(-1)->set_controller(rig.peer_id(p_client));
        rig.step_ticks(4);
        draw(p_client, p_strokes);
        rig.step_ticks(6);
    }

    int strokes_differ(int64_t p_strokes, int p_skip = -2) const {
        for (int client = -1; client < 2; ++client) {
            if (client != p_skip && strokes(client) != p_strokes) {
                return client;
            }
        }
        return -2;
    }

    int controller_differs(int64_t p_peer) const {
        for (int client = -1; client < 2; ++client) {
            const Ref<NetwEntity> held = entity(client);
            if (held.is_null() || held->get_controller() != p_peer
                || node(client)->get_multiplayer_authority() != int(p_peer)) {
                return client;
            }
        }
        return -2;
    }
};

void quiet(int64_t, const String &) {
}

struct Stand {
    String label;
};

typedef netw_test::LawRowFor<Stand> Row;

LawVerdict release_to_third(const Stand &) {
    Board board(BOARD_SOURCE);
    const int successor = board.rig.peer_id(1);
    board.author_with(0, 3);
    if (board.strokes(1) != 3) {
        return law_broken("client 1 never read the first strokes");
    }

    board.draw(0, 8);
    const Ref<NetwPromise> release
        = board.entity(0)->release_control(successor);
    if (release->get_is_settled()) {
        return law_broken("the release settled before anything was sent");
    }
    board.rig.step_ticks(8);
    if (!release->get_is_completed()) {
        return law_broken("the release did not complete");
    }
    const int behind = board.controller_differs(successor);
    if (behind != -2) {
        return law_broken("peer %d does not follow the successor", behind);
    }
    const int stale = board.strokes_differ(8);
    if (stale != -2) {
        return law_broken("peer %d lost the releaser's newest board", stale);
    }

    board.draw(1, 9);
    board.rig.step_ticks(8);
    const int unheard = board.strokes_differ(9);
    if (unheard != -2) {
        return law_broken("peer %d never read the successor's stroke", unheard);
    }
    return law_held();
}

LawVerdict release_to_stranger(const Stand &) {
    Board board(BOARD_SOURCE);
    const int author = board.rig.peer_id(0);
    board.author_with(0, 3);

    const Ref<NetwPromise> release = board.entity(0)->release_control(999);
    release->catch_error(callable_mp_static(&quiet));
    if (!release->get_is_failed()
        || release->get_code() != int64_t(ERR_UNAVAILABLE)) {
        return law_broken("the release did not fail with ERR_UNAVAILABLE");
    }
    board.rig.step_ticks(6);
    const int moved = board.controller_differs(author);
    if (moved != -2) {
        return law_broken("peer %d moved control off the releaser", moved);
    }
    return law_held();
}

LawVerdict release_past_budget(const Stand &) {
    FaultWatch faults;
    faults.expect_error("*byte array*[NW0032]*", 1);
    faults.expect_warning("*released authority*[NW1018]*", 1);

    Board board(NOTED_BOARD_SOURCE, 3);
    board.entity(-1)->set_controller(board.rig.peer_id(0));
    board.rig.step_ticks(4);
    if (board.strokes(-1) != 3) {
        return law_broken("the server never read the first strokes");
    }

    PackedByteArray notes;
    notes.resize(OVERSIZED_NOTES);
    board.node(0)->set("notes", notes);
    board.draw(0, 8);
    const Ref<NetwPromise> release = board.entity(0)->release_control();
    board.rig.step_ticks(8);
    if (!release->get_is_completed()) {
        return law_broken("the release did not complete");
    }
    const int behind = board.controller_differs(1);
    if (behind != -2) {
        return law_broken("peer %d does not hold the server", behind);
    }
    const int sent = board.strokes_differ(3, 0);
    if (sent != -2) {
        return law_broken("peer %d read the state past the budget", sent);
    }
    return law_held();
}

LawVerdict server_hands_back(const Stand &) {
    Board board(BOARD_SOURCE);
    const int author = board.rig.peer_id(0);

    board.entity(-1)->set_controller(author);
    board.rig.step_ticks(4);
    const int behind = board.controller_differs(author);
    if (behind != -2) {
        return law_broken("peer %d does not follow the client", behind);
    }

    board.entity(-1)->set_controller(1);
    board.rig.step_ticks(4);
    const int stuck = board.controller_differs(1);
    if (stuck != -2) {
        return law_broken("peer %d does not follow the server", stuck);
    }
    return law_held();
}

LawVerdict client_assigns(const Stand &) {
    Board board(BOARD_SOURCE);
    const int author = board.rig.peer_id(0);
    const RID at_server
        = Internals::entity_of(board.rig.server(), board.node(-1));
    const RID at_client
        = Internals::entity_of(board.rig.client(1), board.node(1));
    if (board.rig.server()->entity_set_controller(at_server, author) != OK) {
        return law_broken("the server could not grant control");
    }
    board.rig.step_ticks(4);

    const FaultWatch faults;
    board.entity(1)->set_controller(board.rig.peer_id(1));
    board.entity(0)->set_controller(1);
    const Error granted
        = board.rig.client(1)->entity_set_controller(at_client, 1);
    if (granted != ERR_UNAUTHORIZED) {
        return law_broken("a client grant answered %d", int(granted));
    }
    if (faults.errors() != 2) {
        return law_broken(
            "%d faults for two assigning clients",
            int(faults.errors())
        );
    }
    board.rig.step_ticks(6);
    const int moved = board.controller_differs(author);
    if (moved != -2) {
        return law_broken("peer %d moved control on a client's word", moved);
    }
    return law_held();
}

const Row ROWS[] = {
    {"a release to a third peer after the releaser drew again",
     "every peer follows the successor from the releaser's newest board, "
     "and then reads what the successor draws",
     &release_to_third},
    {"a release naming a peer that holds no copy",
     "it fails with ERR_UNAVAILABLE and control stays with the releaser",
     &release_to_stranger},
    {"a release whose final state is past the control budget",
     "the state is reported and not sent, and the server still takes "
     "control on every peer",
     &release_past_budget},
    {"the server sets the controller to a client and then back",
     "every peer follows each assignment",
     &server_hands_back},
    {"a client assigning the controller, by property and by "
     "entity_set_controller",
     "each assigning client is reported once, the grant answers "
     "ERR_UNAUTHORIZED, and control stays where the server put it",
     &client_assigns},
};

TEST_CASE(
    "[Netw][Control][SceneTree] control moves only on the server's "
    "word or a release, and a successor starts from the releaser's values"
) {
    const Stand stand = {String("a board on a loopback session")};
    for (const Row &row : ROWS) {
        NETW_CELL(row, stand);
        NETW_LAW_HOLDS(row, stand);
    }
}

} // namespace TestControl

#endif
