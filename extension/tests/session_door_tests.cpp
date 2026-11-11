#include "support/fault_log.h"
#include "support/minted_script.h"
#include "support/netw_cells.h"
#include "support/netw_test.h"

#if defined(NETW_TIER_HOSTED)

#include "godot/multiplayer.hpp"
#include "godot/node.hpp"
#include "godot/scene_tree.hpp"
#include "netw/api/clock.hpp"
#include "netw/api/clock_config.hpp"
#include "netw/api/context.hpp"
#include "netw/api/loopback.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/nodes/multiplayer_viewport.hpp"
#include "netw/api/promise.hpp"
#include "netw/api/session.hpp"
#include "netw/api/session_config.hpp"
#include "netw/api/simulation.hpp"

namespace TestNetwSessionDoor {

using namespace godot;
using netw::MultiplayerViewport;
using netw::Netw;
using netw::NetwClock;
using netw::NetwPromise;
using netw::NetwSession;
using netw::NetwSessionConfig;
using netw_test::FaultWatch;
using netw_test::law_broken;
using netw_test::law_held;
using netw_test::LawVerdict;

constexpr const char *HANDLERS = R"(extends RefCounted

var calls := 0

func none() -> void:
	calls += 1

func one(_value) -> void:
	calls += 1
)";

struct Stand {
    String label;
};

typedef netw_test::LawRowFor<Stand> Row;

struct Mounted {
    MultiplayerViewport *tree = memnew(MultiplayerViewport);
    Node *probe = memnew(Node);

    explicit Mounted(const char *p_name) {
        tree->set_name(StringName(p_name));
        probe->set_name(StringName("Probe"));
        tree->add_child(probe);
        netw::gd::scene_root()->add_child(tree);
    }

    ~Mounted() {
        assign(Ref<MultiplayerPeer>());
        tree->get_parent()->remove_child(tree);
        memdelete(tree);
    }

    Node *child(const char *p_name) const {
        Node *node = memnew(Node);
        node->set_name(StringName(p_name));
        tree->add_child(node);
        return node;
    }

    Ref<MultiplayerAPI> api() const {
        return tree->get_api();
    }

    Ref<MultiplayerPeer> peer() const {
        return api()->get_multiplayer_peer();
    }

    void assign(const Ref<MultiplayerPeer> &p_peer) const {
        api()->set_multiplayer_peer(p_peer);
    }

    Ref<NetwSession> session() const {
        return Netw::session(probe);
    }

    StringName app_id() const {
        const Ref<NetwSessionConfig> config = session()->get_config();
        return config.is_valid() ? config->get_app_id() : StringName();
    }
};

Ref<MultiplayerPeer> a_server() {
    Ref<netw::LocalMultiplayerPeer> made;
    made.instantiate();
    REQUIRE(made->create_server() == OK);
    return made;
}

Ref<RefCounted> handlers() {
    const Ref<Script> shape = netw_test::minted_script(HANDLERS);
    REQUIRE(shape.is_valid());
    const Ref<RefCounted> made = shape->call("new");
    REQUIRE(made.is_valid());
    return made;
}

int calls_on(const Ref<RefCounted> &p_on) {
    return int(p_on->get(StringName("calls")));
}

LawVerdict outlives_session(const Stand &) {
    Ref<NetwSession> door;
    Ref<NetwClock> clock;
    {
        Mounted branch("DoorOutlives");
        door = branch.session();
        if (door.is_null()) {
            return law_broken("a mounted viewport answered no session");
        }
        clock = door->get_clock();
    }

    if (int64_t(door->get_role()) != netw::NetwMultiplayer::ROLE_NONE
        || door->get_is_online() || door->get_is_local_client()
        || door->get_root() != nullptr || door->get_config().is_valid()
        || door->get_local_player().is_valid()
        || door->get_players().size() != 0 || door->get_scenes().size() != 0
        || door->player_of(1).is_valid()) {
        return law_broken("a session that is gone still answered a value");
    }
    const Ref<NetwPromise> left = door->leave();
    if (left.is_null()) {
        return law_broken("leave on a session that is gone answered nothing");
    }
    (void)left->get_is_failed();
    if (clock.is_null() || clock->get_tick() != 0
        || clock->get_is_synchronized() || clock->get_is_configured()) {
        return law_broken("a clock whose session is gone still ticks");
    }
    clock->set_display_offset(clock->get_display_offset() + 3);
    return law_held();
}

LawVerdict off_tree_session(const Stand &) {
    Node *orphan = memnew(Node);
    const bool orphan_answered = Netw::session(orphan).is_valid();
    memdelete(orphan);
    if (orphan_answered || Netw::session(nullptr).is_valid()) {
        return law_broken("a node outside the tree reached a session");
    }
    return law_held();
}

LawVerdict settings_before_peer(const Stand &) {
    Mounted branch("DoorSettings");
    const Ref<NetwSessionConfig> draft
        = Netw::configure_session(branch.child("Session"));
    draft->app(StringName("still-authoring"));
    const Ref<MultiplayerPeer> offered = a_server();
    const FaultWatch watch;

    branch.assign(offered);
    if (branch.peer() != offered
        || branch.app_id() != StringName("still-authoring")) {
        return law_broken("the declared app id was not applied in time");
    }

    branch.assign(offered);
    if (branch.peer() != offered
        || branch.app_id() != StringName("still-authoring")
        || watch.log->warnings_logged() != 0
        || watch.log->errors_logged() != 0) {
        return law_broken("a repeated assignment changed the session");
    }

    draft->app(StringName("too-late"));
    if (watch.log->error_count_containing("[NW0019]") != 1
        || branch.app_id() != StringName("still-authoring")) {
        return law_broken("an edit after the peer was applied silently");
    }
    return law_held();
}

LawVerdict clock_from_constructor(const Stand &) {
    Mounted branch("DoorClock");
    Node *declaring = memnew(Node);
    declaring->set_name(StringName("Game"));
    Netw::configure_clock(declaring)->ticks_per_second(60);

    branch.tree->add_child(declaring);
    branch.assign(a_server());

    const Ref<NetwClock> clock = branch.session()->get_clock();
    if (!clock->get_is_configured() || clock->get_tickrate() != 60) {
        return law_broken("the clock ticks at %d", int(clock->get_tickrate()));
    }
    return law_held();
}

LawVerdict rival_settings(const Stand &) {
    Mounted branch("DoorRivals");
    const Ref<MultiplayerPeer> standing = a_server();
    branch.assign(standing);
    const int64_t state = int64_t(branch.session()->get_state());

    FaultWatch watch;
    watch.expect_error("*[NW0034]*", 1);
    const Ref<NetwSessionConfig> kept
        = Netw::configure_session(branch.child("First"));
    kept->app(StringName("first-app"));
    Node *rival = branch.child("Second");
    Netw::configure_session(rival)->app(StringName("other-app"));

    const Ref<MultiplayerPeer> offered = a_server();
    branch.assign(offered);
    branch.assign(offered);
    if (branch.peer() != standing
        || int64_t(branch.session()->get_state()) != state) {
        return law_broken("rival settings let a new peer replace the old");
    }

    kept->app(StringName("fixed-app"));
    branch.tree->remove_child(rival);
    memdelete(rival);
    branch.assign(offered);
    if (branch.peer() != offered
        || branch.app_id() != StringName("fixed-app")) {
        return law_broken("the retry after the fix did not apply settings");
    }
    return law_held();
}

LawVerdict then_without_parameter(const Stand &) {
    FaultWatch watch;
    watch.expect_error("*The NetwPromise.then callback*[NW1002]*", 1);
    const Ref<RefCounted> on = handlers();
    const Callable none(on.ptr(), StringName("none"));

    NetwPromise::resolved(Variant(7))->then(none);
    Ref<NetwPromise> later;
    later.instantiate();
    later->then(none);
    later->resolve(Variant(9));

    if (calls_on(on) != 0) {
        return law_broken("a callback that takes no result ran");
    }
    return law_held();
}

LawVerdict catch_without_parameters(const Stand &) {
    FaultWatch watch;
    watch.expect_error("*The NetwPromise.catch_error callback*[NW1002]*", 1);
    const Ref<RefCounted> on = handlers();
    Ref<NetwPromise> pending;
    pending.instantiate();
    pending->catch_error(Callable(on.ptr(), StringName("none")));

    pending->reject(ERR_TIMEOUT, String("late"));

    if (calls_on(on) != 0) {
        return law_broken("a callback that takes no code ran");
    }
    return law_held();
}

LawVerdict settled_with_parameter(const Stand &) {
    FaultWatch watch;
    watch.expect_error("*The NetwPromise.when_settled callback*[NW1002]*", 1);
    const Ref<RefCounted> on = handlers();

    NetwPromise::resolved(Variant(7))
        ->when_settled(Callable(on.ptr(), StringName("one")));

    if (calls_on(on) != 0) {
        return law_broken("a callback that wants an argument ran");
    }
    return law_held();
}

LawVerdict step_with_wrong_parameters(const Stand &) {
    FaultWatch watch;
    watch.expect_error("*NetwSimulation.step*[NW1002]*", 1);
    const Ref<RefCounted> on = handlers();
    Ref<netw::NetwSimulation> simulation;
    simulation.instantiate();

    simulation->set_step(Callable(on.ptr(), StringName("none")));
    return law_held();
}

const Row ROWS[] = {
    {"a session handle and its clock kept after the session is freed",
     "every read answers empty, leave answers a promise, and the clock "
     "reads stopped",
     &outlives_session},
    {"Netw.session on a node outside the tree and on null",
     "both answer no session",
     &off_tree_session},
    {"Netw.configure_session on a node before a peer is assigned",
     "the settings apply before the assignment returns, a repeat changes "
     "nothing, and a later edit reports NW0019",
     &settings_before_peer},
    {"Netw.configure_clock in a constructor, then the node enters",
     "the first peer assignment ticks at the declared rate",
     &clock_from_constructor},
    {"two nodes declaring the session, then one leaves",
     "a new peer is refused with NW0034 and the standing one stays, and "
     "the retry applies the remaining settings",
     &rival_settings},
    {"a then callback that takes no result, on two promises",
     "it never runs and one NW1002 names NetwPromise.then",
     &then_without_parameter},
    {"a catch_error callback that takes no code or detail",
     "it never runs and one NW1002 names NetwPromise.catch_error",
     &catch_without_parameters},
    {"a when_settled callback that takes an argument",
     "it never runs and one NW1002 names NetwPromise.when_settled",
     &settled_with_parameter},
    {"a simulation step with the wrong parameters",
     "one NW1002 names NetwSimulation.step when the step is set",
     &step_with_wrong_parameters},
};

TEST_CASE(
    "[Networked][Session][SceneTree] Netw.session answers the live session "
    "or nothing, settings declared before the peer apply, and a handler "
    "with the wrong signature is reported"
) {
    const Stand stand = {String("a MultiplayerViewport session")};
    for (const Row &row : ROWS) {
        NETW_CELL(row, stand);
        NETW_LAW_HOLDS(row, stand);
    }
}

} // namespace TestNetwSessionDoor

#endif
