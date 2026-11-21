#include "support/fault_log.h"
#include "support/internals.h"
#include "support/loopback_rig.h"

#if defined(NETW_TIER_HOSTED)

#include "godot/node.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/entity_options.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/participant.hpp"
#include "netw/api/scene.hpp"
#include "netw/entity/ids.hpp"
#include "netw/scene_membership.hpp"
#include "support/joined_peer.h"
#include "support/netw_call_log.h"
#include "support/netw_recorder.h"

namespace TestSceneResidencyLaws {

using namespace godot;
using netw::NetwEntity;
using netw::NetwMultiplayer;
using netw_test::Internals;
using netw_test::LoopbackRig;
using netw_test::Recorder;

netw_test::CallLog *&avatar_listener() {
    static netw_test::CallLog *held = nullptr;
    return held;
}

Node *build_avatar(const Variant &p_marker) {
    Node2D *made = memnew(Node2D);
    made->set_name("Avatar");
    made->set_meta(StringName("marker"), p_marker);
    if (avatar_listener() != nullptr) {
        NetwEntity::ensure(made)->connect(
            StringName("control_changed"),
            avatar_listener()->callable("changed").bind(
                int64_t(made->get_instance_id())
            )
        );
    }
    return made;
}

Node *build_level(const Variant &p_name) {
    Node2D *made = memnew(Node2D);
    made->set_name(String(p_name));
    Node2D *carrier = memnew(Node2D);
    carrier->set_name("Carrier");
    made->add_child(carrier);
    const Ref<NetwEntity> facet = NetwEntity::ensure(made);
    REQUIRE(facet.is_valid());
    facet->set_declares_scene(true);
    facet->set_scene_label(StringName(String(p_name)));
    return made;
}

Array one_string(const String &p_value) {
    Array out;
    out.push_back(p_value);
    return out;
}

Array string_types() {
    Array out;
    out.push_back(int(Variant::STRING));
    return out;
}

RID open_scene(LoopbackRig &p_rig, const StringName &p_name, Node *p_under) {
    const int route = p_rig.spawn_registered(
        StringName("level"),
        callable_mp_static(&build_level),
        one_string(String(p_name)),
        string_types(),
        p_under != nullptr ? p_under : p_rig.branch(-1)
    );
    REQUIRE(route > 0);
    const RID scene = p_rig.spawned_entity();
    p_rig.pump(6);
    REQUIRE(p_rig.server()->scene_is_declared(scene));
    return scene;
}

Node *level_of(LoopbackRig &p_rig, const RID &p_scene) {
    Node *held = p_rig.server()->entity_get_node(p_scene);
    REQUIRE(held != nullptr);
    return held;
}

int spawn_into(
    LoopbackRig &p_rig,
    Node *p_parent,
    const Ref<netw::NetwPlayer> &p_owner,
    const char *p_marker,
    bool p_pump = true
) {
    return p_rig.spawn_registered(
        StringName("avatar"),
        callable_mp_static(&build_avatar),
        one_string(p_marker),
        string_types(),
        p_parent,
        p_owner.is_valid() ? p_owner->get_id() : 0,
        p_pump
    );
}

TEST_CASE(
    "[Netw][Scene] JR2 two entities of one player in one scene are two "
    "reasons, so the first one leaving keeps the player subscribed and the "
    "last one leaving takes the subscription away"
) {
    LoopbackRig rig(1);
    rig.mount();
    NetwMultiplayer *host = rig.server();
    const RID arena = open_scene(rig, StringName("Arena"), nullptr);
    Node *level = level_of(rig, arena);
    const int peer = rig.peer_id(0);
    const Ref<netw::NetwPlayer> ana
        = netw_test::seated_peer(host, peer, StringName("ana"));

    const int first = spawn_into(rig, level, ana, "first");
    const int second = spawn_into(rig, level, ana, "second");
    Node *first_node = rig.route_node(first);
    Node *second_node = rig.route_node(second);
    REQUIRE(first_node != nullptr);
    REQUIRE(second_node != nullptr);
    NETW_CHECK_EQ(Internals::membership_book(host).entities_in(peer, arena), 2);

    first_node->get_parent()->remove_child(first_node);
    rig.pump(8);

    NETW_CHECK_EQ(Internals::membership_book(host).entities_in(peer, arena), 1);
    CHECK(host->scene_subscribes(arena, peer));

    second_node->get_parent()->remove_child(second_node);
    rig.pump(8);

    NETW_CHECK_EQ(Internals::membership_book(host).entities_in(peer, arena), 0);
    CHECK_FALSE(host->scene_subscribes(arena, peer));

    memdelete(first_node);
    memdelete(second_node);
    rig.pump(4);
}

TEST_CASE(
    "[Netw][Scene] JR11 a player entity moved into another scene reports "
    "its controller on its client as it did when it first arrived"
) {
    LoopbackRig rig(1);
    rig.mount();
    const RID arena = open_scene(rig, StringName("Arena"), nullptr);
    const RID annex = open_scene(rig, StringName("Annex"), nullptr);
    const Ref<netw::NetwPlayer> ana = netw_test::seated_peer(
        rig.server(),
        rig.peer_id(0),
        StringName("ana")
    );
    netw_test::CallLog log;
    avatar_listener() = &log;
    const int route = spawn_into(rig, level_of(rig, arena), ana, "steered");
    const int64_t first = int64_t(rig.route_node(route, 0)->get_instance_id());
    rig.route_node(route)->reparent(level_of(rig, annex));
    rig.pump(8);
    avatar_listener() = nullptr;
    const int64_t moved = int64_t(rig.route_node(route, 0)->get_instance_id());
    int on_first = 0;
    int on_moved = 0;
    for (int at = 0; at < log.count("changed"); at++) {
        const int64_t heard = int64_t(log.args("changed", at)[2]);
        on_first += heard == first ? 1 : 0;
        on_moved += heard == moved ? 1 : 0;
    }
    REQUIRE(on_first > 0);
    NETW_CHECK_EQ(on_moved, on_first);
}

TEST_CASE(
    "[Netw][Scene] JR12 a player entity spawned under its controller "
    "reports control_changed once on every peer"
) {
    LoopbackRig rig(1);
    rig.mount();
    const RID arena = open_scene(rig, StringName("Arena"), nullptr);
    const Ref<netw::NetwPlayer> ana = netw_test::seated_peer(
        rig.server(),
        rig.peer_id(0),
        StringName("ana")
    );
    netw_test::CallLog log;
    avatar_listener() = &log;
    const int route = spawn_into(rig, level_of(rig, arena), ana, "steered");
    rig.pump(8);
    avatar_listener() = nullptr;
    for (const int peer : {-1, 0}) {
        const int64_t node
            = int64_t(rig.route_node(route, peer)->get_instance_id());
        int heard = 0;
        for (int at = 0; at < log.count("changed"); at++) {
            heard += int64_t(log.args("changed", at)[2]) == node ? 1 : 0;
        }
        NETW_CHECK_EQ(heard, 1);
    }
}

void rebuild_while_draining(
    NetwMultiplayer *p_host,
    NetwEntity *p_entity,
    const RID &p_handle
) {
    p_entity->announce_reparented(p_host);
    Internals::adopt_handle(Ref<NetwEntity>(p_entity), p_handle);
    p_host->scene_on_entity_live(p_handle);
}

TEST_CASE(
    "[Netw][Scene] JR13 an entity whose handle changes with a report "
    "queued stays in its scene and does not enter it again"
) {
    LoopbackRig rig(0);
    rig.mount();
    NetwMultiplayer *host = rig.server();
    const RID arena = open_scene(rig, StringName("Arena"), nullptr);
    const int route = spawn_into(
        rig,
        level_of(rig, arena),
        Ref<netw::NetwPlayer>(),
        "crate"
    );
    const int other = spawn_into(
        rig,
        level_of(rig, arena),
        Ref<netw::NetwPlayer>(),
        "other"
    );
    const Ref<NetwEntity> entity = NetwEntity::ensure(rig.route_node(route));
    const Ref<NetwEntity> neighbour
        = NetwEntity::ensure(rig.route_node(other));
    netw_test::CallLog log;
    host->scene_handle_of(arena)->connect(
        StringName("entity_entered"),
        log.callable("entered")
    );

    const RID prior = entity->get_rid_handle();
    netw::entity::retain(prior);
    const RID handle = netw::entity::mint();
    neighbour->connect(
        StringName("reparented"),
        callable_mp_static(&rebuild_while_draining)
            .bind(host, entity.ptr(), handle),
        Object::CONNECT_ONE_SHOT
    );
    neighbour->announce_reparented(host);
    rig.pump(2);
    const int entered = log.count("entered");
    Internals::adopt_handle(entity, prior);
    host->scene_on_entity_live(prior);
    netw::entity::release(handle);
    netw::entity::release(prior);
    rig.pump(2);

    NETW_CHECK_EQ(entered, 0);
}

TEST_CASE(
    "[Netw][Scene] JR3 an entity that leaves a scene and returns inside "
    "one settle never reports leaving it, because a move is classified from "
    "the tree the entity ended in rather than from the hops it took"
) {
    LoopbackRig rig(1);
    rig.mount();
    NetwMultiplayer *host = rig.server();
    const RID arena = open_scene(rig, StringName("Arena"), nullptr);
    const RID annex = open_scene(rig, StringName("Annex"), nullptr);
    const int peer = rig.peer_id(0);
    const Ref<netw::NetwPlayer> ana
        = netw_test::seated_peer(host, peer, StringName("ana"));

    const int route = spawn_into(rig, level_of(rig, arena), ana, "mover");
    Node *avatar = rig.route_node(route);
    REQUIRE(avatar != nullptr);
    REQUIRE(host->scene_subscribes(arena, peer));

    const Ref<netw::NetwScene> left = host->scene_handle_of(arena);
    const Ref<netw::NetwScene> passed = host->scene_handle_of(annex);
    REQUIRE(left.is_valid());
    REQUIRE(passed.is_valid());
    const Recorder exits(
        left.ptr(),
        Vector<StringName>{StringName("viewer_exited")}
    );
    const Recorder enters(
        passed.ptr(),
        Vector<StringName>{StringName("viewer_entered")}
    );

    avatar->reparent(level_of(rig, annex));
    avatar->reparent(level_of(rig, arena));
    rig.pump(8);

    NETW_CHECK_EQ(exits.count(StringName("viewer_exited")), 0);
    NETW_CHECK_EQ(enters.count(StringName("viewer_entered")), 0);
    CHECK(host->scene_subscribes(arena, peer));
    CHECK_FALSE(host->scene_subscribes(annex, peer));
    NETW_CHECK_EQ(Internals::membership_book(host).entities_in(peer, arena), 1);

    avatar->reparent(level_of(rig, annex));
    rig.pump(8);

    NETW_CHECK_EQ(exits.count(StringName("viewer_exited")), 1);
    NETW_CHECK_EQ(enters.count(StringName("viewer_entered")), 1);
}

TEST_CASE(
    "[Netw][Scene] JR4 moving an ordinary parent moves every represented "
    "descendant inside it, so both players it carried subscribe to where the "
    "parent landed and neither is left subscribed to where it was"
) {
    LoopbackRig rig(2);
    rig.mount();
    NetwMultiplayer *host = rig.server();
    const RID arena = open_scene(rig, StringName("Arena"), nullptr);
    const RID annex = open_scene(rig, StringName("Annex"), nullptr);
    const int first = rig.peer_id(0);
    const int second = rig.peer_id(1);
    const Ref<netw::NetwPlayer> ana
        = netw_test::seated_peer(host, first, StringName("ana"));
    const Ref<netw::NetwPlayer> bo
        = netw_test::seated_peer(host, second, StringName("bo"));

    Node *carrier = level_of(rig, arena)->get_node_or_null("Carrier");
    REQUIRE(carrier != nullptr);

    spawn_into(rig, carrier, ana, "ana");
    spawn_into(rig, carrier, bo, "bo");
    REQUIRE(host->scene_subscribes(arena, first));
    REQUIRE(host->scene_subscribes(arena, second));

    Node *vacant = level_of(rig, annex)->get_node_or_null("Carrier");
    REQUIRE(vacant != nullptr);
    memdelete(vacant);
    carrier->reparent(level_of(rig, annex));
    rig.pump(10);

    CHECK(host->scene_subscribes(annex, first));
    CHECK(host->scene_subscribes(annex, second));
    CHECK_FALSE(host->scene_subscribes(arena, first));
    CHECK_FALSE(host->scene_subscribes(arena, second));
}

TEST_CASE(
    "[Netw][Scene] JR5 an explicit watch outlives the entity that "
    "happened to be there, because a departure drops the entity's own reason "
    "and never the reason a caller asked for"
) {
    LoopbackRig rig(1);
    rig.mount();
    NetwMultiplayer *host = rig.server();
    const RID arena = open_scene(rig, StringName("Arena"), nullptr);
    const RID annex = open_scene(rig, StringName("Annex"), nullptr);
    const int peer = rig.peer_id(0);
    const Ref<netw::NetwPlayer> ana
        = netw_test::seated_peer(host, peer, StringName("ana"));

    const int route = spawn_into(rig, level_of(rig, arena), ana, "mover");
    Node *avatar = rig.route_node(route);
    REQUIRE(avatar != nullptr);
    REQUIRE(host->scene_admit_peer(arena, peer));
    REQUIRE(host->scene_watches(arena, peer));

    avatar->reparent(level_of(rig, annex));
    rig.pump(8);

    CHECK(host->scene_watches(arena, peer));
    CHECK(host->scene_subscribes(arena, peer));
    NETW_CHECK_EQ(Internals::membership_book(host).entities_in(peer, arena), 0);
    CHECK(host->scene_subscribes(annex, peer));
}

TEST_CASE(
    "[Netw][Scene] JR6 an entity parented under another "
    "session's tree is refused rather than adopted, so the session it left "
    "revokes its "
    "membership and the destination claims nothing"
) {
    netw_test::FaultWatch expected_faults;
    expected_faults.expect_error("*[NW0041]*", 1);

    LoopbackRig rig(1);
    rig.mount();
    NetwMultiplayer *host = rig.server();
    const RID arena = open_scene(rig, StringName("Arena"), nullptr);
    const int peer = rig.peer_id(0);
    const Ref<netw::NetwPlayer> ana
        = netw_test::seated_peer(host, peer, StringName("ana"));

    const int route = spawn_into(rig, level_of(rig, arena), ana, "mover");
    Node *avatar = rig.route_node(route);
    REQUIRE(avatar != nullptr);
    REQUIRE(host->scene_subscribes(arena, peer));

    Node *foreign = rig.route_node(int(host->entity_get_route(arena)), 0);
    REQUIRE(foreign != nullptr);
    avatar->reparent(foreign);
    rig.pump(8);

    CHECK_FALSE(host->scene_subscribes(arena, peer));
    NETW_CHECK_EQ(Internals::membership_book(host).subscriptions_of(peer), 0);

    avatar->reparent(level_of(rig, arena));
    rig.pump(8);
    CHECK(host->scene_subscribes(arena, peer));
}

TEST_CASE(
    "[Netw][Scene] JR7 an entity whose player holds no live "
    "membership subscribes nobody when it lands, because an armed orphan can "
    "outlive the acceptance that asked for it"
) {
    LoopbackRig rig(1);
    rig.mount();
    NetwMultiplayer *host = rig.server();
    const RID arena = open_scene(rig, StringName("Arena"), nullptr);
    const int peer = rig.peer_id(0);
    const Ref<netw::NetwPlayer> ana
        = netw_test::seated_peer(host, peer, StringName("ana"));

    const int route
        = spawn_into(rig, level_of(rig, arena), ana, "orphan", false);
    NETW_CHECK_GT(route, 0);
    host->player_forget(ana->get_id());
    REQUIRE_FALSE(host->player_has(peer));

    rig.pump(8);

    CHECK_FALSE(host->scene_subscribes(arena, peer));
    NETW_CHECK_EQ(Internals::membership_book(host).entities_in(peer, arena), 0);
}

TEST_CASE(
    "[Netw][Scene] JR9 watching a nested scene delivers that scene and "
    "the structural ancestors it hangs from, because a scene root has to "
    "exist on a receiver before anything inside it can"
) {
    LoopbackRig rig(1);
    rig.mount();
    NetwMultiplayer *host = rig.server();
    const RID outer = open_scene(rig, StringName("Outer"), nullptr);
    const RID inner
        = open_scene(rig, StringName("Inner"), level_of(rig, outer));
    const int peer = rig.peer_id(0);
    REQUIRE(netw_test::seated_peer(host, peer, StringName("ana")).is_valid());
    const int inner_body = spawn_into(
        rig,
        level_of(rig, inner),
        Ref<netw::NetwPlayer>(),
        "inside"
    );

    REQUIRE(host->scene_admit_peer(inner, peer));
    rig.pump(10);

    CHECK(host->scene_subscribes(inner, peer));
    CHECK_FALSE(host->scene_subscribes(outer, peer));
    CHECK(rig.route_node(int(host->entity_get_route(inner)), 0) != nullptr);
    CHECK(rig.route_node(int(host->entity_get_route(outer)), 0) != nullptr);
    CHECK(rig.route_node(inner_body, 0) != nullptr);
}

TEST_CASE(
    "[Netw][Scene] JR10 a nested watch reaches nothing else the ancestor "
    "holds, so the sibling content of an outer scene stays with the peers the "
    "outer scene admits and the nested watcher never sees it"
) {
    LoopbackRig rig(3);
    rig.mount();
    NetwMultiplayer *host = rig.server();
    const RID outer = open_scene(rig, StringName("Outer"), nullptr);
    const RID inner
        = open_scene(rig, StringName("Inner"), level_of(rig, outer));
    const int nested = rig.peer_id(0);
    const int whole = rig.peer_id(1);
    const int outside = rig.peer_id(2);
    REQUIRE(netw_test::seated_peer(host, nested, StringName("ana")).is_valid());
    REQUIRE(netw_test::seated_peer(host, whole, StringName("bo")).is_valid());
    REQUIRE(netw_test::seated_peer(host, outside, StringName("cy")).is_valid());

    const int sibling = spawn_into(
        rig,
        level_of(rig, outer),
        Ref<netw::NetwPlayer>(),
        "sibling"
    );

    REQUIRE(host->scene_admit_peer(inner, nested));
    REQUIRE(host->scene_admit_peer(outer, whole));
    rig.pump(12);

    CHECK(rig.route_node(int(host->entity_get_route(inner)), 0) != nullptr);
    CHECK(rig.route_node(sibling, 0) == nullptr);
    CHECK(rig.route_node(sibling, 1) != nullptr);
    CHECK(rig.route_node(sibling, 2) == nullptr);
    CHECK(rig.route_node(int(host->entity_get_route(inner)), 2) == nullptr);
    CHECK_FALSE(host->scene_subscribes(outer, nested));
    CHECK_FALSE(host->scene_subscribes(inner, whole));
}

TEST_CASE(
    "[Netw][Scene] JR8 the run's placements agree with the tree, so "
    "every entity reason names an entity that resides in that scene and "
    "every resident entity holds one"
) {
#if defined(NETW_TESTS)
    const int64_t opened = netw::residency_audit::ledger().observations;
#endif
    LoopbackRig rig(2);
    rig.mount();
    NetwMultiplayer *host = rig.server();
    const RID arena = open_scene(rig, StringName("Arena"), nullptr);
    const RID annex = open_scene(rig, StringName("Annex"), nullptr);
    const Ref<netw::NetwPlayer> ana
        = netw_test::seated_peer(host, rig.peer_id(0), StringName("ana"));
    const Ref<netw::NetwPlayer> bo
        = netw_test::seated_peer(host, rig.peer_id(1), StringName("bo"));

    const int mover = spawn_into(rig, level_of(rig, arena), ana, "ana");
    spawn_into(rig, level_of(rig, annex), bo, "bo");
    Node *avatar = rig.route_node(mover);
    REQUIRE(avatar != nullptr);

    avatar->reparent(level_of(rig, annex));
    rig.pump(8);
    avatar->reparent(level_of(rig, arena));
    rig.pump(8);

    spawn_into(rig, level_of(rig, annex), ana, "ana-twin", false);
    spawn_into(rig, level_of(rig, annex), bo, "bo-twin", false);
    rig.pump(8);

#if defined(NETW_TESTS)
    const int64_t audited
        = netw::residency_audit::ledger().observations - opened;
    NETW_CHECK_GT(int(audited), 0);
    NETW_CHECK_EQ(netw::residency_audit::ledger().divergences, 0);
#endif
}

} // namespace TestSceneResidencyLaws

#endif
