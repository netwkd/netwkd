#include "support/fault_log.h"
#include "support/internals.h"
#include "support/loopback_rig.h"

#if defined(NETW_TIER_HOSTED)

#include "godot/node.hpp"
#include "netw/api/context.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/entity_options.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/promise.hpp"
#include "netw/entity/control.hpp"
#include "netw/session/event_plane.hpp"
#include "netw/session/frames.hpp"
#include "netw/wire/registry.hpp"
#include "support/event_ring.h"
#include "support/netw_call_log.h"

#include <godot_cpp/classes/node2d.hpp>

namespace TestControlAxis {

using namespace godot;
using namespace netw_test;
using netw::NetwControlRequest;
using netw::NetwEntity;
using netw::NetwMultiplayer;
using netw::NetwPromise;
using Control = netw::entity::Control;

const char *PLAYER_ID = "control_player";

bool &builds_controller_authority() {
    static bool held = false;
    return held;
}

int64_t &builds_starting_controller() {
    static int64_t held = 0;
    return held;
}

int64_t &builds_root_authority() {
    static int64_t held = 0;
    return held;
}

Node *build_player(const Variant &p_name) {
    Node2D *made = memnew(Node2D);
    made->set_name(String(p_name));
    if (builds_controller_authority()) {
        NetwEntity::ensure(made)->set_authority(
            NetwEntity::AUTHORITY_CONTROLLER
        );
    }
    if (builds_starting_controller() != 0) {
        NetwEntity::ensure(made)->set_controller(builds_starting_controller());
    }
    if (builds_root_authority() != 0) {
        made->set_multiplayer_authority(int32_t(builds_root_authority()));
    }
    return made;
}

Array named(const char *p_name) {
    Array out;
    out.push_back(String(p_name));
    return out;
}

Array one_type() {
    Array out;
    out.push_back(int(Variant::STRING));
    return out;
}

void deny_every_request(int64_t, Object *p_request) {
    NetwControlRequest *asked = Object::cast_to<NetwControlRequest>(p_request);
    if (asked != nullptr) {
        asked->deny();
    }
}

void teach_constructor(LoopbackRig &p_rig, NetwMultiplayer *p_api) {
    p_rig.register_constructor(
        p_api,
        StringName(PLAYER_ID),
        callable_mp_static(&build_player),
        one_type()
    );
}

int seat_player(LoopbackRig &p_rig, Node *p_parent, const char *p_name) {
    teach_constructor(p_rig, p_rig.server());
    for (int at = 0; at < p_rig.count(); ++at) {
        teach_constructor(p_rig, p_rig.client(at));
    }
    const RID entity = p_rig.server()->spawn_registered(
        StringName(PLAYER_ID),
        named(p_name),
        p_rig.player(0)->get_id()
    );
    REQUIRE_MESSAGE(entity.is_valid(), "the spawn verb minted no entity");
    Node *node = p_rig.server()->entity_get_node(entity);
    REQUIRE_MESSAGE(node != nullptr, "the spawn verb built no node");
    p_parent->add_child(node);
    p_rig.pump(8);
    return int(p_rig.server()->entity_get_route(entity));
}

Ref<NetwEntity> entity_at(LoopbackRig &p_rig, int p_route, int p_client) {
    Node *node = p_rig.route_node(p_route, p_client);
    return node == nullptr ? Ref<NetwEntity>() : NetwEntity::of(node);
}

TEST_CASE(
    "[Networked][Entity] CA1 a granted request moves the controller on BOTH "
    "peers, and moves the node authority with it exactly when the entity "
    "declares AUTHORITY_CONTROLLER"
) {
    LoopbackRig rig(2);
    rig.mount();
    Node *arena = rig.mirror_child("Arena");
    bool follows = false;
    SUBCASE("AUTHORITY_SERVER keeps the node with the server") {
        follows = false;
    }
    SUBCASE("AUTHORITY_CONTROLLER hands the node to the controller") {
        follows = true;
    }
    builds_controller_authority() = follows;
    const int route = seat_player(rig, arena, "Steerable");
    builds_controller_authority() = false;
    const Ref<NetwEntity> host = entity_at(rig, route, -1);
    REQUIRE(host.is_valid());
    host->set_transfer(NetwEntity::TRANSFER_REQUESTABLE);

    const Ref<NetwEntity> asking = entity_at(rig, route, 1);
    REQUIRE_MESSAGE(asking.is_valid(), "the spawn never reached the asker");
    const int64_t asker = rig.peer_id(1);
    const int64_t holder = follows ? asker : int64_t(1);

    asking->claim_control();
    rig.pump(10);

    NETW_CHECK_EQ(host->get_controller(), asker);
    NETW_CHECK_EQ(entity_at(rig, route, 1)->get_controller(), asker);
    NETW_CHECK_EQ(entity_at(rig, route, 0)->get_controller(), asker);
    CHECK(entity_at(rig, route, 1)->get_is_controlled_locally());
    for (int seat = -1; seat < 2; ++seat) {
        NETW_CHECK_EQ(
            rig.route_node(route, seat)->get_multiplayer_authority(),
            holder
        );
    }
}

TEST_CASE(
    "[Networked][Entity] CA2 a request the server denies steers nobody, and "
    "neither does one against an entity that never offered the transfer"
) {
    netw_test::FaultWatch expected_faults;
    expected_faults.expect_warning("*[NW0024]*", 1);

    LoopbackRig rig(2);
    rig.mount();
    Node *arena = rig.mirror_child("Arena");

    const int route = seat_player(rig, arena, "Refusing");
    const Ref<NetwEntity> host = entity_at(rig, route, -1);
    REQUIRE(host.is_valid());
    const int64_t represented = rig.peer_id(0);
    NETW_CHECK_EQ(host->get_controller(), represented);

    host->set_transfer(NetwEntity::TRANSFER_REQUESTABLE);
    host->connect(
        StringName("control_requested"),
        callable_mp_static(&deny_every_request)
    );

    const Ref<NetwPromise> denied = entity_at(rig, route, 1)->claim_control();
    rig.pump(10);

    CHECK(denied->get_is_failed());
    NETW_CHECK_EQ(denied->get_code(), ERR_UNAUTHORIZED);
    NETW_CHECK_EQ(host->get_controller(), represented);
    NETW_CHECK_EQ(rig.route_node(route, -1)->get_multiplayer_authority(), 1);

    host->disconnect(
        StringName("control_requested"),
        callable_mp_static(&deny_every_request)
    );
    host->set_transfer(NetwEntity::TRANSFER_FIXED);

    const Ref<NetwPromise> fixed = entity_at(rig, route, 1)->claim_control();
    rig.pump(10);

    NETW_CHECK_EQ(host->get_controller(), represented);
    CHECK(fixed->get_is_failed());
    NETW_CHECK_EQ(fixed->get_code(), ERR_UNAVAILABLE);
    NETW_CHECK_EQ(entity_at(rig, route, 1)->get_controller(), represented);
    NETW_CHECK_EQ(rig.route_node(route, 1)->get_multiplayer_authority(), 1);
}

void teach_every_session(LoopbackRig &p_rig) {
    teach_constructor(p_rig, p_rig.server());
    for (int at = 0; at < p_rig.count(); ++at) {
        teach_constructor(p_rig, p_rig.client(at));
    }
}

int seat_free(LoopbackRig &p_rig, Node *p_parent, const char *p_name) {
    const RID entity = p_rig.server()->spawn_registered(
        StringName(PLAYER_ID),
        named(p_name),
        0
    );
    REQUIRE_MESSAGE(entity.is_valid(), "the spawn verb minted no entity");
    Node *node = p_rig.server()->entity_get_node(entity);
    REQUIRE_MESSAGE(node != nullptr, "the spawn verb built no node");
    p_parent->add_child(node);
    p_rig.pump(8);
    NetwEntity::of(node)->set_transfer(NetwEntity::TRANSFER_REQUESTABLE);
    return int(p_rig.server()->entity_get_route(entity));
}

int hold_at(LoopbackRig &p_rig, int p_route, int p_client) {
    return int(entity_at(p_rig, p_route, p_client)->get_hold());
}

Error deliver_apply(
    LoopbackRig &p_rig,
    int p_client,
    int p_route,
    const netw::session::ControlApply &p_applied
) {
    return p_rig.client(p_client)->receive_carrier(
        NetwMultiplayer::frame_pack(
            p_route,
            0,
            netw::wire::builtin_channel("CONTROL_APPLY"),
            netw::session::frame_write(p_applied),
            String()
        ),
        p_rig.peer_id(-1),
        true,
        -1,
        -1
    );
}

TEST_CASE(
    "[Networked][Entity] CA6 a request the confirmed decision already "
    "excludes is refused before anything is sent, so the server never hears "
    "of it"
) {
    LoopbackRig rig(2);
    rig.mount();
    Node *arena = rig.mirror_child("Arena");
    teach_every_session(rig);
    rig.server()->event_arm(NetwMultiplayer::EVENT_GROUP_LIFECYCLE);
    const int route = seat_free(rig, arena, "Held");

    const Ref<NetwPromise> first = entity_at(rig, route, 0)->claim_control();
    rig.pump(10);
    CHECK(first->get_is_completed());
    NETW_CHECK_EQ(entity_at(rig, route, 1)->get_controller(), rig.peer_id(0));
    NETW_CHECK_EQ(hold_at(rig, route, 1), int(Control::HOLD_EXCLUSIVE));

    rig.server()->event_drain();
    const Ref<NetwPromise> refused = entity_at(rig, route, 1)->claim_control();
    CHECK(refused->get_is_failed());
    NETW_CHECK_EQ(int(refused->get_code()), int(ERR_UNAUTHORIZED));
    CHECK_FALSE(entity_at(rig, route, 1)->get_is_control_pending());
    rig.pump(10);

    const EventRing heard(rig.server()->event_drain());
    NETW_CHECK_EQ(
        heard.count_of(netw::EventPlane::CONTROL_REQUESTED, route),
        0
    );
    NETW_CHECK_EQ(entity_at(rig, route, -1)->get_controller(), rig.peer_id(0));
}

TEST_CASE(
    "[Networked][Entity] CA7 an exclusive request takes an entity another "
    "peer holds yieldably, and a yieldable one does not"
) {
    LoopbackRig rig(2);
    rig.mount();
    Node *arena = rig.mirror_child("Arena");
    teach_every_session(rig);
    const int route = seat_free(rig, arena, "Touched");

    entity_at(rig, route, 0)->claim_control(Control::HOLD_SOFT);
    rig.pump(10);
    NETW_CHECK_EQ(entity_at(rig, route, -1)->get_controller(), rig.peer_id(0));
    NETW_CHECK_EQ(hold_at(rig, route, 1), int(Control::HOLD_SOFT));
    const int64_t touched
        = int64_t(entity_at(rig, route, -1)->get_control_tenure());

    const Ref<NetwPromise> nudged
        = entity_at(rig, route, 1)->claim_control(Control::HOLD_SOFT);
    CHECK(nudged->get_is_failed());
    NETW_CHECK_EQ(int(nudged->get_code()), int(ERR_UNAUTHORIZED));

    const Ref<NetwPromise> grabbed
        = entity_at(rig, route, 1)->claim_control(Control::HOLD_EXCLUSIVE);
    rig.pump(10);
    CHECK(grabbed->get_is_completed());
    for (int client = -1; client < 2; ++client) {
        NETW_CHECK_EQ(
            entity_at(rig, route, client)->get_controller(),
            rig.peer_id(1)
        );
        NETW_CHECK_EQ(
            hold_at(rig, route, client),
            int(Control::HOLD_EXCLUSIVE)
        );
    }
    NETW_CHECK_GT(
        int64_t(entity_at(rig, route, -1)->get_control_tenure()),
        touched
    );
}

TEST_CASE(
    "[Networked][Entity] CA8 the controller changing its own hold keeps its "
    "tenure and never asks the filter, because nobody else is asking for "
    "anything"
) {
    LoopbackRig rig(1);
    rig.mount();
    Node *arena = rig.mirror_child("Arena");
    teach_every_session(rig);
    const int route = seat_free(rig, arena, "Regripped");
    const Ref<NetwEntity> host = entity_at(rig, route, -1);

    entity_at(rig, route, 0)->claim_control(Control::HOLD_SOFT);
    rig.pump(10);
    NETW_REQUIRE_EQ(host->get_controller(), int64_t(rig.peer_id(0)));
    const int64_t tenure = int64_t(host->get_control_tenure());
    const int64_t revision = int64_t(host->get_control_revision());

    CallLog log;
    entity_at(rig, route, 0)
        ->connect(StringName("control_changed"), log.callable("changed"));
    host->connect(
        StringName("control_requested"),
        callable_mp_static(&deny_every_request)
    );
    const Ref<NetwPromise> tightened
        = entity_at(rig, route, 0)->claim_control(Control::HOLD_EXCLUSIVE);
    rig.pump(10);

    CHECK(tightened->get_is_completed());
    NETW_CHECK_EQ(int(host->get_hold()), int(Control::HOLD_EXCLUSIVE));
    NETW_CHECK_EQ(int64_t(host->get_control_tenure()), tenure);
    NETW_CHECK_EQ(int64_t(host->get_control_revision()), revision + 1);
    NETW_CHECK_EQ(hold_at(rig, route, 0), int(Control::HOLD_EXCLUSIVE));
    NETW_CHECK_EQ(
        int64_t(entity_at(rig, route, 0)->get_control_tenure()),
        tenure
    );
    NETW_CHECK_EQ(log.count("changed"), 0);
    host->disconnect(
        StringName("control_requested"),
        callable_mp_static(&deny_every_request)
    );
}

TEST_CASE(
    "[Networked][Entity] CA9 a controller write moves an entity a peer "
    "holds exclusively, and the session's own decision holds nothing"
) {
    LoopbackRig rig(2);
    rig.mount();
    Node *arena = rig.mirror_child("Arena");
    teach_every_session(rig);
    const int route = seat_free(rig, arena, "Seized");

    entity_at(rig, route, 0)->claim_control(Control::HOLD_EXCLUSIVE);
    rig.pump(10);
    NETW_REQUIRE_EQ(
        entity_at(rig, route, -1)->get_controller(),
        int64_t(rig.peer_id(0))
    );

    entity_at(rig, route, -1)->set_controller(rig.peer_id(1));
    rig.pump(10);
    for (int client = -1; client < 2; ++client) {
        NETW_CHECK_EQ(
            entity_at(rig, route, client)->get_controller(),
            rig.peer_id(1)
        );
        NETW_CHECK_EQ(hold_at(rig, route, client), int(Control::HOLD_NONE));
    }
}

TEST_CASE(
    "[Networked][Entity] CA9b a controller write off the session authority "
    "is refused and moves nothing, and a write of 1 takes control back"
) {
    netw_test::FaultWatch expected_faults;
    expected_faults.expect_error("*NetwEntity.controller*[NW0012]*", 1);

    LoopbackRig rig(2);
    rig.mount();
    Node *arena = rig.mirror_child("Arena");
    teach_every_session(rig);
    const int route = seat_free(rig, arena, "Contested");

    entity_at(rig, route, -1)->set_controller(rig.peer_id(0));
    rig.pump(10);
    entity_at(rig, route, 1)->set_controller(rig.peer_id(1));
    rig.pump(10);
    for (int client = -1; client < 2; ++client) {
        NETW_CHECK_EQ(
            entity_at(rig, route, client)->get_controller(),
            rig.peer_id(0)
        );
    }

    entity_at(rig, route, -1)->set_controller(1);
    rig.pump(10);
    for (int client = -1; client < 2; ++client) {
        NETW_CHECK_EQ(entity_at(rig, route, client)->get_controller(), 1);
    }
}

TEST_CASE(
    "[Networked][Entity] CA9c claim_control and release_control on the "
    "entity a node belongs to move its controller, and a node under no "
    "entity has no entity to ask"
) {
    LoopbackRig rig(1);
    rig.mount();
    Node *arena = rig.mirror_child("Arena");
    teach_every_session(rig);
    const int route = seat_free(rig, arena, "Relayed");
    Node *held = rig.route_node(route, 0);
    REQUIRE(held != nullptr);

    const Ref<NetwEntity> entity = NetwEntity::of(held);
    REQUIRE(entity.is_valid());
    const Ref<NetwPromise> claimed
        = entity->claim_control(Control::HOLD_EXCLUSIVE);
    rig.pump(10);
    CHECK(claimed->get_is_settled());
    NETW_CHECK_EQ(
        entity_at(rig, route, -1)->get_controller(),
        int64_t(rig.peer_id(0))
    );

    const Ref<NetwPromise> released = entity->release_control();
    rig.pump(10);
    CHECK(released->get_is_settled());
    NETW_CHECK_EQ(entity_at(rig, route, -1)->get_controller(), 1);

    Node *loose = memnew(Node);
    CHECK(NetwEntity::of(loose).is_null());
    memdelete(loose);
}

TEST_CASE(
    "[Networked][Entity] CA19 a controller written inside the spawn function "
    "arrives identically on the host, a client and a late joiner, under "
    "either authority"
) {
    NetwEntity::Authority mode = NetwEntity::AUTHORITY_SERVER;
    SUBCASE("AUTHORITY_SERVER") {
        mode = NetwEntity::AUTHORITY_SERVER;
    }
    SUBCASE("AUTHORITY_CONTROLLER") {
        mode = NetwEntity::AUTHORITY_CONTROLLER;
    }
    LoopbackRig rig(1);
    rig.mount();
    Node *arena = rig.mirror_child("Arena");
    teach_every_session(rig);
    const int64_t steerer = rig.peer_id(0);
    builds_controller_authority() = mode == NetwEntity::AUTHORITY_CONTROLLER;
    builds_starting_controller() = steerer;
    const int route = seat_free(rig, arena, "Started");
    builds_controller_authority() = false;
    builds_starting_controller() = 0;

    const int late = rig.add_client();
    teach_constructor(rig, rig.client(late));
    rig.hold(late);
    rig.mount_late(late);
    rig.mirror_late(late, "Arena");
    rig.release(late);
    rig.pump(10);

    const int64_t holder
        = mode == NetwEntity::AUTHORITY_CONTROLLER ? steerer : int64_t(1);
    const int seats[] = {-1, 0, late};
    for (const int seat : seats) {
        NETW_FORMAT_INT(seat_text, seat);
        CAPTURE(seat_text);
        const Ref<NetwEntity> seen = entity_at(rig, route, seat);
        REQUIRE(seen.is_valid());
        NETW_CHECK_EQ(seen->get_controller(), steerer);
        NETW_CHECK_EQ(int(seen->get_authority()), int(mode));
        NETW_CHECK_EQ(
            int64_t(rig.route_node(route, seat)->get_multiplayer_authority()),
            holder
        );
        NETW_CHECK_EQ(int(seen->get_is_controlled_locally()), int(seat == 0));
    }
}

TEST_CASE(
    "[Networked][Entity] CA19b a spawn function that writes the controller "
    "and also hands the root to that peer keeps AUTHORITY_SERVER, and every "
    "peer warns as it moves the root back to the server"
) {
    netw_test::FaultWatch expected_faults;
    expected_faults.expect_error("*'Handed'*[NW1012]*", 3);

    LoopbackRig rig(2);
    rig.mount();
    Node *arena = rig.mirror_child("Arena");
    teach_every_session(rig);
    const int64_t steerer = rig.peer_id(0);
    builds_starting_controller() = steerer;
    builds_root_authority() = steerer;
    const int route = seat_free(rig, arena, "Handed");
    builds_starting_controller() = 0;
    builds_root_authority() = 0;
    rig.pump(10);

    const int seats[] = {-1, 0, 1};
    for (const int seat : seats) {
        NETW_FORMAT_INT(seat_text, seat);
        CAPTURE(seat_text);
        const Ref<NetwEntity> seen = entity_at(rig, route, seat);
        REQUIRE(seen.is_valid());
        NETW_CHECK_EQ(seen->get_controller(), steerer);
        NETW_CHECK_EQ(
            int(seen->get_authority()),
            int(NetwEntity::AUTHORITY_SERVER)
        );
        NETW_CHECK_EQ(
            int64_t(rig.route_node(route, seat)->get_multiplayer_authority()),
            int64_t(1)
        );
        NETW_CHECK_EQ(int(seen->get_is_controlled_locally()), int(seat == 0));
    }
}

TEST_CASE(
    "[Networked][Entity] CA10 a late joiner starts on the revision, tenure "
    "and hold the session already decided"
) {
    LoopbackRig rig(1);
    rig.mount();
    Node *arena = rig.mirror_child("Arena");
    teach_every_session(rig);
    const int route = seat_free(rig, arena, "Seeded");

    entity_at(rig, route, 0)->claim_control(Control::HOLD_SOFT);
    rig.pump(10);
    entity_at(rig, route, 0)->claim_control(Control::HOLD_EXCLUSIVE);
    rig.pump(10);
    const Ref<NetwEntity> host = entity_at(rig, route, -1);
    NETW_REQUIRE_EQ(int64_t(host->get_control_revision()), int64_t(2));

    const int late = rig.add_client();
    teach_constructor(rig, rig.client(late));
    rig.hold(late);
    rig.mount_late(late);
    rig.mirror_late(late, "Arena");
    rig.release(late);
    rig.pump(10);

    const Ref<NetwEntity> joined = entity_at(rig, route, late);
    REQUIRE(joined.is_valid());
    NETW_CHECK_EQ(joined->get_controller(), int64_t(rig.peer_id(0)));
    NETW_CHECK_EQ(int64_t(joined->get_control_revision()), int64_t(2));
    NETW_CHECK_EQ(int64_t(joined->get_control_tenure()), int64_t(1));
    NETW_CHECK_EQ(int(joined->get_hold()), int(Control::HOLD_EXCLUSIVE));
}

TEST_CASE(
    "[Networked][Entity] CA11 an apply that reaches a joiner before its spawn "
    "waits for the spawn, and one older than the spawn's own decision is "
    "discarded"
) {
    LoopbackRig rig(1);
    rig.mount();
    Node *arena = rig.mirror_child("Arena");
    teach_every_session(rig);
    const int route = seat_free(rig, arena, "Deferred");

    entity_at(rig, route, 0)->claim_control(Control::HOLD_SOFT);
    rig.pump(10);
    entity_at(rig, route, 0)->claim_control(Control::HOLD_EXCLUSIVE);
    rig.pump(10);

    const int late = rig.add_client();
    teach_constructor(rig, rig.client(late));
    rig.hold(late);
    rig.mount_late(late);
    rig.mirror_late(late, "Arena");
    rig.client(late)->event_arm(NetwMultiplayer::EVENT_GROUP_LIFECYCLE);

    netw::session::ControlApply stale;
    stale.controller = 0;
    stale.revision = 1;
    stale.tenure_changed = true;
    deliver_apply(rig, late, route, stale);

    netw::session::ControlApply newer;
    newer.controller = uint64_t(rig.peer_id(0));
    newer.revision = 3;
    newer.hold = uint8_t(Control::HOLD_SOFT);
    deliver_apply(rig, late, route, newer);

    rig.release(late);
    rig.pump(10);

    const Ref<NetwEntity> joined = entity_at(rig, route, late);
    REQUIRE(joined.is_valid());
    NETW_CHECK_EQ(joined->get_controller(), int64_t(rig.peer_id(0)));
    NETW_CHECK_EQ(int64_t(joined->get_control_revision()), int64_t(3));
    NETW_CHECK_EQ(int64_t(joined->get_control_tenure()), int64_t(1));
    NETW_CHECK_EQ(int(joined->get_hold()), int(Control::HOLD_SOFT));

    const EventRing moved(rig.client(late)->event_drain());
    for (int at = 0; at < moved.size(); ++at) {
        if (moved.event_at(at) == netw::EventPlane::CONTROL_CHANGED
            && moved.route_at(at) == route) {
            NETW_CHECK_GT(moved.b_at(at), int64_t(0));
        }
    }
}

TEST_CASE(
    "[Networked][Entity] CA12 an apply that answers another op never settles "
    "a pending request, because settlement matches the op it names"
) {
    LoopbackRig rig(1);
    rig.mount();
    Node *arena = rig.mirror_child("Arena");
    teach_every_session(rig);
    const int route = seat_free(rig, arena, "Matched");

    rig.hold(0);
    const Ref<NetwPromise> asked = entity_at(rig, route, 0)->claim_control();
    rig.pump(10);
    REQUIRE_FALSE(asked->get_is_settled());

    netw::session::ControlApply unrelated;
    deliver_apply(rig, 0, route, unrelated);
    unrelated.op = 7;
    unrelated.outcome = uint8_t(Control::Outcome::UNAUTHORIZED);
    deliver_apply(rig, 0, route, unrelated);

    CHECK_FALSE(asked->get_is_settled());
    CHECK(entity_at(rig, route, 0)->get_is_control_pending());

    rig.release(0);
    rig.pump(10);
    CHECK(asked->get_is_completed());
    CHECK_FALSE(entity_at(rig, route, 0)->get_is_control_pending());
}

TEST_CASE(
    "[Networked][Entity] CA13 a request that times out at the issuer and is "
    "granted afterwards is a real decision, so the issuer gives it straight "
    "back and control_changed fires twice"
) {
    LoopbackRig rig(1);
    rig.mount();
    Node *arena = rig.mirror_child("Arena");
    teach_every_session(rig);
    const int route = seat_free(rig, arena, "Late");
    const Ref<NetwEntity> issuer = entity_at(rig, route, 0);

    CallLog log;
    issuer->connect(StringName("control_changed"), log.callable("changed"));

    rig.hold(0);
    const Ref<NetwPromise> asked = issuer->claim_control();
    rig.pump(6);
    NETW_REQUIRE_EQ(
        entity_at(rig, route, -1)->get_controller(),
        int64_t(rig.peer_id(0))
    );

    rig.step_ticks(
        int(Internals::clock_engine(rig.client(0)).get_tickrate()) + 1
    );
    CHECK(asked->get_is_failed());
    NETW_CHECK_EQ(int(asked->get_code()), int(ERR_TIMEOUT));
    CHECK_FALSE(issuer->get_is_control_pending());

    rig.release(0);
    rig.pump(12);

    NETW_CHECK_EQ(log.count("changed"), 2);
    NETW_CHECK_EQ(entity_at(rig, route, -1)->get_controller(), int64_t(1));
    NETW_CHECK_EQ(issuer->get_controller(), int64_t(1));
    NETW_CHECK_EQ(int(asked->get_code()), int(ERR_TIMEOUT));
}

TEST_CASE(
    "[Networked][Entity] CA15 a ninth request outstanding on one entity is "
    "refused before it is sent"
) {
    LoopbackRig rig(1);
    rig.mount();
    Node *arena = rig.mirror_child("Arena");
    teach_every_session(rig);
    const int route = seat_free(rig, arena, "Crowded");
    const Ref<NetwEntity> issuer = entity_at(rig, route, 0);

    for (uint32_t at = 0; at < Control::MAX_PENDING; ++at) {
        CHECK_FALSE(issuer->claim_control()->get_is_settled());
    }
    const Ref<NetwPromise> ninth = issuer->claim_control();
    CHECK(ninth->get_is_failed());
    NETW_CHECK_EQ(int(ninth->get_code()), int(ERR_UNAVAILABLE));

    rig.pump(10);
    CHECK_FALSE(issuer->get_is_control_pending());
    NETW_CHECK_EQ(issuer->get_controller(), int64_t(rig.peer_id(0)));
}

TEST_CASE(
    "[Networked][Entity] CA16 an immediate request on an entity a "
    "MultiplayerSynchronizer replicates is refused before it is sent, "
    "because nothing it authors can run ahead of the decision"
) {
    LoopbackRig rig(1);
    rig.mount();
    Node *arena = rig.mirror_child("Arena");
    teach_every_session(rig);
    builds_controller_authority() = true;
    const int route = seat_free(rig, arena, "Adopted");
    builds_controller_authority() = false;
    const Ref<NetwEntity> issuer = entity_at(rig, route, 0);
    issuer->set_transfer(NetwEntity::TRANSFER_IMMEDIATE);
    MultiplayerSynchronizer *adopted = memnew(MultiplayerSynchronizer);
    issuer->get_owner()->add_child(adopted);
    adopted->set_owner(issuer->get_owner());
    issuer->invalidate_synchronizers_cache();

    const Ref<NetwPromise> refused = issuer->claim_control();
    CHECK(refused->get_is_failed());
    NETW_CHECK_EQ(int(refused->get_code()), int(ERR_UNAVAILABLE));
    CHECK_FALSE(issuer->get_is_control_pending());

    issuer->set_transfer(NetwEntity::TRANSFER_REQUESTABLE);
    const Ref<NetwPromise> asked = issuer->claim_control();
    rig.pump(10);
    CHECK(asked->get_is_completed());
}

TEST_CASE(
    "[Networked][Entity] CA17 a request still outstanding when its entity is "
    "freed fails as unavailable, because no decision can reach it any more"
) {
    LoopbackRig rig(1);
    rig.mount();
    Node *arena = rig.mirror_child("Arena");
    teach_every_session(rig);
    const int route = seat_free(rig, arena, "Dropped");

    rig.hold(0);
    const Ref<NetwPromise> asked = entity_at(rig, route, 0)->claim_control();
    REQUIRE_FALSE(asked->get_is_settled());

    Node *copy = rig.route_node(route, 0);
    REQUIRE(copy != nullptr);
    copy->get_parent()->remove_child(copy);
    memdelete(copy);
    rig.step_ticks(1);

    CHECK(asked->get_is_failed());
    NETW_CHECK_EQ(int(asked->get_code()), int(ERR_UNAVAILABLE));
    rig.release(0);
    rig.pump(4);
}

TEST_CASE(
    "[Networked][Entity] CA18 an entity its own session outlives only inside "
    "that session's teardown is freed there, and a request it still holds "
    "fails as unavailable"
) {
    Ref<NetwPromise> asked;
    {
        LoopbackRig rig(1);
        rig.mount();
        Node *arena = rig.mirror_child("Arena");
        teach_every_session(rig);
        const int route = seat_free(rig, arena, "Orphaned");
        rig.hold(0);
        asked = entity_at(rig, route, 0)->claim_control();
        REQUIRE_FALSE(asked->get_is_settled());
    }

    CHECK(asked->get_is_failed());
    NETW_CHECK_EQ(int(asked->get_code()), int(ERR_UNAVAILABLE));
}

} // namespace TestControlAxis

#endif
