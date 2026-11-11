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
		var entity := Netw.configure_entity(self)
		entity.prediction.archetype = NetwPrediction.ARCHETYPE_SCRIPTED
		Netw.configure_property(self, &"beat").state()
		Netw.configure_property(self, &"motion").input()

	func _network_tick(_delta: float, _tick: int, _is_fresh: bool) -> void:
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
			return NetwEntity.of(self).prediction.stats.corrections

	func _init() -> void:
		var entity := Netw.configure_entity(self)
		entity.prediction.archetype = NetwPrediction.ARCHETYPE_SCRIPTED
		entity.prediction.recovery_policy = (
				NetwPrediction.RECOVERY_POLICY_REBASE_RECOVER)
		entity.prediction.divergence_epsilon = 0.01
		entity.simulation.schedule = NetwSimulation.SCHEDULE_FRAME
		declare()
		Netw.configure_property(self, &"motion").input()

	func declare() -> void:
		Netw.configure_property(self, &"position").state()

	func _network_tick(delta: float, _tick: int, _is_fresh: bool) -> void:
		position.x += motion * 60.0 * delta

	func nudge() -> void:
		position.y += 0.05


class Ruled extends Sled:
	func declare() -> void:
		Netw.configure_property(self, &"position").state().carry_step(advance)

	func advance(value: Vector3, ctx: NetwPredictCarryContext) -> Vector3:
		var pace: float = ctx.input.get(&"motion", 0.0)
		return value + Vector3(pace * 60.0 * ctx.delta, 0, 0)


class Warner extends Node3D:
	var motion := 0.0

	func _init() -> void:
		var entity := Netw.configure_entity(self)
		entity.prediction.archetype = NetwPrediction.ARCHETYPE_SCRIPTED
		Netw.configure_property(self, &"position").state().carry_step(advance)
		Netw.configure_property(self, &"motion").input()

	func advance(value: Vector3, _ctx: NetwPredictCarryContext) -> Vector3:
		return value
)";

constexpr int DRIVER = 0;
constexpr int BUMPED = 20;
constexpr int NUDGED = 30;
constexpr int NUDGE = 5000;

World relay() {
    World world = World(RELAY, 1)
                      .paced(60)
                      .watch("Kid", "gap")
                      .watch("Kid", "bumped")
                      .at(1, spawn("Kid", "Kid"))
                      .at(1, spawn("Warner", "Warner"))
                      .at(1, control("Kid", DRIVER))
                      .at(2, impair(DRIVER, polls(6)))
                      .at(BUMPED, call("Kid", "bump"));
    for (const char *name : {"Sled", "Ruled"}) {
        world.watch(name, "lift")
            .watch(name, "corrections")
            .at(1, spawn(name, name))
            .at(1, control(name, DRIVER))
            .at(8, on(DRIVER, set(name, "motion", 1.0)))
            .at(NUDGED, call(name, "nudge"));
    }
    return world;
}

NETW_FRAME_SCENARIO(WorldFrames, relay_frames, &relay, NUDGED + 60);

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

LawVerdict warned_once(const Seen &p_seen, const char *p_pattern) {
    int matched = 0;
    for (const String &line : p_seen.faults) {
        matched += line.match(p_pattern);
    }
    if (matched != 1 || p_seen.faults.size() != 1) {
        return law_broken(
            "%d of %d faults matched '%s'",
            matched,
            int(p_seen.faults.size()),
            p_pattern
        );
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
    "the server"
) {
    const Seen &seen = relay_frames.seen();
    for (const char *name : {"Sled", "Ruled"}) {
        NETW_HOLDS(reads(seen, SERVER, name, "lift", NUDGE));
        NETW_HOLDS(reads(seen, DRIVER, name, "lift", NUDGE));
        NETW_HOLDS(reads(seen, DRIVER, name, "corrections", 1));
        NETW_HOLDS(never_above(seen, DRIVER, name, "lift", NUDGE));
    }
}

TEST_CASE(
    "[Networked][Predict][Frame] C3 a carry_step under the tick schedule is "
    "reported once, by the peer that drives the entity"
) {
    const Seen &seen = relay_frames.seen();
    NETW_HOLDS(
        warned_once(seen, "*'Warner'*carry_step under the tick schedule*")
    );
}

constexpr const char *CONVOY = R"(
class Walker extends CharacterBody3D:
	var motion := 0
	var speed := 0.0
	var replayed := {}

	func _init() -> void:
		var hull := CollisionShape3D.new()
		hull.shape = BoxShape3D.new()
		add_child(hull)
		motion_mode = CharacterBody3D.MOTION_MODE_FLOATING
		var entity := Netw.configure_entity(self)
		entity.prediction.archetype = NetwPrediction.ARCHETYPE_SCRIPTED
		Netw.configure_property(self, &"position").state().on_spawn()
		Netw.configure_property(self, &"speed").state().on_spawn()
		Netw.configure_property(self, &"motion").input()

	func _ready() -> void:
		collision_layer = 1 if multiplayer.is_server() else 2
		collision_mask = collision_layer

	func _network_tick(_delta: float, tick: int, is_fresh: bool) -> void:
		velocity = Vector3(speed, 0.0, 0.0)
		move_and_slide()
		if not is_fresh:
			replayed[tick] = position.x

	func join() -> void:
		var entity := NetwEntity.of(self)
		var tail := get_parent().get_node(^"Tail")
		entity.simulation.simulate(NetwEntity.of(tail))
		entity.prediction.reconcile_mode = NetwPrediction.RECONCILE_JOINT

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
    NETW_HOLDS(quiet(seen));
}

} // namespace TestPredictCorrection

#endif
