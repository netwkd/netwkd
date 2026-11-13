#include "support/loopback_rig.h"

#if defined(NETW_TIER_HOSTED)

#include "godot/node.hpp"
#include "netw/api/netw_multiplayer.hpp"

namespace TestSpawnVerbWireHosted {

using namespace godot;
using namespace netw_test;

Node *build_body(const Variant &) {
    Node *made = memnew(Node);
    made->set_name("Body");
    return made;
}

Array one_arg() {
    Array out;
    out.push_back(String("body"));
    return out;
}

Array one_type() {
    Array out;
    out.push_back(int(Variant::STRING));
    return out;
}

int64_t counter_of(LoopbackRig &p_rig, int p_client, const char *p_key) {
    return int64_t(
        p_rig.spawn_plane(p_client)->counters().get(StringName(p_key), 0)
    );
}

TEST_CASE(
    "[Networked][Spawn] VW5 a frame naming a life the receiver has already "
    "buried is refused, because an unreliable frame authored before a death "
    "can arrive after the reliable frame that opened the next life"
) {
    LoopbackRig rig(1);
    rig.mount();
    Node *arena = rig.mirror_child("Arena");

    const int route = rig.spawn_registered(
        StringName("vw_body"),
        callable_mp_static(&build_body),
        one_arg(),
        one_type(),
        arena,
        0,
        true
    );
    rig.pump(10);
    REQUIRE(rig.route_node(route, 0) != nullptr);

    const int64_t stale = counter_of(rig, 0, "drops_spawn_stale_life");
    rig.deliver_despawn(0, route, 3);
    rig.pump(5);

    rig.deliver_despawn(0, route, 1);
    NETW_CHECK_EQ(counter_of(rig, 0, "drops_spawn_stale_life"), stale + 1);
}

} // namespace TestSpawnVerbWireHosted

#endif
