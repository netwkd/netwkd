#include "support/internals.h"
#include "support/netw_test.h"

#include "godot/node.hpp"
#include "godot/object.hpp"
#include "godot/variant.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/predict_stats.hpp"
#include "netw/api/prediction.hpp"
#include "netw/entity/ids.hpp"
#include "netw/predict/decl.hpp"

namespace TestNetwPredictStatsLifetimeLaws {

using namespace godot;
using netw::NetwEntity;
using netw::NetwMultiplayer;
using netw::NetwPrediction;
using netw::NetwPredictionEngine;
using netw_test::Internals;

TEST_CASE(
    "[Netw][Predict][Hosted] PL1 a predicted entity the session "
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
        REQUIRE(Internals::seat(pool, entity) >= 0);
        Internals::record(entity.ptr())
            ->predict_declaration()
            .facts.bind_slot(pool, entity);
        Internals::unseat(pool, entity);
    }
    REQUIRE(netw::gd::object_registered(wrapper));

    memdelete(body);

    CHECK_FALSE(netw::gd::object_registered(wrapper));
}

TEST_CASE(
    "[Netw][Predict][Hosted] PL2 an island member stays listed while its "
    "entity is alive, whether or not the session holds its record"
) {
    Ref<NetwMultiplayer> core;
    core.instantiate();
    NetwPredictionEngine *pool = Internals::get_prediction_engine(core);

    Node *seated = memnew(Node);
    Node *joined = memnew(Node);
    {
        const Ref<NetwEntity> subject = NetwEntity::ensure(seated);
        const Ref<NetwEntity> member = NetwEntity::ensure(joined);
        REQUIRE(subject.is_valid());
        REQUIRE(member.is_valid());
        member->set_name("Crate");
        const int64_t slot = Internals::seat(pool, subject);
        REQUIRE(slot >= 0);
        Internals::record(member.ptr())->predict_declaration().sim_mode
            = NetwPrediction::SIM_MODE_SPECULATIVE;
        const RID island = member->adopt_in(core.ptr());
        REQUIRE(pool->roster_add(
            slot,
            NetwPredictionEngine::ROSTER_ISLAND_MEMBERS,
            island
        ));

        const LocalVector<RID> listed = pool->roster_list(
            slot,
            NetwPredictionEngine::ROSTER_ISLAND_MEMBERS
        );
        NETW_CHECK_EQ(int(listed.size()), 1);
        const PackedStringArray ids = pool->live_participant_ids(slot);
        NETW_CHECK_EQ(int(ids.size()), 1);
        CHECK(ids[0] == String("Crate"));
        CHECK(pool->contact_is_equivalent(slot, false));
        Internals::unseat(pool, subject);
    }
    memdelete(seated);
    memdelete(joined);
}

} // namespace TestNetwPredictStatsLifetimeLaws
