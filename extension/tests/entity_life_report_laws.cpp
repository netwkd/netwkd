#include "support/netw_test.h"

#include "support/event_ring.h"

#include "godot/node.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/entity_options.hpp"
#include "netw/api/entity_record.hpp"
#include "netw/api/event_plane.hpp"
#include "netw/api/netw_multiplayer.hpp"

namespace TestNetwEntityLifeReportLaws {

using namespace godot;
using netw::EventPlane;
using netw::NetwEntity;
using netw::NetwEntityRecord;
using netw::NetwMultiplayer;
using netw_test::EventRing;

constexpr int64_t REPORTED_ROUTE = 11;

TEST_CASE(
    "[Networked][Entity][Hosted] EL1 a body that materializes goes live and "
    "records it on its own route, so a tool learns the entity that asked for "
    "the route the session already published as live"
) {
    Ref<NetwMultiplayer> core;
    core.instantiate();
    core->session_set_role(NetwMultiplayer::ROLE_LISTEN_SERVER);
    core->event_arm(NetwMultiplayer::EVENT_GROUP_LIFECYCLE);
    Node *owner = memnew(Node);
    owner->set_name("Born");
    const Ref<NetwEntity> wrapper = NetwEntity::ensure(owner);
    REQUIRE(wrapper.is_valid());
    NetwEntityRecord *const record = wrapper->get_record();
    REQUIRE(record != nullptr);
    record->set_entity_id(StringName("born"));
    record->set_peer_id(4);
    record->set_route(REPORTED_ROUTE);

    NetwMultiplayer::entity_enter_tree(
        wrapper.ptr(),
        owner,
        record,
        core.ptr(),
        true
    );

    NETW_CHECK_EQ(
        int64_t(wrapper->get_stage()),
        int64_t(NetwEntity::STAGE_LIVE)
    );
    const EventRing ring(core->event_drain());
    const int materialized = ring.first_of(EventPlane::SPAWNING);
    REQUIRE(materialized >= 0);
    NETW_CHECK_EQ(ring.route_at(materialized), REPORTED_ROUTE);
    NETW_CHECK_EQ(ring.peer_at(materialized), record->get_peer_id());
    NETW_CHECK_EQ(
        ring.entity_at(materialized),
        int64_t(wrapper->get_rid_handle().get_id())
    );

    memdelete(owner);
}

} // namespace TestNetwEntityLifeReportLaws
