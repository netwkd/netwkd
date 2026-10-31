#include "support/fault_log.h"
#include "support/loopback_rig.h"

#if defined(NETW_TIER_HOSTED)

#include "godot/node.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "support/netw_call_log.h"

namespace TestConnectOnceLaws {

using namespace godot;
using namespace netw_test;
using netw::NetwMultiplayer;

TEST_CASE(
    "[Networked][Session] CO1 connecting an edge twice leaves one "
    "connection, so a caller may arm without asking whether it already did"
) {
    LoopbackRig rig(0);
    NetwMultiplayer *core = rig.server();
    REQUIRE(core != nullptr);

    Node *source = memnew(Node);
    netw::gd::add_signal(source, StringName("armed"), 1);
    const CallLog seen;

    const Signal edge(source, StringName("armed"));
    const Callable arm = seen.callable("armed");
    NETW_CHECK_EQ(int(core->connect_once(edge, arm)), 1);
    NETW_CHECK_EQ(int(core->connect_once(edge, arm)), 1);

    source->emit_signal(StringName("armed"), 7);

    NETW_CHECK_EQ(seen.count("armed"), 1);

    memdelete(source);
}

} // namespace TestConnectOnceLaws

#endif
