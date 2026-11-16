#include "tests/compliance/laws.h"

#if defined(NETW_TIER_HOSTED)

namespace TestPredictCorrection {

using namespace godot;
using namespace netw_test;

constexpr const char *RELAY = R"(
class Part extends Node:
	var charge := 0

	func _init() -> void:
		name = &"Part"
		Netw.configure_property(self, &"charge").state()


class Kid extends Node3D:
	var beat := 0
	var motion := 0
	var part := Part.new()
	var gap: int:
		get:
			return beat - part.charge
	var bumped: bool:
		get:
			return beat > 100

	func _init() -> void:
		add_child(part)
		Netw.configure_entity(self)
		Netw.configure_property(self, &"beat").state()
		Netw.configure_property(self, &"motion").input()

	func _network_process(_delta: float, _tick: int, _is_fresh: bool) -> void:
		beat += 1
		part.charge += 1

	func bump() -> void:
		beat += 100
		part.charge += 100


class Sled extends Node3D:
	var motion := 0.0
	var lift: int:
		get:
			return roundi(position.y * 100000.0)
	var corrections: int:
		get:
			return NetwEntity.of(self).prediction.correction_count

	func _init() -> void:
		var entity := Netw.configure_entity(self)
		entity.prediction.mode = NetwPrediction.MODE_OFFSET
		entity.prediction.correction_threshold = 0.01
		entity.simulation.schedule = NetwSimulation.SCHEDULE_FRAME
		Netw.configure_property(self, &"position").state()
		Netw.configure_property(self, &"motion").input()

	func _network_process(delta: float, _tick: int, _is_fresh: bool) -> void:
		position.x += motion * 60.0 * delta

	func nudge() -> void:
		position.y += 0.05
)";

constexpr int DRIVER = 0;
constexpr int BUMPED = 20;
constexpr int NUDGED = 30;
constexpr int NUDGE = 5000;

World sleds(World p_world) {
    return p_world.watch("Sled", "lift")
        .watch("Sled", "corrections")
        .at(1, spawn("Sled", "Sled"))
        .at(1, control("Sled", DRIVER))
        .at(8, on(DRIVER, set("Sled", "motion", 1.0)))
        .at(NUDGED, call("Sled", "nudge"));
}

World relay() {
    return sleds(World(RELAY, 1)
                     .paced(60)
                     .watch("Kid", "gap")
                     .watch("Kid", "bumped")
                     .at(1, spawn("Kid", "Kid"))
                     .at(1, control("Kid", DRIVER))
                     .at(2, impair(DRIVER, polls(6)))
                     .at(BUMPED, call("Kid", "bump")));
}

World swift() {
    return sleds(World(RELAY, 1).paced(60));
}

NETW_FRAME_SCENARIO(WorldFrames, relay_frames, &relay, NUDGED + 60);
NETW_FRAME_SCENARIO(WorldFrames, swift_frames, &swift, NUDGED + 60);

LawVerdict never_above(
    const Seen &p_seen,
    int p_peer,
    const char *p_name,
    const char *p_fact,
    int64_t p_most
) {
    for (const Sight &held : p_seen.sights) {
        if (held.peer == p_peer && held.name == String(p_name)
            && int64_t(held.facts.get(p_fact, 0)) > p_most) {
            return law_broken(
                "%s reads '%s' %s as %s at tick %d, above %d",
                who(p_peer).c_str(),
                p_name,
                p_fact,
                said(held.facts[p_fact]).c_str(),
                held.tick,
                int(p_most)
            );
        }
    }
    return law_held();
}

TEST_CASE(
    "[Networked][Predict][Frame] C1 a client the server corrects agrees "
    "with it on a state property of a child node"
) {
    const Seen &seen = relay_frames.seen();
    NETW_HOLDS(reads(seen, SERVER, "Kid", "gap", 0));
    NETW_HOLDS(reads(seen, DRIVER, "Kid", "gap", 0));
    NETW_HOLDS(reads(seen, DRIVER, "Kid", "bumped", true));
}

TEST_CASE(
    "[Networked][Predict][Frame] C2 one server change to a moving frame "
    "body costs one correction, never overshoots, and the client then equals "
    "the server, over a slow link and over none"
) {
    for (const Seen *seen : {&relay_frames.seen(), &swift_frames.seen()}) {
        NETW_HOLDS(reads(*seen, SERVER, "Sled", "lift", NUDGE));
        NETW_HOLDS(reads(*seen, DRIVER, "Sled", "lift", NUDGE));
        NETW_HOLDS(reads(*seen, DRIVER, "Sled", "corrections", 1));
        NETW_HOLDS(never_above(*seen, DRIVER, "Sled", "lift", NUDGE));
    }
}

constexpr const char *CONVOY = R"(
class Walker extends CharacterBody3D:
	var motion := 0
	var speed := 0.0
	var replayed := {}
	var replayed_twice := false

	func _init() -> void:
		var hull := CollisionShape3D.new()
		hull.shape = BoxShape3D.new()
		add_child(hull)
		motion_mode = CharacterBody3D.MOTION_MODE_FLOATING
		Netw.configure_entity(self)
		Netw.configure_property(self, &"position").state().on_spawn()
		Netw.configure_property(self, &"speed").state().on_spawn()
		Netw.configure_property(self, &"motion").input()

	func _ready() -> void:
		collision_layer = 1 if multiplayer.is_server() else 2
		collision_mask = collision_layer

	func _network_process(_delta: float, tick: int, is_fresh: bool) -> void:
		velocity = Vector3(speed, 0.0, 0.0)
		move_and_slide()
		if not is_fresh:
			replayed_twice = replayed_twice or replayed.has(tick)
			replayed[tick] = position.x

	func join() -> void:
		var tail := NetwEntity.of(get_parent().get_node(^"Tail"))
		tail.simulation.remote_mode = NetwSimulation.REMOTE_MODE_ACTIVE
		NetwEntity.of(self).prediction.rollback_together = true

	func bump() -> void:
		position.x += 0.05


class Lead extends Walker:
	func _init() -> void:
		super()
		position = Vector3(1.5, 0, 0)
		speed = 1.5


class Tail extends Walker:
	func _init() -> void:
		super()
		speed = 3.0
)";

constexpr int JOINED = 4;
constexpr int SHOVED = 40;
constexpr double STRIDE = 3.0 / 60.0;

World convoy() {
    return World(CONVOY, 1)
        .watch("Lead", "replayed")
        .watch("Tail", "replayed")
        .watch("Tail", "replayed_twice")
        .at(1, spawn("Lead", "Lead"))
        .at(1, spawn("Tail", "Tail"))
        .at(1, control("Lead", DRIVER))
        .at(JOINED, call("Lead", "join"))
        .at(JOINED, on(DRIVER, call("Lead", "join")))
        .at(JOINED, impair(DRIVER, polls(6)))
        .at(SHOVED, call("Lead", "bump"));
}

NETW_FRAME_SCENARIO(WorldFrames, convoy_frames, &convoy, SHOVED + 30);

Dictionary replays(const Seen &p_seen, int p_peer, const char *p_name) {
    const Sight *held = p_seen.sight(p_peer, p_name);
    return held != nullptr ? Dictionary(held->facts["replayed"]) : Dictionary();
}

LawVerdict replays_together(const Seen &p_seen, int p_peer) {
    if (replays(p_seen, p_peer, "Lead").is_empty()) {
        return law_broken("%s never replayed 'Lead'", who(p_peer).c_str());
    }
    const Dictionary replayed = replays(p_seen, p_peer, "Tail");
    const Array ticks = replayed.keys();
    for (int at = 0; at < ticks.size(); ++at) {
        const Variant before = int64_t(ticks[at]) - 1;
        if (replayed.has(before)
            && double(replayed[ticks[at]]) - double(replayed[before])
                < STRIDE - 5e-3) {
            return law_held();
        }
    }
    return law_broken(
        "%s replayed 'Tail' over %d ticks and Lead never held it back",
        who(p_peer).c_str(),
        int(ticks.size())
    );
}

TEST_CASE(
    "[Networked][Predict][Frame] C4 a joint island replays the driven body "
    "and the body behind it together, and the one ahead holds the other back"
) {
    const Seen &seen = convoy_frames.seen();
    NETW_HOLDS(replays_together(seen, DRIVER));
    NETW_HOLDS(reads(seen, DRIVER, "Tail", "replayed_twice", true));
    NETW_HOLDS(quiet(seen));
}

constexpr const char *LOT = R"(
func admit() -> void:
	var lot := Netw.session(self).interest_layer(&"lot")
	for peer: int in multiplayer.get_peers():
		lot.add_viewer(peer)


class Car extends RigidBody3D:
	var motion := 0
	var mode: int:
		get:
			return NetwEntity.of(self).simulation.mode

	func _init() -> void:
		gravity_scale = 0.0
		collision_layer = 0
		collision_mask = 0
		var hull := CollisionShape3D.new()
		hull.shape = SphereShape3D.new()
		add_child(hull)
		Netw.configure_entity(self).interest.join(&"lot")
		Netw.configure_property(self, &"position").state()
		Netw.configure_property(self, &"motion").input()


class Parked extends Car:
	func _init() -> void:
		super()
		var simulation := Netw.configure_entity(self).simulation
		simulation.remote_mode = NetwSimulation.REMOTE_MODE_PROXY
)";

World lot() {
    return World(LOT, 1)
        .paced(60)
        .watch("Rival", "mode")
        .watch("Parked", "mode")
        .at(1, call("Level", "admit"))
        .at(3, spawn("Car", "Car"))
        .at(3, spawn("Car", "Rival"))
        .at(3, spawn("Parked", "Parked"))
        .at(5, control("Car", DRIVER));
}

NETW_FRAME_SCENARIO(WorldFrames, lot_frames, &lot, 40);

TEST_CASE(
    "[Networked][Predict][Frame] C5 a client predicting a physics body runs "
    "the other bodies it sees, and leaves one whose remote mode is proxy "
    "interpolated"
) {
    const Seen &seen = lot_frames.seen();
    const int active = netw::NetwSimulation::MODE_ACTIVE;
    const int proxy = netw::NetwSimulation::MODE_PROXY;
    NETW_HOLDS(reads(seen, DRIVER, "Rival", "mode", active));
    NETW_HOLDS(reads(seen, DRIVER, "Parked", "mode", proxy));
}

} // namespace TestPredictCorrection

#endif
