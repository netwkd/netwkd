#include "tests/compliance/laws.h"

#include "netw/api/simulation.hpp"

#if defined(NETW_TIER_HOSTED)

namespace TestSimulationFrame {

using namespace godot;
using namespace netw_test;

constexpr const char *YARD = R"(
var rock := Rock.new()


func _ready() -> void:
	add_child(rock)
	var floor := StaticBody2D.new()
	floor.position = Vector2(0, 200)
	var edge := CollisionShape2D.new()
	edge.shape = WorldBoundaryShape2D.new()
	floor.add_child(edge)
	add_child(floor)


class Puck extends RigidBody2D:
	var mode: int:
		get:
			return NetwEntity.of(self).simulation.mode
	var height: float:
		get:
			return position.y

	func _init() -> void:
		contact_monitor = true
		max_contacts_reported = 4
		var hull := CollisionShape2D.new()
		hull.shape = RectangleShape2D.new()
		add_child(hull)
		var entity := Netw.configure_entity(self)
		entity.simulation.replicas = NetwSimulation.REPLICAS_PROXY
		Netw.configure_property(self, &"position").broadcast()


class Kit:
	static func body(named: StringName, at: Vector3, layer: int) -> RigidBody3D:
		var made := RigidBody3D.new()
		made.name = named
		made.position = at
		made.gravity_scale = 0.0
		made.linear_damp_mode = RigidBody3D.DAMP_MODE_REPLACE
		made.linear_damp = 0.0
		made.collision_layer = layer
		made.collision_mask = layer
		var hull := CollisionShape3D.new()
		hull.shape = BoxShape3D.new()
		made.add_child(hull)
		return made


class Rock extends RigidBody3D:
	var height: float:
		get:
			return position.y

	func _init() -> void:
		name = &"Rock"
		can_sleep = false
		position = Vector3(10, 8, 0)
		var hull := CollisionShape3D.new()
		hull.shape = SphereShape3D.new()
		add_child(hull)


class Counter extends Node3D:
	var steps := 0
	var fresh := 0
	var twice := 0
	var skipped := 0
	var last := -1
	var mode: int:
		get:
			return NetwEntity.of(self).simulation.mode

	func count(tick: int, is_fresh: bool) -> void:
		steps += 1
		twice += int(tick == last)
		skipped += int(last >= 0 and tick > last + 1)
		last = tick
		fresh += int(is_fresh)


class Car extends Counter:
	var sphere := Kit.body(&"Sphere", Vector3.ZERO, 0)
	var tail := Kit.body(&"Tail", Vector3(0, 0, 2), 0)
	var x: float:
		get:
			return sphere.position.x
	var frozen: bool:
		get:
			var kinematic := RigidBody3D.FREEZE_MODE_KINEMATIC
			return (
				sphere.freeze and tail.freeze
				and sphere.freeze_mode == kinematic
				and tail.freeze_mode == kinematic
			)
	var thawed: bool:
		get:
			return not sphere.freeze and not tail.freeze
	var speed: int:
		get:
			return roundi(sphere.linear_velocity.x * 1000.0)
	var sphere_position: Vector3:
		get:
			return sphere.position
		set(value):
			sphere.position = value
	var sphere_velocity: Vector3:
		get:
			return sphere.linear_velocity
		set(value):
			sphere.linear_velocity = value
	var tail_position: Vector3:
		get:
			return tail.position
		set(value):
			tail.position = value
	var tail_velocity: Vector3:
		get:
			return tail.linear_velocity
		set(value):
			tail.linear_velocity = value

	func _init() -> void:
		sphere.can_sleep = false
		tail.can_sleep = false
		add_child(sphere)
		add_child(tail)
		var entity := Netw.configure_entity(self)
		entity.simulation.bodies = [^"Sphere", ^"Tail"]
		for property in [
			&"sphere_position", &"sphere_velocity",
			&"tail_position", &"tail_velocity",
		]:
			Netw.configure_property(self, property).broadcast()

	func activate() -> void:
		NetwEntity.of(self).simulation.replicas = NetwSimulation.REPLICAS_ACTIVE

	func _network_tick(_delta: float, tick: int, is_fresh: bool) -> void:
		count(tick, is_fresh)
		if is_fresh:
			sphere.linear_velocity = Vector3(3, 0, 0)
			tail.linear_velocity = Vector3(3, 0, 0)


class Beat extends Counter:
	var beat := 0

	func _init() -> void:
		var entity := Netw.configure_entity(self)
		entity.prediction.archetype = NetwPrediction.ARCHETYPE_SCRIPTED
		Netw.configure_property(self, &"beat").state()

	func _network_tick(_delta: float, tick: int, is_fresh: bool) -> void:
		count(tick, is_fresh)
		beat += 1


class Solver extends RigidBody3D:
	var mode: int:
		get:
			return NetwEntity.of(self).simulation.mode

	func _init() -> void:
		position = Vector3(-10, 0, 0)
		gravity_scale = 0.0
		collision_layer = 0
		collision_mask = 0
		var hull := CollisionShape3D.new()
		hull.shape = SphereShape3D.new()
		add_child(hull)
		var entity := Netw.configure_entity(self)
		entity.prediction.archetype = NetwPrediction.ARCHETYPE_SOLVER_BODY
		entity.simulation.schedule = NetwSimulation.SCHEDULE_FRAME
		Netw.configure_property(self, &"position").state()


class Ball extends RigidBody3D:
	var mode: int:
		get:
			return NetwEntity.of(self).simulation.mode
	var x: float:
		get:
			return position.x

	func _init() -> void:
		position = Vector3(0, 0, -10)
		gravity_scale = 0.0
		collision_layer = 0
		collision_mask = 0
		linear_damp_mode = RigidBody3D.DAMP_MODE_REPLACE
		linear_damp = 0.0
		angular_damp_mode = RigidBody3D.DAMP_MODE_REPLACE
		angular_damp = 0.0
		var hull := CollisionShape3D.new()
		hull.shape = SphereShape3D.new()
		add_child(hull)
		var entity := Netw.configure_entity(self)
		entity.simulation.replicas = NetwSimulation.REPLICAS_ACTIVE
		Netw.configure_property(self, &"position").state().heartbeat(30)
		Netw.configure_property(self, &"linear_velocity").state()

	func throw() -> void:
		linear_velocity = Vector3(3, 0, 0)

	func stop() -> void:
		linear_velocity = Vector3.ZERO

	func drift() -> void:
		position += Vector3(0, 0, 0.5)
		sleeping = true


class Pusher extends RigidBody3D:
	func place() -> Vector3:
		return Vector3(0, 0, 20)

	func _init() -> void:
		position = place()
		gravity_scale = 0.0
		collision_layer = 2
		collision_mask = 2
		var hull := CollisionShape3D.new()
		hull.shape = BoxShape3D.new()
		add_child(hull)
		var entity := Netw.configure_entity(self)
		entity.authority = NetwEntity.AUTHORITY_CONTROLLER
		entity.transfer = NetwEntity.TRANSFER_IMMEDIATE
		entity.simulation.replicas = NetwSimulation.REPLICAS_ACTIVE
		entity.simulation.claim_on_contact = true
		Netw.configure_property(self, &"position").broadcast()

	func shove() -> void:
		sleeping = false
		linear_velocity = Vector3(3, 0, 0)


class Mark extends Pusher:
	func place() -> Vector3:
		return Vector3(1.5, 0, 20)


class Tug extends Node3D:
	var sphere := Kit.body(&"Sphere", Vector3.ZERO, 4)
	var asleep: bool:
		get:
			return sphere.sleeping
	var sphere_position: Vector3:
		get:
			return sphere.position
		set(value):
			sphere.position = value

	func place() -> Vector3:
		return Vector3(0, 0, 30)

	func _init() -> void:
		position = place()
		add_child(sphere)
		var entity := Netw.configure_entity(self)
		entity.authority = NetwEntity.AUTHORITY_CONTROLLER
		entity.transfer = NetwEntity.TRANSFER_IMMEDIATE
		entity.simulation.bodies = [^"Sphere"]
		entity.simulation.replicas = NetwSimulation.REPLICAS_ACTIVE
		entity.simulation.claim_on_contact = true
		Netw.configure_property(self, &"sphere_position").broadcast()

	func shove() -> void:
		sphere.sleeping = false
		sphere.linear_velocity = Vector3(3, 0, 0)


class Peg extends Tug:
	func place() -> Vector3:
		return Vector3(1.5, 0, 30)
)";

constexpr int DRIVER = 0;
constexpr int RIDER = 1;
constexpr int PEERS = 3;
constexpr int THROWN = 4;
constexpr int FLYING = 20;
constexpr int PREDICTED = 30;
constexpr int LANDED = 50;
constexpr int STOPPED = 60;
constexpr int HANDED = 60;
constexpr int SETTLED = HANDED + 4;
constexpr int ACTIVE = 90;
constexpr int INSTALLING = ACTIVE + 14;
constexpr int WOKEN = 100;
constexpr int RESTING = 100;
constexpr int DRIFTED = 130;
constexpr int END = 220;
constexpr double STEP = 3.0 / 60.0;

Ref<netw::NetwLinkConditions> shaky() {
    const Ref<netw::NetwLinkConditions> conditions = link(29);
    conditions->set_latency_ms(10.0);
    conditions->set_jitter_ms(25.0);
    return conditions;
}

World yard() {
    World world = World(YARD, 2).paced(60).at(1, impair(RIDER, shaky()));
    for (const char *fact : {"mode", "steps", "fresh", "twice", "skipped"}) {
        world.watch("Car", fact).watch("Beat", fact);
    }
    for (const char *fact : {"x", "frozen", "thawed", "speed"}) {
        world.watch("Car", fact);
    }
    world.watch("Rock", "height")
        .watch("Solver", "mode")
        .watch("Solver", "freeze")
        .watch("Ball", "mode")
        .watch("Ball", "x")
        .watch("Ball", "position")
        .watch("Ball", "sleeping")
        .watch("Pusher", "sleeping")
        .watch("Tug", "asleep")
        .watch("Puck", "height")
        .watch("Puck", "mode");
    const char *spawned[] = {
        "Car", "Ball", "Pusher", "Mark", "Tug", "Peg", "Puck"
    };
    for (const char *name : spawned) {
        world.at(1, spawn(name, name));
    }
    for (const char *name : {"Beat", "Solver"}) {
        world.at(PREDICTED, spawn(name, name));
    }
    for (const char *name : {"Car", "Pusher", "Tug", "Puck"}) {
        world.at(1, control(name, DRIVER));
    }
    for (const char *name : {"Car", "Puck"}) {
        world.at(HANDED, control(name, RIDER));
    }
    return world.at(THROWN, call("Ball", "throw"))
        .at(STOPPED, call("Ball", "stop"))
        .at(ACTIVE, call("Car", "activate"))
        .at(WOKEN, on(DRIVER, call("Pusher", "shove")))
        .at(WOKEN, on(DRIVER, call("Tug", "shove")))
        .at(DRIFTED, on(DRIVER, call("Ball", "drift")));
}

NETW_FRAME_SCENARIO(WorldFrames, yard_frames, &yard, END);

constexpr int AUTHORITY = netw::NetwSimulation::MODE_AUTHORITY;
constexpr int ACTIVE_COPY = netw::NetwSimulation::MODE_ACTIVE;
constexpr int PROXY = netw::NetwSimulation::MODE_PROXY;

Variant fact(
    const Seen &p_seen,
    int p_peer,
    const char *p_name,
    const char *p_fact,
    int p_tick
) {
    const Sight *held = p_seen.sight(p_peer, p_name, p_tick);
    return held != nullptr ? held->facts.get(p_fact, Variant()) : Variant();
}

LawVerdict missing(int p_peer, const char *p_name, int p_tick) {
    return law_broken(
        "%s holds no '%s' at tick %d",
        who(p_peer).c_str(),
        p_name,
        p_tick
    );
}

LawVerdict gains(
    const Seen &p_seen,
    int p_peer,
    const char *p_name,
    const char *p_fact,
    int p_from,
    int p_to,
    int64_t p_gain
) {
    const Variant from = fact(p_seen, p_peer, p_name, p_fact, p_from);
    const Variant to = fact(p_seen, p_peer, p_name, p_fact, p_to);
    if (from.get_type() == Variant::NIL || to.get_type() == Variant::NIL) {
        return missing(p_peer, p_name, p_from);
    }
    const int64_t gained = int64_t(to) - int64_t(from);
    if (gained != p_gain) {
        return law_broken(
            "%s's '%s' %s rose by %d from tick %d to %d, not %d",
            who(p_peer).c_str(),
            p_name,
            p_fact,
            int(gained),
            p_from,
            p_to,
            int(p_gain)
        );
    }
    return law_held();
}

LawVerdict steps(
    const Seen &p_seen,
    int p_peer,
    const char *p_name,
    int p_from,
    int p_to,
    bool p_authored
) {
    const int64_t ran = int64_t(fact(p_seen, p_peer, p_name, "steps", p_to))
        - int64_t(fact(p_seen, p_peer, p_name, "steps", p_from));
    if (ran <= 0) {
        return law_broken(
            "%s never stepped '%s' from tick %d to %d",
            who(p_peer).c_str(),
            p_name,
            p_from,
            p_to
        );
    }
    for (const char *never : {"twice", "skipped"}) {
        const LawVerdict held
            = gains(p_seen, p_peer, p_name, never, p_from, p_to, 0);
        if (!held.held) {
            return held;
        }
    }
    return gains(
        p_seen,
        p_peer,
        p_name,
        "fresh",
        p_from,
        p_to,
        p_authored ? ran : 0
    );
}

LawVerdict glides(
    const Seen &p_seen,
    int p_peer,
    const char *p_name,
    const char *p_fact,
    int p_from,
    int p_to,
    double p_step
) {
    for (int tick = p_from + 1; tick <= p_to; ++tick) {
        const Variant was = fact(p_seen, p_peer, p_name, p_fact, tick - 1);
        const Variant is = fact(p_seen, p_peer, p_name, p_fact, tick);
        if (was.get_type() == Variant::NIL || is.get_type() == Variant::NIL) {
            return missing(p_peer, p_name, tick);
        }
        const double moved = double(is) - double(was);
        const bool held = p_step == 0.0 ? moved < 0.0
                                        : Math::abs(moved - p_step) < 0.01;
        if (!held) {
            return law_broken(
                "%s moved '%s' %s by %f at tick %d",
                who(p_peer).c_str(),
                p_name,
                p_fact,
                moved,
                tick
            );
        }
    }
    return law_held();
}

LawVerdict rests(
    const Seen &p_seen,
    int p_peer,
    const char *p_name,
    int p_from,
    int p_to
) {
    const double rest = fact(p_seen, p_peer, p_name, "height", p_from);
    for (int tick = p_from; tick <= p_to; ++tick) {
        const double at = fact(p_seen, p_peer, p_name, "height", tick);
        if (Math::abs(at - rest) > 1.0) {
            return law_broken(
                "%s holds '%s' at height %f at tick %d, off its rest at %f",
                who(p_peer).c_str(),
                p_name,
                at,
                tick,
                rest
            );
        }
    }
    return law_held();
}

LawVerdict stays(
    const Seen &p_seen,
    int p_peer,
    const char *p_name,
    const char *p_fact,
    const Variant &p_value,
    int p_from,
    int p_to
) {
    for (int tick = p_from; tick <= p_to; ++tick) {
        const LawVerdict held
            = reads(p_seen, p_peer, p_name, p_fact, p_value, tick);
        if (!held.held) {
            return held;
        }
    }
    return law_held();
}

LawVerdict beside(
    const Seen &p_seen,
    int p_peer,
    const char *p_name,
    int p_tick,
    bool p_near
) {
    const Vector3 at = fact(p_seen, p_peer, p_name, "position", p_tick);
    const Vector3 server = fact(p_seen, SERVER, p_name, "position", p_tick);
    if ((at.distance_to(server) < 1e-3) != p_near) {
        return law_broken(
            "%s holds '%s' at %s and the server at %s at tick %d",
            who(p_peer).c_str(),
            p_name,
            said(at).c_str(),
            said(server).c_str(),
            p_tick
        );
    }
    return law_held();
}

TEST_CASE(
    "[Networked][Sim][Frame] S1 only the peer that runs a body moves it, "
    "and every other copy holds all of its bodies frozen kinematic"
) {
    const Seen &seen = yard_frames.seen();
    const int before = HANDED - 1;
    NETW_HOLDS(reads(seen, DRIVER, "Car", "mode", AUTHORITY, before));
    NETW_HOLDS(reads(seen, DRIVER, "Car", "thawed", true, before));
    NETW_HOLDS(reads(seen, SERVER, "Solver", "mode", AUTHORITY));
    for (int peer : {SERVER, RIDER}) {
        NETW_HOLDS(reads(seen, peer, "Car", "mode", PROXY, before));
        NETW_HOLDS(reads(seen, peer, "Car", "frozen", true, before));
    }
    for (int peer : {DRIVER, RIDER}) {
        NETW_HOLDS(reads(seen, peer, "Solver", "mode", PROXY));
        NETW_HOLDS(reads(seen, peer, "Solver", "freeze", true));
    }
    for (int peer = SERVER; peer < PEERS - 1; ++peer) {
        NETW_HOLDS(glides(seen, peer, "Rock", "height", 2, END, 0.0));
    }
    NETW_HOLDS(quiet(seen));
}

TEST_CASE(
    "[Networked][Sim][Frame] S2 control passing to another peer thaws the "
    "new controller's bodies and freezes the old one's where they stand, "
    "and the body runs on at the speed its step sets"
) {
    const Seen &seen = yard_frames.seen();
    NETW_HOLDS(reads(seen, RIDER, "Car", "mode", AUTHORITY, SETTLED));
    NETW_HOLDS(reads(seen, RIDER, "Car", "thawed", true, SETTLED));
    NETW_HOLDS(reads(seen, RIDER, "Car", "speed", 3000, SETTLED));
    NETW_HOLDS(reads(seen, DRIVER, "Car", "mode", PROXY, SETTLED));
    NETW_HOLDS(reads(seen, DRIVER, "Car", "frozen", true, SETTLED));
    NETW_HOLDS(glides(seen, RIDER, "Car", "x", SETTLED, END, STEP));
    NETW_HOLDS(reads(seen, DRIVER, "Puck", "mode", PROXY, SETTLED));
    for (int peer = SERVER; peer < PEERS - 1; ++peer) {
        NETW_HOLDS(rests(seen, peer, "Puck", LANDED, END));
    }
}

TEST_CASE(
    "[Networked][Sim][Frame] S3 an active copy thaws, keeps the velocity it "
    "receives, and moves on evenly between the samples it installs, even "
    "over a jittery link"
) {
    const Seen &seen = yard_frames.seen();
    NETW_HOLDS(reads(seen, SERVER, "Car", "mode", ACTIVE_COPY, INSTALLING));
    NETW_HOLDS(reads(seen, SERVER, "Car", "thawed", true, INSTALLING));
    NETW_HOLDS(reads(seen, SERVER, "Car", "speed", 3000, INSTALLING));
    NETW_HOLDS(glides(seen, SERVER, "Car", "x", ACTIVE, END, STEP));
    for (int peer : {DRIVER, RIDER}) {
        NETW_HOLDS(reads(seen, peer, "Ball", "mode", ACTIVE_COPY, FLYING));
    }
    NETW_HOLDS(glides(seen, DRIVER, "Ball", "x", FLYING, STOPPED, STEP));
}

TEST_CASE(
    "[Networked][Sim][Frame] S4 a step runs once a tick on each peer that "
    "runs the body, fresh only where the body is authored, never on a "
    "frozen copy, and before anything in the session predicts"
) {
    const Seen &seen = yard_frames.seen();
    NETW_HOLDS(steps(seen, RIDER, "Car", SETTLED, END, true));
    NETW_HOLDS(steps(seen, SERVER, "Car", INSTALLING, END, false));
    NETW_HOLDS(gains(seen, DRIVER, "Car", "steps", SETTLED, END, 0));
    NETW_HOLDS(gains(seen, RIDER, "Car", "steps", 2, HANDED - 1, 0));
    NETW_HOLDS(steps(seen, DRIVER, "Car", 2, PREDICTED - 1, true));
    NETW_HOLDS(steps(seen, SERVER, "Beat", PREDICTED + 2, END, true));
    NETW_HOLDS(gains(seen, SERVER, "Beat", "steps", 60, 200, 70));
    for (int peer : {DRIVER, RIDER}) {
        NETW_HOLDS(gains(seen, peer, "Beat", "steps", PREDICTED + 4, END, 0));
    }
}

TEST_CASE(
    "[Networked][Sim][Frame] S5 a sleeping body a game wakes by writing "
    "sleeping false claims what it touches, as a root or a child body"
) {
    const Seen &seen = yard_frames.seen();
    NETW_HOLDS(reads(seen, DRIVER, "Pusher", "sleeping", true, WOKEN - 1));
    NETW_HOLDS(reads(seen, DRIVER, "Tug", "asleep", true, WOKEN - 1));
    for (const char *touched : {"Mark", "Peg"}) {
        NETW_HOLDS(
            reads(seen, SERVER, touched, "controller", "the server", WOKEN)
        );
        for (int peer = SERVER; peer < PEERS - 1; ++peer) {
            NETW_HOLDS(reads(seen, peer, touched, "controller", "client 0"));
        }
    }
}

TEST_CASE(
    "[Networked][Sim][Frame] S6 a resting copy the heartbeat agrees with "
    "stays asleep, and one a game moved returns to the server's pose"
) {
    const Seen &seen = yard_frames.seen();
    NETW_HOLDS(reads(seen, SERVER, "Ball", "sleeping", true, RESTING));
    NETW_HOLDS(stays(seen, RIDER, "Ball", "sleeping", true, RESTING, END));
    NETW_HOLDS(beside(seen, RIDER, "Ball", END, true));
    NETW_HOLDS(beside(seen, DRIVER, "Ball", DRIFTED + 1, false));
    NETW_HOLDS(beside(seen, DRIVER, "Ball", END, true));
}

} // namespace TestSimulationFrame

#endif
