#include "tests/compliance/laws.h"

#include "netw/api/simulation.hpp"

#if defined(NETW_TIER_HOSTED)

namespace TestClaim {

using namespace godot;
using namespace netw_test;

constexpr const char *GRABS = R"(
class Grip extends RigidBody3D:
	var grabbed: NetwPromise
	var released: NetwPromise
	var mode: int:
		get:
			return NetwEntity.of(self).simulation.mode
	var velocity: Vector3:
		get:
			return linear_velocity
	var grab_code: int:
		get:
			return grabbed.code if grabbed and grabbed.is_settled else -1
	var release_code: int:
		get:
			return released.code if released and released.is_settled else -1

	func _init() -> void:
		gravity_scale = 0.0
		collision_layer = 0
		collision_mask = 0
		linear_damp_mode = RigidBody3D.DAMP_MODE_REPLACE
		linear_damp = 0.0
		can_sleep = false
		var hull := CollisionShape3D.new()
		hull.shape = SphereShape3D.new()
		add_child(hull)
		var entity := Netw.configure_entity(self)
		entity.authority = NetwEntity.AUTHORITY_CONTROLLER
		entity.transfer = NetwEntity.TRANSFER_IMMEDIATE
		entity.simulation.replicas = NetwSimulation.REPLICAS_ACTIVE
		Netw.configure_property(self, &"position").broadcast()
		Netw.configure_property(self, &"linear_velocity").broadcast()

	func grab(push: Vector3) -> void:
		grabbed = NetwEntity.of(self).claim_control()
		linear_velocity = push

	func grab_softly(push: Vector3) -> void:
		grabbed = NetwEntity.of(self).claim_control(NetwEntity.HOLD_SOFT)
		linear_velocity = push

	func shove(to: Vector3) -> void:
		linear_velocity = to

	func settle() -> void:
		position = Vector3(10, 0, 0)
		linear_velocity = Vector3.ZERO
		released = NetwEntity.of(self).release_control()

	func hand_to(peer: int) -> void:
		released = NetwEntity.of(self).release_control(peer)


class Crate extends Grip:
	pass


class Box extends Grip:
	pass


class Sled extends Grip:
	pass


class Kite extends Grip:
	pass
)";

constexpr int SLOW = 0;
constexpr int FAST = 1;
constexpr int PEERS = 3;
constexpr int PUSHED = 20;
constexpr int GRABBED = 40;
constexpr int RELEASED = 70;
constexpr int WOKEN = RELEASED + 2;
constexpr int STOPPED = WOKEN + 30;
constexpr int SOFT = 80;
constexpr int TAKEN = 92;
constexpr int HANDED = 98;
constexpr int END = 170;
const Vector3 GRAB_A(0, 0, 4);
const Vector3 GRAB_B(0, 0, -4);
const Vector3 PUSH(12, 0, 0);
const Vector3 WAKE(0, 0, 3);
const Vector3 SIDE(4, 0, 0);

Ref<netw::NetwLinkConditions> lagging(int p_ticks) {
    const Ref<netw::NetwLinkConditions> conditions = link(43);
    conditions->set_latency_ms(p_ticks * TICK_MS);
    return conditions;
}

Step hand_back(const char *p_name) {
    return [=](Live &p_live, int) {
        if (Node *node = p_live.need(FAST, p_name)) {
            node->call("hand_to", p_live.rig.peer_id(p_live.index(SLOW)));
        }
    };
}

World grabs() {
    World world = World(GRABS, 2)
                      .at(1, impair(SLOW, lagging(10)))
                      .at(1, impair(FAST, lagging(2)));
    for (const char *name : {"Crate", "Box", "Sled", "Kite"}) {
        for (const char *fact : {"mode", "velocity", "position"}) {
            world.watch(name, fact);
        }
        for (const char *fact : {"grab_code", "release_code"}) {
            world.watch(name, fact);
        }
        world.hear(name, "control_changed").at(1, spawn(name, name));
    }
    for (const char *name : {"Box", "Sled"}) {
        world.at(1, control(name, SLOW))
            .at(PUSHED, on(SLOW, call(name, "shove", Array::make(PUSH))))
            .at(RELEASED, on(SLOW, call(name, "settle")));
    }
    return world
        .at(GRABBED, on(SLOW, call("Crate", "grab", Array::make(GRAB_A))))
        .at(GRABBED, on(FAST, call("Crate", "grab", Array::make(GRAB_B))))
        .at(WOKEN, on(SLOW, call("Sled", "grab", Array::make(WAKE))))
        .at(STOPPED, on(SLOW, call("Sled", "shove", Array::make(Vector3()))))
        .at(SOFT, on(SLOW, call("Kite", "grab_softly", Array::make(GRAB_A))))
        .at(TAKEN, on(FAST, call("Kite", "grab", Array::make(SIDE))))
        .at(HANDED, hand_back("Kite"));
}

NETW_FRAME_SCENARIO(WorldFrames, grabs_frames, &grabs, END);

Variant fact(
    const Seen &p_seen,
    int p_peer,
    const char *p_name,
    const char *p_fact,
    int p_tick = LAST
) {
    const Sight *held = p_seen.sight(p_peer, p_name, p_tick);
    return held != nullptr ? held->facts.get(p_fact, Variant()) : Variant();
}

LawVerdict near(
    const Seen &p_seen,
    int p_peer,
    const char *p_name,
    const char *p_fact,
    const Vector3 &p_value,
    double p_within,
    int p_tick = LAST
) {
    const int tick = p_tick == LAST ? p_seen.ticks : p_tick;
    const Variant held = fact(p_seen, p_peer, p_name, p_fact, tick);
    if (held.get_type() != Variant::VECTOR3
        || Vector3(held).distance_to(p_value) > p_within) {
        return law_broken(
            "%s reads '%s' %s as %s at tick %d, not near %s",
            who(p_peer).c_str(),
            p_name,
            p_fact,
            said(held).c_str(),
            tick,
            said(p_value).c_str()
        );
    }
    return law_held();
}

LawVerdict never(
    const Seen &p_seen,
    int p_peer,
    const char *p_name,
    const Vector3 &p_velocity,
    int p_from,
    int p_to
) {
    for (int tick = p_from; tick <= p_to; ++tick) {
        const Variant held = fact(p_seen, p_peer, p_name, "velocity", tick);
        if (held.get_type() == Variant::VECTOR3
            && Vector3(held).is_equal_approx(p_velocity)) {
            return law_broken(
                "%s moved '%s' at %s at tick %d",
                who(p_peer).c_str(),
                p_name,
                said(held).c_str(),
                tick
            );
        }
    }
    return law_held();
}

TEST_CASE(
    "[Networked][Sim][Frame] X09 two players grabbing one free body in one "
    "tick both run ahead, the first grab the server hears keeps it, and the "
    "loser takes the winner's state and never sends its own"
) {
    const Seen &seen = grabs_frames.seen();
    for (int client : {SLOW, FAST}) {
        NETW_HOLDS(reads(
            seen, client, "Crate", "mode",
            netw::NetwSimulation::MODE_AUTHORITY, GRABBED
        ));
    }
    NETW_HOLDS(reads(seen, FAST, "Crate", "grab_code", OK));
    NETW_HOLDS(reads(seen, SLOW, "Crate", "grab_code", ERR_UNAUTHORIZED));
    NETW_HOLDS(reads(
        seen, SLOW, "Crate", "mode", netw::NetwSimulation::MODE_ACTIVE
    ));
    for (int peer = SERVER; peer < PEERS - 1; ++peer) {
        NETW_HOLDS(reads(seen, peer, "Crate", "controller", "client 1"));
        NETW_HOLDS(near(seen, peer, "Crate", "velocity", GRAB_B, 1e-3));
        NETW_HOLDS(hears(seen, peer, "Crate", "control_changed", ONCE));
    }
    for (int peer : {SERVER, FAST}) {
        NETW_HOLDS(never(seen, peer, "Crate", GRAB_A, GRABBED, END));
    }
}

TEST_CASE(
    "[Networked][Sim][Frame] X11 a body released at rest stays where its "
    "releaser left it on every peer, and one that wakes while its release "
    "is pending stays with the peer that woke it"
) {
    const Seen &seen = grabs_frames.seen();
    const Vector3 rest(10, 0, 0);
    const Variant lagged = fact(seen, SERVER, "Box", "position", RELEASED);
    CHECK(Vector3(lagged).x < rest.x - 1.0);
    NETW_HOLDS(reads(seen, SLOW, "Box", "release_code", OK));
    NETW_HOLDS(reads(seen, SLOW, "Sled", "release_code", OK));
    NETW_HOLDS(reads(seen, SLOW, "Sled", "grab_code", OK));
    NETW_HOLDS(near(seen, SLOW, "Sled", "velocity", WAKE, 1e-3, STOPPED - 1));
    const Vector3 woke = fact(seen, SLOW, "Sled", "position", STOPPED);
    CHECK(woke.z > 0.3);
    for (int peer = SERVER; peer < PEERS - 1; ++peer) {
        NETW_HOLDS(reads(seen, peer, "Box", "controller", "the server"));
        NETW_HOLDS(near(seen, peer, "Box", "position", rest, 0.05));
        NETW_HOLDS(near(seen, peer, "Box", "velocity", Vector3(), 0.01));
        NETW_HOLDS(reads(seen, peer, "Sled", "controller", "client 0"));
        NETW_HOLDS(near(seen, peer, "Sled", "position", woke, 0.05));
    }
}

TEST_CASE(
    "[Networked][Sim][Frame] CI3 state a slow player sent before another "
    "player took the body is never installed once control comes back to "
    "the slow player"
) {
    const Seen &seen = grabs_frames.seen();
    NETW_HOLDS(reads(seen, SERVER, "Kite", "controller", "client 1", HANDED));
    for (int peer = SERVER; peer < PEERS - 1; ++peer) {
        NETW_HOLDS(reads(seen, peer, "Kite", "controller", "client 0"));
        NETW_HOLDS(near(seen, peer, "Kite", "velocity", SIDE, 1e-3));
    }
    NETW_HOLDS(never(seen, SERVER, "Kite", GRAB_A, HANDED, END));
}

} // namespace TestClaim

#endif
