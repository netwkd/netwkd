#include "support/netw_test.h"

#include "netw/scene_core.hpp"

namespace TestNetwSpawnArgSchemaLaws {

using netw::NetwSceneCore;

TEST_CASE(
    "[Netw][Scene][Hosted] SA4 one member of the isolation enum means a "
    "world of its own, so the viewport wrap and any policy that branches "
    "on isolation cannot disagree about which one it is"
) {
    CHECK(
        NetwSceneCore::isolation_owns_world(NetwSceneCore::ISOLATION_OWN_WORLD)
    );
    CHECK(!NetwSceneCore::isolation_owns_world(NetwSceneCore::ISOLATION_NONE));
    CHECK(!NetwSceneCore::isolation_owns_world(-1));
    CHECK(!NetwSceneCore::isolation_owns_world(7));
}

} // namespace TestNetwSpawnArgSchemaLaws
