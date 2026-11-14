#include "support/internals.h"
#include "support/netw_test.h"

#include "godot/node.hpp"
#include "godot/object.hpp"
#include "godot/variant.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/predict_stats.hpp"
#include "netw/api/prediction.hpp"
#include "netw/predict/decl.hpp"

namespace TestNetwPredictStatsLifetimeLaws {

using namespace godot;
using netw::NetwEntity;
using netw::NetwMultiplayer;
using netw::NetwPredictionEngine;
using netw_test::Internals;

TEST_CASE(
    "[Networked][Predict][Hosted] PL1 a predicted entity the session "
    "despawns is freed once the game drops its node, because its stats "
    "never hold it alive"
) {
    Ref<NetwMultiplayer> core;
    core.instantiate();
    NetwPredictionEngine *pool = Internals::get_prediction_engine(core);

    Node *body = memnew(Node);
    body->set_name("Predicted");
    ObjectID wrapper;
    {
        const Ref<NetwEntity> entity = NetwEntity::ensure(body);
        REQUIRE(entity.is_valid());
        wrapper = entity->get_instance_id();
        REQUIRE(pool->slot_register(entity) >= 0);
        Internals::record(entity.ptr())->predict_declaration().facts.bind_slot(
            pool,
            entity
        );
        pool->slot_unregister(entity);
    }
    REQUIRE(netw::gd::object_registered(wrapper));

    memdelete(body);

    CHECK_FALSE(netw::gd::object_registered(wrapper));
}

} // namespace TestNetwPredictStatsLifetimeLaws
