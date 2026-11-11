#include "support/fault_log.h"
#include "support/frame_drive.h"
#include "support/netw_cells.h"
#include "support/netw_test.h"

#include "godot/multiplayer.hpp"
#include "godot/node.hpp"
#include "godot/scene_tree.hpp"
#include "godot/viewport.hpp"
#include "godot/world.hpp"
#include "netw/api/context.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/nodes/multiplayer_viewport.hpp"

namespace TestNetwMultiplayerViewport {

using namespace godot;
using netw::MultiplayerViewport;
using netw::Netw;
using netw::NetwMultiplayer;
using netw_test::law_broken;
using netw_test::law_held;
using netw_test::LawVerdict;

struct Stand {
    String label;
};

typedef netw_test::LawRowFor<Stand> Row;

struct Mounted {
    MultiplayerViewport *seat = memnew(MultiplayerViewport);
    Node *child = memnew(Node);

    explicit Mounted(const char *p_name) {
        seat->set_name(p_name);
        seat->add_child(child);
        netw::gd::scene_root()->add_child(seat);
    }

    ~Mounted() {
        if (seat->get_parent() != nullptr) {
            seat->get_parent()->remove_child(seat);
        }
        memdelete(seat);
    }

    bool answers(Node *p_node) const {
        return Netw::of(p_node) == seat->get_api();
    }
};

MultiplayerAPI *mounted_at(const NodePath &p_path) {
    return netw::gd::scene_tree()->get_multiplayer(p_path).ptr();
}

MultiplayerAPI *root_session() {
    return netw::gd::scene_tree()->get_multiplayer().ptr();
}

LawVerdict two_viewports(const Stand &) {
    const Mounted first("ViewportFirst");
    const Mounted second("ViewportSecond");

    if (first.seat->get_api() == second.seat->get_api()) {
        return law_broken("both viewports hold one session");
    }
    if (!first.answers(first.child) || !second.answers(second.child)) {
        return law_broken("a node does not answer its own viewport's session");
    }
    if (first.child->get_viewport() != first.seat) {
        return law_broken("a child does not live in its viewport");
    }
    return law_held();
}

LawVerdict api_before_entering(const Stand &) {
    netw_test::FaultWatch faults;
    faults.expect_error("*MultiplayerViewport.api*[NW0036]*", 1);

    MultiplayerViewport *seat = memnew(MultiplayerViewport);
    seat->set_name("ViewportChosen");
    Ref<NetwMultiplayer> chosen;
    chosen.instantiate();
    seat->set_api(chosen);
    netw::gd::scene_root()->add_child(seat);
    const bool mounted = mounted_at(seat->get_path()) == chosen.ptr();

    Ref<NetwMultiplayer> late;
    late.instantiate();
    seat->set_api(late);
    const bool kept = seat->get_api() == chosen
        && mounted_at(seat->get_path()) == chosen.ptr();

    netw::gd::scene_root()->remove_child(seat);
    memdelete(seat);

    if (!mounted) {
        return law_broken("the chosen api is not mounted on the branch");
    }
    if (!kept) {
        return law_broken("an api set inside the tree replaced the first");
    }
    return law_held();
}

LawVerdict renamed_then_removed(const Stand &) {
    Mounted branch("ViewportNamed");
    const NodePath named = branch.seat->get_path();

    branch.seat->set_name("ViewportRenamed");
    const NodePath renamed = branch.seat->get_path();
    if (mounted_at(named) != root_session()) {
        return law_broken("the old path still answers the viewport");
    }
    if (mounted_at(branch.child->get_path()) != branch.seat->get_api().ptr()
        || !branch.answers(branch.child)) {
        return law_broken("a child stopped answering the renamed viewport");
    }

    netw::gd::scene_root()->remove_child(branch.seat);
    if (mounted_at(renamed) != root_session()) {
        return law_broken("a removed viewport's path still answers it");
    }
    return law_held();
}

LawVerdict own_worlds(const Stand &) {
    const Mounted branch("ViewportWorlds");
    Window *window = netw::gd::scene_tree()->get_root();
    REQUIRE(window != nullptr);

    const Ref<World2D> own_2d = branch.seat->find_world_2d();
    const Ref<World3D> own_3d = branch.seat->find_world_3d();
    if (own_2d.is_null() || own_2d == window->find_world_2d()) {
        return law_broken("the viewport shares the window's 2D world");
    }
    if (own_3d.is_null() || own_3d == window->find_world_3d()) {
        return law_broken("the viewport shares the window's 3D world");
    }
    return law_held();
}

const Row ROWS[] = {
    {"two viewports in one tree",
     "they hold two sessions, and a node answers the viewport it sits in",
     &two_viewports},
    {"an api set before the viewport enters, then one set inside the tree",
     "the first is the session of the branch and the second is refused",
     &api_before_entering},
    {"a viewport renamed, then removed from the tree",
     "a child answers the viewport at its new path, and the old path and the "
     "removed one answer the root session",
     &renamed_then_removed},
    {"a viewport beside the window",
     "it owns a 2D and a 3D world apart from the window's",
     &own_worlds},
};

TEST_CASE(
    "[Networked][Session][SceneTree] a MultiplayerViewport holds its own "
    "session for the branch under it"
) {
    const Stand stand = {String("a mounted viewport")};
    for (const Row &row : ROWS) {
        NETW_CELL(row, stand);
        NETW_LAW_HOLDS(row, stand);
    }
}

constexpr int POLL_FRAMES = 30;

struct PollEvidence {
    bool driven = false;
    int first = 0;
    int second = 0;
};

PollEvidence &poll_evidence() {
    static PollEvidence evidence;
    return evidence;
}

class PollScenario final : public netw_test::FrameScenario {
    int step = 0;
    MultiplayerViewport *first = nullptr;
    MultiplayerViewport *second = nullptr;
    int first_before = 0;
    int second_before = 0;

    static int polls(MultiplayerViewport *p_seat) {
        return int(p_seat->get_api()->clock_get_monitor(
            NetwMultiplayer::CLOCK_MONITOR_POLLS
        ));
    }

    static MultiplayerViewport *mounted(const char *p_name) {
        MultiplayerViewport *seat = memnew(MultiplayerViewport);
        seat->set_name(p_name);
        netw::gd::scene_root()->add_child(seat);
        return seat;
    }

    static void unmounted(MultiplayerViewport *p_seat) {
        netw::gd::scene_root()->remove_child(p_seat);
        memdelete(p_seat);
    }

public:
    bool advance() override {
        if (netw::gd::scene_root() == nullptr) {
            return false;
        }
        if (step == 0) {
            first = mounted("ViewportPolledFirst");
            second = mounted("ViewportPolledSecond");
            first_before = polls(first);
            second_before = polls(second);
        }
        if (step < POLL_FRAMES) {
            ++step;
            return true;
        }
        PollEvidence &evidence = poll_evidence();
        evidence.first = polls(first) - first_before;
        evidence.second = polls(second) - second_before;
        evidence.driven = true;
        unmounted(first);
        unmounted(second);
        return false;
    }
};

NETW_FRAME_SCENARIO(PollScenario, poll_scenario);

TEST_CASE(
    "[Networked][Session][Frame] each of two MultiplayerViewport sessions is "
    "polled once per frame"
) {
    const PollEvidence &evidence = poll_evidence();
    REQUIRE(evidence.driven);

    NETW_CHECK_GE(evidence.first, POLL_FRAMES - 2);
    NETW_CHECK_LE(evidence.first, POLL_FRAMES + 2);
    NETW_CHECK_GE(evidence.second, POLL_FRAMES - 2);
    NETW_CHECK_LE(evidence.second, POLL_FRAMES + 2);
}

} // namespace TestNetwMultiplayerViewport
