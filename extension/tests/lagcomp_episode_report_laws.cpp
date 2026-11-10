#include "support/link.h"
#include "support/netw_test.h"

#include "support/scenario_run.h"

#if defined(NETW_TIER_HOSTED)

#include "support/event_ring.h"

#include "netw/api/event_plane.hpp"
#include "netw/api/netw_multiplayer.hpp"

namespace TestNetwLagCompEpisodeReportLaws {

using namespace netw_test;
using godot::Vector;
using godot::Vector2;
using netw::EventPlane;
using netw::NetwMultiplayer;

const Vector2 OFFSET = Vector2(60.0, -40.0);

EntityDecl watched_player() {
    return EntityDecl()
        .named("P")
        .on_schema("PredictedPose")
        .synced("position")
        .placed_at(Vector2())
        .predicted(0.01)
        .scheduled(netw::Schedule::TICK);
}

Scenario watched_lane(bool p_perturbed) {
    Scenario scenario;
    scenario.label = p_perturbed ? "episode-lane" : "stage-lane";
    scenario.epsilon = 0.01;
    scenario.world.clocked(30, 3).lag_compensated().player(watched_player(), 0);
    if (p_perturbed) {
        scenario.conditions(netw_test::polls(4));
    }
    scenario.hold_input(1, "P", Vector2(1.0, 0.0));
    if (!p_perturbed) {
        return scenario.until(20);
    }
    scenario.perturb(31, "P", OFFSET);
    return scenario.until(100);
}

bool is_episode_row(int64_t p_event) {
    return p_event == EventPlane::DIVERGENCE
        || p_event == EventPlane::EPISODE_OPEN
        || p_event == EventPlane::RECOVERY
        || p_event == EventPlane::EPISODE_CLOSE
        || p_event == EventPlane::EPISODE_FALLBACK;
}

TEST_CASE(
    "[Networked][LagComp][Episode] EP1 an owner records the disagreement "
    "BEFORE the episode opened to supervise it, then the write that answered "
    "it, and finally the close, because a divergence is a comparison's own "
    "verdict while an episode is what the pool does about it"
) {
    const Scenario scenario = watched_lane(true);
    LoopbackRig rig(scenario.clients);
    rig.client(0)->event_arm(NetwMultiplayer::EVENT_GROUP_PREDICTION);

    const ScenarioRun run = ScenarioRun::session(rig, scenario);
    REQUIRE(run.regime_reached());

    const EventRing ring(rig.client(0)->event_drain());
    Vector<int> rows;
    for (int at = 0; at < ring.size(); ++at) {
        if (is_episode_row(ring.event_at(at))) {
            rows.push_back(at);
        }
    }
    const int seen = int(rows.size());
    REQUIRE(seen >= 4);
    const int closed = rows[seen - 1];

    NETW_CHECK_EQ(ring.event_at(rows[0]), int64_t(EventPlane::DIVERGENCE));
    NETW_CHECK_EQ(ring.event_at(rows[1]), int64_t(EventPlane::EPISODE_OPEN));
    NETW_CHECK_EQ(ring.event_at(rows[2]), int64_t(EventPlane::RECOVERY));
    NETW_CHECK_EQ(ring.event_at(closed), int64_t(EventPlane::EPISODE_CLOSE));

    const int64_t transition = ring.a_at(rows[0]);
    NETW_CHECK_GE(transition, int64_t(0));
    NETW_CHECK_EQ(ring.a_at(rows[1]), transition);
    NETW_CHECK_EQ(ring.a_at(rows[2]), transition);
    NETW_CHECK_EQ(ring.a_at(closed), transition);

    SUBCASE(
        "and an acknowledgement already in flight when the write landed still "
        "disagrees, so it names a LATER transition and earns no second write"
    ) {
        for (int at = 3; at < seen - 1; ++at) {
            NETW_CHECK_EQ(
                ring.event_at(rows[at]),
                int64_t(EventPlane::DIVERGENCE)
            );
            NETW_CHECK_GT(ring.a_at(rows[at]), transition);
        }
    }

    NETW_CHECK_GT(ring.count_of(EventPlane::PREDICT_EVALUATE), 1);
    NETW_CHECK_GT(ring.count_of(EventPlane::PREDICT_RECOVER), 0);
}

TEST_CASE(
    "[Networked][LagComp][Episode] EP2 every driven pass records the "
    "transition it opened, and consuming is the other side of that pass, "
    "which only authority runs, so an owner never records one"
) {
    const Scenario scenario = watched_lane(false);
    LoopbackRig rig(scenario.clients);
    rig.client(0)->event_arm(NetwMultiplayer::EVENT_GROUP_PREDICTION);
    rig.server()->event_arm(NetwMultiplayer::EVENT_GROUP_PREDICTION);

    const ScenarioRun run = ScenarioRun::session(rig, scenario);
    REQUIRE(run.regime_reached());

    const EventRing owner(rig.client(0)->event_drain());
    const EventRing authority(rig.server()->event_drain());

    const int drive = owner.first_of(EventPlane::PREDICT_DRIVE);
    REQUIRE(drive >= 0);
    NETW_CHECK_GE(owner.a_at(drive), int64_t(0));

    NETW_CHECK_EQ(owner.count_of(EventPlane::PREDICT_CONSUME), 0);

    const int consume = authority.first_of(EventPlane::PREDICT_CONSUME);
    REQUIRE(consume >= 0);
    NETW_CHECK_GE(authority.b_at(consume), int64_t(0));
}

} // namespace TestNetwLagCompEpisodeReportLaws

#endif
