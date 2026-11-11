#include "support/internals.h"
#include "support/netw_test.h"

#if defined(NETW_TIER_HOSTED)

#include "support/display_stand.h"
#include "support/event_ring.h"

#include "godot/spatial_node.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/session/event_plane.hpp"

namespace TestNetwDisplaySettleLaws {

using namespace godot;
using netw::NetwMultiplayer;
using netw_test::Internals;

const Vector2 START = Vector2(0.0, 0.0);
const Vector2 FINISH = Vector2(100.0, 0.0);

struct Pumped {
    Vector<Vector2> visuals;
    int64_t pumps = 0;
    int64_t entity = 0;
    PackedInt64Array rows;
};

Pumped pump_to_rest(int64_t p_groups) {
    Pumped seen;
    const Ref<NetwMultiplayer> core = netw_test::clocked_session();
    const netw_test::DisplaySubject subject = netw_test::stand_subject(core, 0);
    core->event_arm(p_groups);
    netw_test::record_sample(core, subject.body, START, 0);
    netw_test::record_sample(core, subject.body, FINISH, 1);
    const int64_t opened = Internals::idle_work(core).pumped;
    for (int tick = 1; tick <= 6; tick++) {
        netw_test::pump_at(core, tick, 1, 0.5);
        seen.visuals.push_back(subject.first->get_global_position());
    }
    seen.pumps = Internals::idle_work(core).pumped - opened;
    seen.entity = int64_t(subject.entity->get_rid_handle().get_id());
    seen.rows = core->event_drain();
    netw_test::retire_subject(subject);
    return seen;
}

TEST_CASE(
    "[Networked][Display][SceneTree] EV4 recording every group changes "
    "neither what a display shows nor how often it pumps, and its record, "
    "pump and write rows name the entity by RID and by route"
) {
    const int64_t every_group = NetwMultiplayer::EVENT_GROUP_LIFECYCLE
        | NetwMultiplayer::EVENT_GROUP_INTEREST
        | NetwMultiplayer::EVENT_GROUP_PREDICTION
        | NetwMultiplayer::EVENT_GROUP_GATE | NetwMultiplayer::EVENT_GROUP_STAGE
        | NetwMultiplayer::EVENT_GROUP_VERDICT
        | NetwMultiplayer::EVENT_GROUP_SESSION
        | NetwMultiplayer::EVENT_GROUP_CARRIER;
    const Pumped quiet = pump_to_rest(0);
    const Pumped armed = pump_to_rest(every_group);

    CHECK(quiet.visuals == armed.visuals);
    NETW_CHECK_EQ(armed.pumps, quiet.pumps);
    NETW_CHECK_EQ(int(quiet.rows.size()), 0);

    const netw_test::EventRing ring(armed.rows);
    NETW_CHECK_EQ(ring.count_of(netw::EventPlane::DISPLAY_RECORD), 2);
    NETW_CHECK_GT(ring.count_of(netw::EventPlane::DISPLAY_PUMP), 0);
    NETW_CHECK_GT(ring.count_of(netw::EventPlane::DISPLAY_WRITE), 0);
    int unnamed = 0;
    for (int index = 0; index < ring.size(); index++) {
        const bool named = ring.route_at(index) == 7
            && ring.entity_at(index) == armed.entity;
        unnamed += named ? 0 : 1;
    }
    NETW_CHECK_EQ(unnamed, 0);
}

} // namespace TestNetwDisplaySettleLaws

#endif
