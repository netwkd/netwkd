#include "tests/compliance/laws.h"

#include "godot/physics_body.hpp"

#if defined(NETW_TIER_HOSTED)

namespace TestStepping {

using namespace godot;
using namespace netw_test;

constexpr const char *YARD = R"(
var stepper: Stepper
var rock := Rock.new()
var steps: int:
	get:
		return stepper.steps if stepper else 0


func _ready() -> void:
	add_child(rock)


func install() -> void:
	stepper = Stepper.new()
	Netw.session(self).install_physics_stepper(get_world_3d(), stepper)


func lift() -> void:
	rock.position = Vector3(10, 8, 0)
	rock.linear_velocity = Vector3.ZERO


class Stepper extends NetwPhysicsStepper:
	var steps := 0

	func _can_step() -> bool:
		return true

	func _step(_space: RID, _delta: float) -> void:
		steps += 1


class Rock extends RigidBody3D:
	var height: int:
		get:
			return roundi(position.y * 1000.0)

	func _init() -> void:
		name = &"Rock"
		can_sleep = false
		position = Vector3(10, 8, 0)
		var hull := CollisionShape3D.new()
		hull.shape = SphereShape3D.new()
		add_child(hull)


class Ball extends RigidBody3D:
	var motion := 0

	func _init() -> void:
		gravity_scale = 0.0
		var hull := CollisionShape3D.new()
		hull.shape = SphereShape3D.new()
		add_child(hull)
		Netw.configure_entity(self)
		Netw.configure_property(self, &"position").state()
		Netw.configure_property(self, &"motion").input()


class Walker extends CharacterBody3D:
	var motion := 0

	func _init() -> void:
		var hull := CollisionShape3D.new()
		hull.shape = BoxShape3D.new()
		add_child(hull)
		collision_layer = 0
		var entity := Netw.configure_entity(self)
		entity.simulation.schedule = NetwSimulation.SCHEDULE_STEPPED
		Netw.configure_property(self, &"position").state()
		Netw.configure_property(self, &"motion").input()

	func _network_process(_delta: float, _tick: int, _is_fresh: bool) -> void:
		pass
)";

constexpr int DRIVER = 0;
constexpr int PEERS = 2;
constexpr int FRAMES_PER_TICK = 60 / TICKRATE;
constexpr int BARE = 15;
constexpr int INSTALLED = 16;
constexpr int SETTLED = 20;
constexpr int LIFTED = 25;
constexpr int LEFT = 40;
constexpr int RELEASED = 42;
constexpr int GONE = 60;

Variant policy(Node *p_ball) {
    const Ref<netw::NetwEntity> ball = netw::NetwEntity::of(p_ball);
    return int(netw::NetwMultiplayer::predict_resolve_mode(
        *ball->get_record(),
        ball->get_owner()
    ));
}

World yard() {
    World world = World(YARD, 1)
                      .paced(60)
                      .watch("Level", "steps")
                      .watch("Rock", "height")
                      .watch("Ball", "policy", &policy);
    for (const char *name : {"Ball", "Walker"}) {
        world.at(1, spawn(name, name))
            .at(1, control(name, DRIVER))
            .at(LEFT, despawn(name));
    }
    for (const char *verb : {"install", "lift"}) {
        const int tick = String(verb) == "install" ? INSTALLED : LIFTED;
        world.at(tick, call("Level", verb))
            .at(tick, on(DRIVER, call("Level", verb)));
    }
    return world;
}

NETW_FRAME_SCENARIO(WorldFrames, yard_frames, &yard, GONE);

int64_t fact(
    const Seen &p_seen,
    int p_peer,
    const char *p_name,
    const char *p_fact,
    int p_tick
) {
    const Sight *held = p_seen.sight(p_peer, p_name, p_tick);
    return held != nullptr ? int64_t(held->facts.get(p_fact, INT64_MIN))
                           : INT64_MIN;
}

LawVerdict rock(const Seen &p_seen, int p_from, int p_to, bool p_falls) {
    for (int peer = SERVER; peer < PEERS - 1; ++peer) {
        const int64_t from = fact(p_seen, peer, "Rock", "height", p_from);
        const int64_t to = fact(p_seen, peer, "Rock", "height", p_to);
        if (p_falls ? to >= from : to != from) {
            return law_broken(
                "%s reads Rock at %d at tick %d and %d at tick %d",
                who(peer).c_str(),
                int(from),
                p_from,
                int(to),
                p_to
            );
        }
    }
    return law_held();
}

LawVerdict steps(const Seen &p_seen, int p_from, int p_to, int p_steps) {
    const int64_t from = fact(p_seen, SERVER, "Level", "steps", p_from);
    const int64_t to = fact(p_seen, SERVER, "Level", "steps", p_to);
    if (to - from != p_steps) {
        return law_broken(
            "the server's stepper stepped %d times from tick %d to %d, not %d",
            int(to - from),
            p_from,
            p_to,
            p_steps
        );
    }
    return law_held();
}

LawVerdict warned(
    const Seen &p_seen,
    const char *p_pattern,
    int p_times,
    int p_others
) {
    int matched = 0;
    for (const String &line : p_seen.faults) {
        matched += line.match(p_pattern);
    }
    if (matched != p_times || p_seen.faults.size() != p_times + p_others) {
        return law_broken(
            "%d of %d faults matched '%s', not %d",
            matched,
            int(p_seen.faults.size()),
            p_pattern,
            p_times
        );
    }
    return law_held();
}

TEST_CASE(
    "[Netw][Predict][Frame] T1 a stepped body on a build that cannot "
    "step physics says so once on each peer, and physics keeps running"
) {
    const Seen &seen = yard_frames.seen();
    const bool steps_alone = netw::gd::can_step_space(3);
    NETW_HOLDS(warned(
        seen,
        "*'Walker'*[NW1032]*",
        steps_alone ? 0 : PEERS,
        netw::gd::can_flush_kinematic() ? 0 : PEERS
    ));
    NETW_HOLDS(rock(seen, 2, BARE, true));
    for (int peer : {SERVER, DRIVER}) {
        NETW_HOLDS(reads(
            seen,
            peer,
            "Ball",
            "policy",
            steps_alone ? netw::NetwPrediction::MODE_ROLLBACK
                        : netw::NetwPrediction::MODE_OFFSET,
            BARE
        ));
    }
}

TEST_CASE(
    "[Netw][Predict][Frame] T2 an installed stepper steps the space once "
    "a tick in place of the engine, so a stepped body replays and a loose "
    "body holds wherever the game puts it"
) {
    const Seen &seen = yard_frames.seen();
    for (int peer : {SERVER, DRIVER}) {
        NETW_HOLDS(reads(
            seen,
            peer,
            "Ball",
            "policy",
            netw::NetwPrediction::MODE_ROLLBACK,
            SETTLED
        ));
        NETW_HOLDS(reads(seen, peer, "Rock", "height", 8000, LEFT));
    }
    NETW_HOLDS(steps(seen, SETTLED, LEFT, (LEFT - SETTLED) / FRAMES_PER_TICK));
    NETW_HOLDS(rock(seen, SETTLED, LIFTED - 1, false));
}

TEST_CASE(
    "[Netw][Predict][Frame] T3 once the last stepped entity is "
    "despawned the engine steps the space again and the stepper rests"
) {
    const Seen &seen = yard_frames.seen();
    NETW_HOLDS(rock(seen, LEFT, GONE, true));
    NETW_HOLDS(steps(seen, RELEASED, GONE, 0));
}

constexpr const char *PIT = R"(
var rock := Rock.new()


func _ready() -> void:
	add_child(rock)


class Rock extends RigidBody3D:
	func _init() -> void:
		name = &"Rock"
		can_sleep = false
		position = Vector3(0, 8, 0)
		var hull := CollisionShape3D.new()
		hull.shape = SphereShape3D.new()
		add_child(hull)


class Car extends RigidBody3D:
	var motion := 0
	var mode: int:
		get:
			return NetwEntity.of(self).simulation.mode

	func _init() -> void:
		gravity_scale = 0.0
		var hull := CollisionShape3D.new()
		hull.shape = SphereShape3D.new()
		add_child(hull)
		var entity := Netw.configure_entity(self)
		entity.prediction.pause_on_unpredicted_contact = true
		Netw.configure_property(self, &"position").state()
		Netw.configure_property(self, &"motion").input()
)";

constexpr int FALLING = 20;
constexpr int LANDED = 80;

World pit() {
    return World(PIT, 1)
        .paced(60)
        .watch("Car", "mode")
        .at(1, spawn("Car", "Car"))
        .at(1, control("Car", DRIVER));
}

NETW_FRAME_SCENARIO(WorldFrames, pit_frames, &pit, LANDED);

LawVerdict pauses(const Seen &p_seen, int p_peer, const char *p_name) {
    for (const Sight &held : p_seen.sights) {
        if (held.peer == p_peer && held.name == String(p_name)
            && held.tick > FALLING
            && int(held.facts.get("mode", 0))
                == netw::NetwSimulation::MODE_PROXY) {
            return law_held();
        }
    }
    return law_broken(
        "%s kept predicting '%s' through the contact",
        who(p_peer).c_str(),
        p_name
    );
}

TEST_CASE(
    "[Netw][Predict][Frame] T4 a predicted body that pauses on an "
    "unpredicted contact follows the server once a body no peer replicates "
    "lands on it"
) {
    const Seen &seen = pit_frames.seen();
    NETW_HOLDS(reads(
        seen,
        DRIVER,
        "Car",
        "mode",
        netw::NetwSimulation::MODE_PREDICT,
        FALLING
    ));
    NETW_HOLDS(pauses(seen, DRIVER, "Car"));
}

} // namespace TestStepping

#endif
