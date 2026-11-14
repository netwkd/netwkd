#include "support/fault_log.h"
#include "support/loopback_rig.h"
#include "support/netw_cells.h"
#include "support/netw_test.h"

#include "netw/api/prediction.hpp"
#include "netw/display/decl.hpp"
#include "netw/display/role_facts.hpp"
#include "netw/predict/decl.hpp"
#include "netw/predict/engine.hpp"
#include "netw/sim/resolve.hpp"

#if defined(NETW_TIER_HOSTED)

#include "support/scenario_run.h"
#include "support/value_flow_stand.h"

#include "godot/node.hpp"
#include "godot/spatial_node.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/simulation.hpp"

namespace TestNetwSimulation {

using namespace godot;
using namespace netw_test;
using netw::NetwEntity;
using netw::NetwMultiplayer;
using netw::NetwPrediction;
using netw::NetwPredictionEngine;
using netw::NetwSimulation;
using netw::sim::Facts;
using netw::sim::Mode;
using netw::sim::Replicas;

const int64_t SESSION_AUTHORITY = 1;
const int64_t NOBODY = 0;

enum Closure {
    OPEN,
    FALLBACK,
    DELAY_CLOSED,
    CLOSED,
    ANY_CLOSURE,
};

enum Selection {
    UNSELECTED,
    SELECTED,
    ANY_SELECTION,
};

enum Inputs {
    INPUTLESS,
    INPUT_ROWS,
};

struct Peers {
    int64_t local;
    int64_t controller;
};

const Peers SERVER_STEERS = {1, 0};
const Peers SERVER_HOSTS_A_CLIENT = {1, 2};
const Peers CLIENT_STEERS = {2, 2};
const Peers CLIENT_WATCHES_THE_SERVER = {2, 0};
const Peers CLIENT_WATCHES_A_CLIENT = {3, 2};

const Peers EVERY_PEERING[] = {
    SERVER_STEERS,
    SERVER_HOSTS_A_CLIENT,
    CLIENT_STEERS,
    CLIENT_WATCHES_THE_SERVER,
    CLIENT_WATCHES_A_CLIENT,
};

struct Cell {
    const char *label;
    Peers peers;
    Inputs inputs;
    Closure closure;
    Selection selection;
    Mode mode;
    NetwPrediction::InputSource source;
    NetwPrediction::Role role;
    netw::display::Role display_role;
};

const Cell PARITY[] = {
    {"a controller-0 entity on the server authors and draws authority",
     SERVER_STEERS,
     INPUT_ROWS,
     ANY_CLOSURE,
     ANY_SELECTION,
     Mode::AUTHORITY,
     NetwPrediction::INPUT_SOURCE_LOCAL,
     NetwPrediction::ROLE_HOST_LOCAL,
     netw::display::ROLE_AUTHORITY},
    {"an inputless controller-0 entity on the server",
     SERVER_STEERS,
     INPUTLESS,
     ANY_CLOSURE,
     ANY_SELECTION,
     Mode::AUTHORITY,
     NetwPrediction::INPUT_SOURCE_NONE,
     NetwPrediction::ROLE_HOST_LOCAL,
     netw::display::ROLE_AUTHORITY},
    {"the server consumes a client's commands",
     SERVER_HOSTS_A_CLIENT,
     INPUT_ROWS,
     ANY_CLOSURE,
     ANY_SELECTION,
     Mode::AUTHORITY,
     NetwPrediction::INPUT_SOURCE_RECEIVED,
     NetwPrediction::ROLE_CONSUME,
     netw::display::ROLE_AUTHORITY},
    {"the server runs an inputless client-controlled entity",
     SERVER_HOSTS_A_CLIENT,
     INPUTLESS,
     ANY_CLOSURE,
     ANY_SELECTION,
     Mode::AUTHORITY,
     NetwPrediction::INPUT_SOURCE_NONE,
     NetwPrediction::ROLE_HOST_LOCAL,
     netw::display::ROLE_AUTHORITY},
    {"a client predicts what it controls",
     CLIENT_STEERS,
     INPUT_ROWS,
     OPEN,
     ANY_SELECTION,
     Mode::PREDICT,
     NetwPrediction::INPUT_SOURCE_LOCAL,
     NetwPrediction::ROLE_PREDICT,
     netw::display::ROLE_PREDICTED},
    {"a closed controller stops predicting and keeps its local commands",
     CLIENT_STEERS,
     INPUT_ROWS,
     CLOSED,
     ANY_SELECTION,
     Mode::PROXY,
     NetwPrediction::INPUT_SOURCE_LOCAL,
     NetwPrediction::ROLE_REMOTE,
     netw::display::ROLE_REMOTE},
    {"an inputless entity is a proxy on its controller",
     CLIENT_STEERS,
     INPUTLESS,
     OPEN,
     UNSELECTED,
     Mode::PROXY,
     NetwPrediction::INPUT_SOURCE_NONE,
     NetwPrediction::ROLE_REMOTE,
     netw::display::ROLE_REMOTE},
    {"an inputless entity is active on its controller when selected",
     CLIENT_STEERS,
     INPUTLESS,
     OPEN,
     SELECTED,
     Mode::ACTIVE,
     NetwPrediction::INPUT_SOURCE_NONE,
     NetwPrediction::ROLE_SIMULATE,
     netw::display::ROLE_PREDICTED},
    {"a closed inputless entity on its controller",
     CLIENT_STEERS,
     INPUTLESS,
     CLOSED,
     ANY_SELECTION,
     Mode::PROXY,
     NetwPrediction::INPUT_SOURCE_NONE,
     NetwPrediction::ROLE_REMOTE,
     netw::display::ROLE_REMOTE},
    {"an unselected copy of the server's entity",
     CLIENT_WATCHES_THE_SERVER,
     INPUT_ROWS,
     ANY_CLOSURE,
     UNSELECTED,
     Mode::PROXY,
     NetwPrediction::INPUT_SOURCE_NONE,
     NetwPrediction::ROLE_REMOTE,
     netw::display::ROLE_REMOTE},
    {"a selected copy of the server's entity",
     CLIENT_WATCHES_THE_SERVER,
     INPUT_ROWS,
     OPEN,
     SELECTED,
     Mode::ACTIVE,
     NetwPrediction::INPUT_SOURCE_PREDICTED,
     NetwPrediction::ROLE_SIMULATE,
     netw::display::ROLE_PREDICTED},
    {"a closed selected copy of the server's entity stays a proxy",
     CLIENT_WATCHES_THE_SERVER,
     INPUT_ROWS,
     CLOSED,
     SELECTED,
     Mode::PROXY,
     NetwPrediction::INPUT_SOURCE_NONE,
     NetwPrediction::ROLE_REMOTE,
     netw::display::ROLE_REMOTE},
    {"an unselected inputless copy of the server's entity",
     CLIENT_WATCHES_THE_SERVER,
     INPUTLESS,
     OPEN,
     UNSELECTED,
     Mode::PROXY,
     NetwPrediction::INPUT_SOURCE_NONE,
     NetwPrediction::ROLE_REMOTE,
     netw::display::ROLE_REMOTE},
    {"a selected inputless copy of the server's entity runs live",
     CLIENT_WATCHES_THE_SERVER,
     INPUTLESS,
     OPEN,
     SELECTED,
     Mode::ACTIVE,
     NetwPrediction::INPUT_SOURCE_NONE,
     NetwPrediction::ROLE_SIMULATE,
     netw::display::ROLE_PREDICTED},
    {"a closed inputless copy of the server's entity",
     CLIENT_WATCHES_THE_SERVER,
     INPUTLESS,
     CLOSED,
     ANY_SELECTION,
     Mode::PROXY,
     NetwPrediction::INPUT_SOURCE_NONE,
     NetwPrediction::ROLE_REMOTE,
     netw::display::ROLE_REMOTE},
    {"an unselected copy of another client's entity",
     CLIENT_WATCHES_A_CLIENT,
     INPUT_ROWS,
     ANY_CLOSURE,
     UNSELECTED,
     Mode::PROXY,
     NetwPrediction::INPUT_SOURCE_NONE,
     NetwPrediction::ROLE_REMOTE,
     netw::display::ROLE_REMOTE},
    {"a selected copy of another client's entity",
     CLIENT_WATCHES_A_CLIENT,
     INPUT_ROWS,
     OPEN,
     SELECTED,
     Mode::ACTIVE,
     NetwPrediction::INPUT_SOURCE_PREDICTED,
     NetwPrediction::ROLE_SIMULATE,
     netw::display::ROLE_PREDICTED},
    {"a closed selected copy of another client's entity stays a proxy",
     CLIENT_WATCHES_A_CLIENT,
     INPUT_ROWS,
     CLOSED,
     SELECTED,
     Mode::PROXY,
     NetwPrediction::INPUT_SOURCE_NONE,
     NetwPrediction::ROLE_REMOTE,
     netw::display::ROLE_REMOTE},
    {"an unselected inputless copy of another client's entity",
     CLIENT_WATCHES_A_CLIENT,
     INPUTLESS,
     OPEN,
     UNSELECTED,
     Mode::PROXY,
     NetwPrediction::INPUT_SOURCE_NONE,
     NetwPrediction::ROLE_REMOTE,
     netw::display::ROLE_REMOTE},
    {"a selected inputless copy of another client's entity runs live",
     CLIENT_WATCHES_A_CLIENT,
     INPUTLESS,
     OPEN,
     SELECTED,
     Mode::ACTIVE,
     NetwPrediction::INPUT_SOURCE_NONE,
     NetwPrediction::ROLE_SIMULATE,
     netw::display::ROLE_PREDICTED},
    {"a closed inputless copy of another client's entity",
     CLIENT_WATCHES_A_CLIENT,
     INPUTLESS,
     CLOSED,
     ANY_SELECTION,
     Mode::PROXY,
     NetwPrediction::INPUT_SOURCE_NONE,
     NetwPrediction::ROLE_REMOTE,
     netw::display::ROLE_REMOTE},
};

bool closure_matches(Closure p_row, Closure p_concrete) {
    switch (p_row) {
        case ANY_CLOSURE:
            return true;
        case CLOSED:
            return p_concrete != OPEN;
        default:
            return p_row == p_concrete;
    }
}

bool selection_matches(Selection p_row, Selection p_concrete) {
    return p_row == ANY_SELECTION || p_row == p_concrete;
}

bool same_peers(const Peers &p_a, const Peers &p_b) {
    return p_a.local == p_b.local && p_a.controller == p_b.controller;
}

struct Concrete {
    Peers peers;
    Inputs inputs;
    Closure closure;
    Selection selection;
};

bool covers(const Cell &p_cell, const Concrete &p_point) {
    return same_peers(p_cell.peers, p_point.peers)
        && p_cell.inputs == p_point.inputs
        && closure_matches(p_cell.closure, p_point.closure)
        && selection_matches(p_cell.selection, p_point.selection);
}

struct Resolved {
    Mode mode = Mode::NONE;
    int source = -1;
    int role = -1;
    int display_role = -1;
};

Resolved resolve_through_prediction(const Concrete &p_point) {
    const bool authority = p_point.peers.local == SESSION_AUTHORITY;
    const bool controls = p_point.peers.controller != NOBODY
        && p_point.peers.controller == p_point.peers.local;
    const bool steers_as_authority
        = authority && p_point.peers.controller == NOBODY;

    NetwPredictionEngine pool;
    const int64_t slot = pool.open();
    netw::predict::Decl decl;
    pool.declare_axes(
        slot,
        authority,
        controls || steers_as_authority,
        p_point.inputs == INPUTLESS
    );
    pool.set_fallback_latched(slot, p_point.closure == FALLBACK);
    if (p_point.closure == DELAY_CLOSED) {
        decl.mode = NetwPrediction::MODE_DISABLED;
    }
    Facts facts = pool.resolution_facts(slot, &decl);
    facts.selection_count = p_point.selection == SELECTED ? 1 : 0;

    Resolved out;
    out.mode = netw::sim::resolve(facts);
    out.role = pool.apply_mode(slot, &decl, out.mode);
    out.source = decl.input_source;
    out.display_role = netw::display::role_for_mode(out.mode, true, controls);
    return out;
}

struct AuthorRow {
    const char *label;
    Facts facts;
    Mode mode;
};

Facts declared() {
    Facts facts;
    facts.declared = true;
    return facts;
}

Facts steered_here() {
    Facts facts = declared();
    facts.controller_here = true;
    return facts;
}

Facts steered_by_nobody_on_the_server() {
    Facts facts = declared();
    facts.controller_is_nobody = true;
    facts.server_here = true;
    return facts;
}

Facts state_rows_steered_here() {
    Facts facts = steered_here();
    facts.state_rows = true;
    return facts;
}

Facts state_rows_on_the_server() {
    Facts facts = declared();
    facts.state_rows = true;
    facts.server_here = true;
    return facts;
}

Facts claimed_here() {
    Facts facts = declared();
    facts.pending_claim_here = true;
    return facts;
}

Facts claimed_on_a_server_authored_entity() {
    Facts facts = claimed_here();
    facts.state_rows = true;
    return facts;
}

Facts claimed_on_a_free_body() {
    Facts facts = claimed_here();
    facts.controller_is_nobody = true;
    return facts;
}

Facts active_replicas() {
    Facts facts = declared();
    facts.replicas = Replicas::ACTIVE;
    return facts;
}

Facts active_replicas_closed() {
    Facts facts = active_replicas();
    facts.fallback_latched = true;
    return facts;
}

Facts undeclared_but_steered_here() {
    Facts facts;
    facts.controller_here = true;
    facts.server_here = true;
    return facts;
}

TEST_CASE(
    "[Networked][Sim] a body resolves one simulation mode for its role on "
    "every peer"
) {
    const Closure CLOSURES[] = {OPEN, FALLBACK, DELAY_CLOSED};
    const Selection SELECTIONS[] = {UNSELECTED, SELECTED};
    const Inputs INPUTS[] = {INPUTLESS, INPUT_ROWS};
    int visited = 0;
    for (const Peers &peers : EVERY_PEERING) {
        for (const Inputs inputs : INPUTS) {
            for (const Closure closure : CLOSURES) {
                for (const Selection selection : SELECTIONS) {
                    const Concrete point = {peers, inputs, closure, selection};
                    const Cell *matched = nullptr;
                    int matches = 0;
                    for (const Cell &cell : PARITY) {
                        if (covers(cell, point)) {
                            matched = &cell;
                            matches += 1;
                        }
                    }
                    NETW_FORMAT_INT(local_text, peers.local);
                    NETW_FORMAT_INT(controller_text, peers.controller);
                    NETW_FORMAT_INT(inputs_text, int(inputs));
                    NETW_FORMAT_INT(closure_text, int(closure));
                    NETW_FORMAT_INT(selection_text, int(selection));
                    CAPTURE(local_text);
                    CAPTURE(controller_text);
                    CAPTURE(inputs_text);
                    CAPTURE(closure_text);
                    CAPTURE(selection_text);
                    NETW_REQUIRE_EQ(matches, 1);
                    NETW_FORMAT_TEXT(label_text, matched->label);
                    CAPTURE(label_text);
                    const Resolved read = resolve_through_prediction(point);
                    NETW_CHECK_EQ(int(read.mode), int(matched->mode));
                    NETW_CHECK_EQ(read.source, int(matched->source));
                    NETW_CHECK_EQ(read.role, int(matched->role));
                    NETW_CHECK_EQ(
                        read.display_role,
                        int(matched->display_role)
                    );
                    visited += 1;
                }
            }
        }
    }
    NETW_CHECK_EQ(visited, 60);

    const AuthorRow AUTHORS[] = {
        {"nothing declared is NONE whoever controls it",
         undeclared_but_steered_here(),
         Mode::NONE},
        {"a controller-authored entity runs on its controller",
         steered_here(),
         Mode::AUTHORITY},
        {"and is a proxy elsewhere", declared(), Mode::PROXY},
        {"controller 0 hands authorship to the session authority",
         steered_by_nobody_on_the_server(),
         Mode::AUTHORITY},
        {"a state row takes authorship from the controller",
         state_rows_steered_here(),
         Mode::PROXY},
        {"and gives it to the session authority",
         state_rows_on_the_server(),
         Mode::AUTHORITY},
        {"a pending claim runs a controller-authored entity",
         claimed_here(),
         Mode::AUTHORITY},
        {"a pending claim never runs a session-authored entity",
         claimed_on_a_server_authored_entity(),
         Mode::PROXY},
        {"a pending claim runs a free body the session was authoring",
         claimed_on_a_free_body(),
         Mode::AUTHORITY},
        {"active replicas run every copy", active_replicas(), Mode::ACTIVE},
        {"a closed active replica is a proxy",
         active_replicas_closed(),
         Mode::PROXY},
    };
    for (const AuthorRow &row : AUTHORS) {
        NETW_FORMAT_TEXT(author_text, row.label);
        CAPTURE(author_text);
        NETW_CHECK_EQ(int(netw::sim::resolve(row.facts)), int(row.mode));
    }
}

struct Stand {
    String label;
};

typedef LawRowFor<Stand> Row;

constexpr const char *CRATE = R"(extends Node2D

var crate_position: Vector2
var steps := 0
var fresh_steps := 0

func _init() -> void:
	Netw.configure_property(self, &"crate_position").broadcast()

func _network_process(_delta: float, _tick: int, fresh: bool) -> void:
	steps += 1
	if fresh:
		fresh_steps += 1
)";

int64_t steps_of(Node2D *p_node) {
    return int64_t(p_node->get(StringName("steps")));
}

Ref<NetwSimulation> simulation_of(Node2D *p_node) {
    const Ref<NetwEntity> entity = NetwEntity::of(p_node);
    REQUIRE(entity.is_valid());
    return entity->get_simulation();
}

struct Selecting {
    LoopbackRig rig;
    FlowPair subject;
    FlowPair member;

    Selecting() : rig(1) {
        rig.mount();
        flow_clocks(rig, 30, 3);
        subject = stand_flow_pair(rig, CRATE, "Subject");
        member = stand_flow_pair(rig, CRATE, "Member");
        steer(subject, rig.peer_id(0));
        rig.pump(2);
    }
};

int64_t steps_over(LoopbackRig &p_rig, Node2D *p_node, int p_ticks) {
    const int64_t before = steps_of(p_node);
    p_rig.step_ticks(p_ticks);
    return steps_of(p_node) - before;
}

LawVerdict selected_until_forgotten(const Stand &) {
    Selecting world;
    Node2D *copy = world.member.mirror(0);
    const Ref<NetwSimulation> chooser = simulation_of(world.subject.mirror(0));
    if (steps_over(world.rig, copy, 3) != 0) {
        return law_broken("an unselected copy stepped");
    }

    chooser->simulate(NetwEntity::of(copy));
    world.rig.step_ticks(1);
    const int64_t selected = steps_over(world.rig, copy, 5);
    if (selected != 5) {
        return law_broken(
            "the selected copy stepped %lld times in 5 ticks",
            (long long)selected
        );
    }
    if (int64_t(copy->get(StringName("fresh_steps"))) != 0) {
        return law_broken("the selected copy stepped as fresh");
    }

    chooser->forget(NetwEntity::of(copy));
    world.rig.step_ticks(1);
    const int64_t forgotten = steps_over(world.rig, copy, 5);
    if (forgotten != 0) {
        return law_broken(
            "a forgotten copy stepped %lld times",
            (long long)forgotten
        );
    }
    return law_held();
}

LawVerdict subject_stops_authoring(const Stand &) {
    Selecting world;
    Node2D *chooser_copy = world.subject.mirror(0);
    Node2D *copy = world.member.mirror(0);
    simulation_of(chooser_copy)->simulate(NetwEntity::of(copy));
    world.rig.step_ticks(2);
    if (steps_over(world.rig, copy, 2) != 2) {
        return law_broken("the selected copy did not step");
    }

    steer(world.subject, 0);
    world.rig.step_ticks(1);
    const int64_t released = steps_over(world.rig, copy, 3);
    if (released != 0) {
        return law_broken(
            "a copy stepped %lld times after its subject stopped authoring",
            (long long)released
        );
    }

    simulation_of(copy)->simulate(NetwEntity::of(chooser_copy));
    world.rig.step_ticks(1);
    const int64_t chosen = steps_over(world.rig, chooser_copy, 3);
    if (chosen != 0) {
        return law_broken(
            "a proxy's selection stepped %lld times",
            (long long)chosen
        );
    }
    return law_held();
}

LawVerdict another_scene(const Stand &) {
    FaultWatch expected_faults;
    expected_faults.expect_error("*NetwSimulation.simulate*[NW0032]*", 1);

    Scenario scenario;
    scenario.label = "two-scenes";
    scenario.world.scene("Arena")
        .scene("Annex")
        .entity(
            EntityDecl().named("Owner").on_route(91).placed_at(Vector2()),
            "Arena"
        )
        .entity(
            EntityDecl().named("Stranger").on_route(92).placed_at(Vector2()),
            "Annex"
        );
    scenario.until(2);
    LoopbackRig rig(scenario.clients);
    const ScenarioRun run = ScenarioRun::scenes(rig, scenario);
    REQUIRE(run.regime_reached());

    NetwMultiplayer *core = rig.server();
    const Ref<NetwEntity> owner = core->entity_get_view(rig.entity_of("Owner"));
    const Ref<NetwEntity> stranger
        = core->entity_get_view(rig.entity_of("Stranger"));
    REQUIRE(owner.is_valid());
    REQUIRE(stranger.is_valid());

    owner->get_simulation()->simulate(stranger);
    if (owner->get_simulation()->get_selected().size() != 0) {
        return law_broken("the other scene's entity was selected");
    }
    return law_held();
}

const Row ROWS[] = {
    {"a subject selects an inputless copy, then forgets it",
     "the copy steps once a tick, never as fresh, and stops when forgotten",
     &selected_until_forgotten},
    {"a subject that stops authoring, then a proxy that selects",
     "the selected copy stops stepping and the proxy's choice never steps",
     &subject_stops_authoring},
    {"a subject selecting an entity in another scene",
     "it is refused with one error and selects nothing",
     &another_scene},
};

TEST_CASE(
    "[Networked][Sim] a selected body runs while its subject "
    "holds it"
) {
    const Stand stand = {String("a client holding two crates")};
    for (const Row &row : ROWS) {
        NETW_CELL(row, stand);
        NETW_LAW_HOLDS(row, stand);
    }
}

} // namespace TestNetwSimulation

#endif
