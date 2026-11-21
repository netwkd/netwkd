#include "support/internals.h"
#include "support/netw_test.h"

#include "godot/multiplayer.hpp"
#include "godot/scene_tree.hpp"
#include "netw/api/clock_config.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/session/frames.hpp"
#include "netw/wire/registry.hpp"
#include "support/loopback_rig.h"

#if defined(NETW_TIER_HOSTED)

namespace TestNetwSessionPauseLaws {

using namespace godot;
using netw::NetwMultiplayer;
using netw_test::Internals;
using netw_test::LoopbackRig;

struct TreeUnpausedAfter {
    ~TreeUnpausedAfter() {
        SceneTree *tree = netw::gd::scene_tree();
        if (tree != nullptr) {
            tree->set_pause(false);
        }
    }
};

bool tree_paused() {
    SceneTree *tree = netw::gd::scene_tree();
    return tree != nullptr && tree->is_paused();
}

void server_says_paused(NetwMultiplayer *p_core, bool p_paused) {
    if (p_paused) {
        p_core->session_publish_control(
            netw::wire::CHANNEL_SESSION_PAUSE,
            MultiplayerPeer::TARGET_PEER_SERVER,
            netw::session::frame_write(netw::session::SessionReason{String()})
        );
        return;
    }
    p_core->session_publish_control(
        netw::wire::CHANNEL_SESSION_UNPAUSE,
        MultiplayerPeer::TARGET_PEER_SERVER,
        PackedByteArray()
    );
}

int64_t ticks_over_frames(NetwMultiplayer *p_core, int p_frames) {
    const int64_t before = Internals::clock_engine(p_core).get_ticks_run();
    for (int frame = 0; frame < p_frames; ++frame) {
        p_core->clock_on_physics_frame();
    }
    return Internals::clock_engine(p_core).get_ticks_run() - before;
}

TEST_CASE(
    "[Netw][Session] the tree stays paused while any session in the "
    "process is paused, and a pause the game set outlasts them all"
) {
    const TreeUnpausedAfter restore;
    Ref<NetwMultiplayer> first;
    first.instantiate();
    Ref<NetwMultiplayer> second;
    second.instantiate();

    server_says_paused(first.ptr(), true);
    server_says_paused(second.ptr(), true);
    server_says_paused(first.ptr(), false);
    NETW_CHECK_EQ(int(tree_paused()), 1);
    server_says_paused(second.ptr(), false);
    NETW_CHECK_EQ(int(tree_paused()), 0);

    netw::gd::scene_tree()->set_pause(true);
    server_says_paused(first.ptr(), true);
    server_says_paused(first.ptr(), false);
    NETW_CHECK_EQ(int(tree_paused()), 1);
}

TEST_CASE(
    "[Netw][Session] the clock keeps ticking through a SceneTree "
    "pause the game set, and stops for a session pause"
) {
    const TreeUnpausedAfter restore;
    LoopbackRig rig(1);
    rig.mount();
    Ref<netw::NetwClockConfig> clock;
    clock.instantiate();
    clock->set_tickrate(60);
    NETW_REQUIRE_EQ(rig.server()->clock_initialize(clock), OK);

    netw::gd::scene_tree()->set_pause(true);
    NETW_CHECK_EQ(int(ticks_over_frames(rig.server(), 4) > 0), 1);
    netw::gd::scene_tree()->set_pause(false);

    rig.server()->session_pause(String());
    NETW_CHECK_EQ(ticks_over_frames(rig.server(), 4), 0);
    rig.server()->session_unpause();
    NETW_CHECK_EQ(int(ticks_over_frames(rig.server(), 4) > 0), 1);
}

} // namespace TestNetwSessionPauseLaws

#endif
