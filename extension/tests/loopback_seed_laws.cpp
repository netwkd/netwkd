#include "support/loopback_rig.h"

#if defined(NETW_TIER_HOSTED)

namespace TestLoopbackSeed {

using namespace netw_test;

godot::Vector<int> ids_after_seed(int64_t p_seed) {
    LoopbackRig rig = LoopbackRig::seeded(p_seed, 3);
    godot::Vector<int> ids;
    for (int index = 0; index < rig.count(); ++index) {
        ids.push_back(rig.peer_id(index));
    }
    return ids;
}

TEST_CASE(
    "[Networked][Session] the seed alone decides every loopback client's "
    "peer id"
) {
    const godot::Vector<int> first = ids_after_seed(20260430);
    const godot::Vector<int> again = ids_after_seed(20260430);
    const godot::Vector<int> other = ids_after_seed(20260431);
    NETW_REQUIRE_EQ(first.size(), 3);
    NETW_REQUIRE_EQ(again.size(), 3);
    NETW_REQUIRE_EQ(other.size(), 3);
    bool differs = false;
    for (int index = 0; index < first.size(); ++index) {
        NETW_CHECK_EQ(again[index], first[index]);
        differs = differs || other[index] != first[index];
    }
    CHECK(differs);
}

} // namespace TestLoopbackSeed

#endif
