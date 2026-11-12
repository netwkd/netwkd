#include "tests/compliance/laws.h"

#include <godot_cpp/variant/utility_functions.hpp>

#if defined(NETW_TIER_HOSTED)

namespace CompliancePhysicsFrame {

using namespace godot;
using namespace netw_test;

real_t apart(const Variant &p_one, const Variant &p_other) {
    if (p_one.get_type() == Variant::TRANSFORM2D) {
        const Transform2D one = p_one;
        const Transform2D other = p_other;
        return MAX(
            one.get_origin().distance_to(other.get_origin()),
            (one.basis_xform(Vector2(1, 1)) - other.basis_xform(Vector2(1, 1)))
                .length()
        );
    }
    if (p_one.get_type() != Variant::TRANSFORM3D) {
        return (Vector3(p_one) - Vector3(p_other)).length();
    }
    const Transform3D one = p_one;
    const Transform3D other = p_other;
    real_t worst = one.origin.distance_to(other.origin);
    for (int axis = 0; axis < 3; ++axis) {
        const Vector3 skew
            = one.basis.get_column(axis) - other.basis.get_column(axis);
        worst = MAX(worst, skew.length());
    }
    return worst;
}

LawVerdict near(
    const Seen &p_seen,
    int p_peer,
    const char *p_name,
    const char *p_fact,
    int p_tick,
    int p_then_peer,
    int p_then
) {
    const int tick = p_tick == LAST ? p_seen.ticks : p_tick;
    const Sight *now = p_seen.sight(p_peer, p_name, tick);
    const Sight *then = p_seen.sight(p_then_peer, p_name, p_then);
    p_then = p_then == LAST ? p_seen.ticks : p_then;
    if (now == nullptr || then == nullptr) {
        return law_broken(
            "'%s' is missing at tick %d or %d",
            p_name,
            p_then,
            tick
        );
    }
    const Variant was = then->facts.get(p_fact, Variant());
    const Variant is = now->facts.get(p_fact, Variant());
    if (was.get_type() != is.get_type() || apart(was, is) > 1e-3) {
        return law_broken(
            "%s reads '%s' %s as %s at tick %d, and %s read %s at tick %d",
            who(p_peer).c_str(),
            p_name,
            p_fact,
            said(is).c_str(),
            tick,
            who(p_then_peer).c_str(),
            said(was).c_str(),
            p_then
        );
    }
    return law_held();
}

constexpr const char *ARENA = R"(
class Zone extends Area3D:
	func _init() -> void:
		name = &"Zone"
		var box := BoxShape3D.new()
		box.size = Vector3(8, 8, 8)
		var shape := CollisionShape3D.new()
		shape.shape = box
		add_child(shape)


class Kart extends Node3D:
	func _init() -> void:
		Netw.configure_entity(self)


class Player extends CharacterBody3D:
	func _init() -> void:
		Netw.configure_entity(self)
		Netw.configure_property(self, &"position").state()
		var shape := CollisionShape3D.new()
		shape.shape = SphereShape3D.new()
		add_child(shape)


func _init() -> void:
	add_child(Zone.new())
)";

constexpr int BOARDS = 16;
constexpr int LEAVES = 28;

World trigger_zone() {
    return World(ARENA, 2)
        .hear("Zone", "body_entered")
        .hear("Zone", "body_exited")
        .at(1, spawn("Kart", "Kart"))
        .at(1, spawn("Player", "Ada"))
        .at(BOARDS, reparent("Ada", "Kart"))
        .at(LEAVES, set("Ada", "position", Vector3(100, 0, 0)));
}

NETW_FRAME_SCENARIO(WorldFrames, trigger_zone_frames, &trigger_zone, 44);

TEST_CASE(
    "[Networked][Compliance][Frame] R2 reparenting an entity fires no area "
    "enter or exit during the move, and one real exit after it (godot#14578)"
) {
    const Seen &seen = trigger_zone_frames.seen();
    for (const int peer : {SERVER, 0, 1}) {
        NETW_HOLDS(reads(seen, peer, "Ada", "parent", "Level/Kart"));
        NETW_HOLDS(hears(seen, peer, "Zone", "body_entered", ONCE));
        NETW_HOLDS(hears(seen, peer, "Zone", "body_exited", NEVER, 1, LEAVES));
        NETW_HOLDS(hears(seen, peer, "Zone", "body_exited", ONCE, LEAVES + 1));
    }
    NETW_HOLDS(quiet(seen));
}

constexpr const char *TRACK = R"(
class Runner extends CharacterBody3D:
	var motion := 0
	var strides := 0
	var corrected := false
	var past_shove: bool:
		get:
			return position.x > 32.0

	func _init() -> void:
		var hull := CollisionShape3D.new()
		hull.shape = BoxShape3D.new()
		add_child(hull)
		collision_mask = 0
		var entity := Netw.configure_entity(self)
		entity.prediction.archetype = NetwPrediction.ARCHETYPE_SCRIPTED
		Netw.configure_property(self, &"position").state()
		Netw.configure_property(self, &"motion").input()

	func _network_tick(_delta: float, _tick: int, is_fresh: bool) -> void:
		if is_fresh:
			strides += motion
		else:
			corrected = true
		move_and_collide(Vector3(0.25 * motion, 0.0, 0.0))
		if is_fresh and strides == 40:
			motion = 0

	func shove() -> void:
		position.x += 64.0
)";

constexpr int SHOVED = 0;
constexpr int CLEAN = 1;
constexpr int SHOVE = 80;

World footrace() {
    return World(TRACK, 2)
        .paced(60)
        .watch("Ada", "position")
        .watch("Ada", "past_shove")
        .watch("Ada", "strides")
        .watch("Ada", "corrected")
        .watch("Bo", "position")
        .watch("Bo", "strides")
        .watch("Bo", "corrected")
        .at(1, spawn("Runner", "Ada"))
        .at(1, spawn("Runner", "Bo"))
        .at(2, impair(SHOVED, polls(2, TICK_MS)))
        .at(2, impair(CLEAN, polls(2, TICK_MS)))
        .at(4, control("Ada", SHOVED))
        .at(4, control("Bo", CLEAN))
        .at(40, on(SHOVED, set("Ada", "motion", 1)))
        .at(40, on(CLEAN, set("Bo", "motion", 1)))
        .at(SHOVE, call("Ada", "shove"));
}

NETW_FRAME_SCENARIO(WorldFrames, footrace_frames, &footrace, 180);

TEST_CASE(
    "[Networked][Compliance][Frame] L4 a predicting client the server "
    "disagrees with is corrected onto the server, a clean one is never "
    "corrected, and a replay repeats no one-shot effect"
) {
    const Seen &seen = footrace_frames.seen();
    NETW_HOLDS(reads(seen, SERVER, "Ada", "past_shove", true, SHOVE + 2));
    NETW_HOLDS(reads(seen, SHOVED, "Ada", "past_shove", false, SHOVE + 2));
    for (const int peer : {SERVER, SHOVED}) {
        NETW_HOLDS(reads(seen, peer, "Ada", "position", Vector3(74, 0, 0)));
        NETW_HOLDS(reads(seen, peer, "Ada", "strides", 40));
    }
    for (const int peer : {SERVER, CLEAN}) {
        NETW_HOLDS(reads(seen, peer, "Bo", "position", Vector3(10, 0, 0)));
        NETW_HOLDS(reads(seen, peer, "Bo", "strides", 40));
    }
    NETW_HOLDS(reads(seen, SHOVED, "Ada", "corrected", true));
    NETW_HOLDS(reads(seen, CLEAN, "Bo", "corrected", false));
    NETW_HOLDS(quiet(seen));
}

constexpr const char *MOMENTS = R"(
func spawn_all() -> void:
	var players := {}
	for player: NetwPlayer in Netw.session(self).players:
		players[player.username] = player
	var made: Array[Node] = [
		Netw.spawn(make_scene, "Probe", "Plain"),
		Netw.spawn_player(players["ana"], make_scene, "Probe", "Held"),
		Netw.spawn_player(players["bo"], make_scene, "Steered", "Steered"),
		Netw.spawn(make_scene, "Hasty", "Hasty"),
	]
	for node: Node in made:
		node.stamp = 41
		add_child(node)


class Probe extends Node:
	var stamp := 0
	var entered := ""
	var readied := -1
	var steady: bool:
		get:
			return entered == knows()

	func _init() -> void:
		var part := Node.new()
		part.name = &"Part"
		add_child(part)
		Netw.configure_property(self, &"stamp").on_spawn()
		Netw.configure_entity(self)

	func _enter_tree() -> void:
		entered = knows()

	func _ready() -> void:
		readied = stamp

	func knows() -> String:
		var e := NetwEntity.of(self)
		return "%s %d %d %d %d %s" % [
			name,
			e.player.peer_id if e.player != null else 0,
			e.controller.peer_id if e.controller != null else 1,
			e.authority,
			get_multiplayer_authority(),
			$Part.get_multiplayer_authority() == get_multiplayer_authority(),
		]


class Steered extends Probe:
	func _init() -> void:
		super()
		Netw.configure_entity(self).authority = NetwEntity.AUTHORITY_CONTROLLER


class Hasty extends Probe:
	func _init() -> void:
		super()
		Netw.configure_entity(self).transfer = NetwEntity.TRANSFER_IMMEDIATE
)";

constexpr int ANA = 1;
constexpr int BO = 2;

World moments() {
    World world = World(MOMENTS, 1).at(1, join("ana")).at(2, join("bo"));
    for (const char *name : {"Plain", "Held", "Steered", "Hasty"}) {
        world.watch(name, "steady").watch(name, "readied");
    }
    return world.at(4, call("Level", "spawn_all"));
}

NETW_FRAME_SCENARIO(WorldFrames, moments_frames, &moments, 12);

TEST_CASE(
    "[Networked][Compliance][Frame] S2 a spawned node knows its name, player, "
    "controller and authority in _enter_tree, and its spawn values in _ready"
) {
    const Seen &seen = moments_frames.seen();
    for (const int peer : {SERVER, 0, ANA, BO}) {
        for (const char *name : {"Plain", "Held", "Steered", "Hasty"}) {
            NETW_HOLDS(reads(seen, peer, name, "steady", true));
            NETW_HOLDS(reads(seen, peer, name, "readied", 41));
        }
        NETW_HOLDS(reads(seen, peer, "Plain", "controller", "the server"));
        NETW_HOLDS(reads(seen, peer, "Held", "controller", who(ANA).c_str()));
        NETW_HOLDS(reads(seen, peer, "Held", "authority", "the server"));
        NETW_HOLDS(reads(seen, peer, "Steered", "authority", who(BO).c_str()));
        NETW_HOLDS(reads(seen, peer, "Hasty", "authority", "the server"));
    }
    NETW_HOLDS(quiet(seen));
}

constexpr const char *CARRIER = R"(
class Avatar extends Node3D:
	func _init() -> void:
		Netw.configure_entity(self)
		position = Vector3(0, 1, 3)
		rotation = Vector3(0, PI / 2, 0)
		var hand := Node3D.new()
		hand.name = &"Hand"
		hand.position = Vector3(0.5, 0.2, -0.4)
		hand.rotation = Vector3(PI / 4, 0, 0)
		add_child(hand)


class Grip:
	static func hold(body: PhysicsBody3D) -> void:
		body.collision_mask = 0
		body.position = Vector3(0.3, 0, 0)
		var shape := CollisionShape3D.new()
		shape.shape = BoxShape3D.new()
		body.add_child(shape)
		var entity := Netw.configure_entity(body)
		entity.on_parent_despawn = NetwEntity.PARENT_DESPAWN_DETACH


class Cube extends RigidBody3D:
	func _init() -> void:
		gravity_scale = 0.0
		Grip.hold(self)


class Box extends CharacterBody3D:
	func _init() -> void:
		Grip.hold(self)
)";

constexpr int DROPPED = 8;

World carrier() {
    return World(CARRIER, 2)
        .watch("Cube", "global_transform")
        .watch("Box", "global_transform")
        .at(1, spawn("Avatar", "Avatar"))
        .at(3, spawn("Cube", "Cube", "Hand"))
        .at(3, spawn("Box", "Box", "Hand"))
        .at(DROPPED, despawn("Avatar"));
}

NETW_FRAME_SCENARIO(WorldFrames, carrier_frames, &carrier, 24);

TEST_CASE(
    "[Networked][Compliance][Frame] N3 a child marked to detach outlives its "
    "parent's despawn and stays where it was in the world"
) {
    const Seen &seen = carrier_frames.seen();
    for (const int peer : {SERVER, 0, 1}) {
        NETW_HOLDS(absent(seen, peer, "Avatar"));
        for (const char *held : {"Cube", "Box"}) {
            const char *hand = "Level/Avatar/Hand";
            const char *pose = "global_transform";
            NETW_HOLDS(reads(seen, peer, held, "parent", hand, DROPPED - 1));
            NETW_HOLDS(reads(seen, peer, held, "parent", "Level"));
            NETW_HOLDS(near(seen, peer, held, pose, LAST, SERVER, DROPPED - 1));
        }
    }
    NETW_HOLDS(quiet(seen));
}

constexpr const char *FERRY = R"(
class Platform extends Node3D:
	func _init() -> void:
		Netw.configure_entity(self)
		Netw.configure_property(self, &"position").state()
		Netw.configure_property(self, &"rotation").state()
		position = Vector3(2, 0, 0)
		rotation = Vector3(0, PI / 2, 0)

	func drift() -> void:
		position.x += 0.1
		rotation.y += 0.05


class Raft extends Node2D:
	func _init() -> void:
		Netw.configure_entity(self)
		Netw.configure_property(self, &"position").state()
		Netw.configure_property(self, &"rotation").state()
		position = Vector2(2, 0)
		rotation = PI / 2

	func drift() -> void:
		position.x += 0.1
		rotation += 0.05


class Crate extends Node3D:
	func _init() -> void:
		Netw.configure_entity(self)
		position = Vector3(0, 1, 0)


class Tile extends Node2D:
	func _init() -> void:
		Netw.configure_entity(self)
		position = Vector2(0, 1)


class Sled extends RigidBody3D:
	func _init() -> void:
		Netw.configure_entity(self)
		collision_mask = 0
		gravity_scale = 0.0
		linear_damp_mode = RigidBody3D.DAMP_MODE_REPLACE
		position = Vector3(0, 5, 0)
		linear_velocity = Vector3(3, 0, 0)
		var shape := CollisionShape3D.new()
		shape.shape = SphereShape3D.new()
		add_child(shape)
)";

constexpr int BOARD = 10;
constexpr int SAILS = 14;
constexpr int DOCKS = 24;

World ferry() {
    return World(FERRY, 2)
        .watch("Crate", "global_transform")
        .watch("Tile", "global_transform")
        .watch("Sled", "linear_velocity")
        .at(1, spawn("Platform", "Platform"))
        .at(1, spawn("Raft", "Raft"))
        .at(1, spawn("Crate", "Crate"))
        .at(1, spawn("Tile", "Tile"))
        .at(1, spawn("Sled", "Sled"))
        .at(BOARD, reparent("Crate", "Platform"))
        .at(BOARD, reparent("Tile", "Raft"))
        .at(BOARD, reparent("Sled", "Platform"))
        .each(SAILS, DOCKS, call("Platform", "drift"))
        .each(SAILS, DOCKS, call("Raft", "drift"));
}

NETW_FRAME_SCENARIO(WorldFrames, ferry_frames, &ferry, DOCKS + 12);

TEST_CASE(
    "[Networked][Compliance][Frame] R3 an entity reparented under a moving "
    "parent keeps its world pose and velocity on every peer"
) {
    const Seen &seen = ferry_frames.seen();
    const char *pose = "global_transform";
    for (const int peer : {SERVER, 0, 1}) {
        NETW_HOLDS(reads(seen, peer, "Crate", "parent", "Level/Platform"));
        NETW_HOLDS(reads(seen, peer, "Tile", "parent", "Level/Raft"));
        NETW_HOLDS(reads(seen, peer, "Sled", "parent", "Level/Platform"));
        for (const char *held : {"Crate", "Tile"}) {
            NETW_HOLDS(
                near(seen, peer, held, pose, SAILS - 1, peer, BOARD - 1)
            );
            NETW_HOLDS(near(seen, peer, held, pose, LAST, SERVER, LAST));
        }
    }
    NETW_HOLDS(
        reads(seen, SERVER, "Sled", "linear_velocity", Vector3(3, 0, 0))
    );
    NETW_HOLDS(quiet(seen));
}

LawVerdict same(
    const Seen &p_seen,
    int p_peer,
    const char *p_name,
    const char *p_fact,
    int p_other,
    const char *p_other_name
) {
    const Sight *held = p_seen.sight(p_other, p_other_name);
    if (held == nullptr) {
        return law_broken(
            "%s holds no '%s' at the end",
            who(p_other).c_str(),
            p_other_name
        );
    }
    return reads(p_seen, p_peer, p_name, p_fact, held->facts[p_fact]);
}

constexpr const char *RELAY = R"(
class Runner extends Node3D:
	var beat := 0
	var motion := 0
	var first_moved := -1
	var last_moved := -1
	var whole: bool:
		get:
			return beat == last_moved - first_moved + 1

	func _init() -> void:
		var e := Netw.configure_entity(self)
		e.prediction.archetype = NetwPrediction.ARCHETYPE_SCRIPTED
		Netw.configure_property(self, &"beat").state()
		Netw.configure_property(self, &"motion").input()

	func _network_tick(_delta: float, tick: int, is_fresh: bool) -> void:
		beat += motion
		if motion > 0 and is_fresh:
			last_moved = tick
			if first_moved < 0:
				first_moved = tick
)";

constexpr int RUNNER = 0;
constexpr int CY = 1;
constexpr int LATE = 12;
constexpr int STOP = 30;

World relay() {
    World world = World(RELAY, 1);
    for (const char *name : {"Early", "Late"}) {
        world.watch(name, "beat")
            .watch(name, "first_moved")
            .watch(name, "last_moved")
            .watch(name, "whole")
            .at(name[0] == 'E' ? 1 : LATE, spawn("Runner", name))
            .at(name[0] == 'E' ? 1 : LATE, control(name, RUNNER))
            .at(LATE + 2, on(RUNNER, set(name, "motion", 1)))
            .at(STOP, on(RUNNER, set(name, "motion", 0)));
    }
    return world.at(20, join("cy"));
}

NETW_FRAME_SCENARIO(WorldFrames, relay_frames, &relay, STOP + 14);

TEST_CASE(
    "[Networked][Compliance][Frame] P1 a predicted player spawned late runs "
    "from its first input, and a late joiner sees it where the server has it"
) {
    const Seen &seen = relay_frames.seen();
    for (const char *fact : {"first_moved", "last_moved"}) {
        NETW_HOLDS(same(seen, SERVER, "Late", fact, RUNNER, "Late"));
        NETW_HOLDS(same(seen, SERVER, "Late", fact, SERVER, "Early"));
        NETW_HOLDS(same(seen, RUNNER, "Late", fact, RUNNER, "Early"));
    }
    NETW_HOLDS(reads(seen, SERVER, "Late", "whole", true));
    NETW_HOLDS(reads(seen, SERVER, "Early", "whole", true));
    for (const char *name : {"Early", "Late"}) {
        NETW_HOLDS(same(seen, RUNNER, name, "beat", SERVER, name));
        NETW_HOLDS(same(seen, CY, name, "beat", SERVER, name));
        NETW_HOLDS(reads(seen, CY, name, "controller", who(RUNNER).c_str()));
    }
    NETW_HOLDS(quiet(seen));
}

constexpr const char *RANGE = R"(
var past := -1
var shots := []


func _init() -> void:
	Netw.configure_lagcomp(self)


func mark() -> void:
	past = Netw.session(self).clock.tick


func fire() -> void:
	var target: Node3D = find_child("Target", true, false)
	shots = Netw.rewind([NetwEntity.of(target)], past, func() -> Array:
		return [hits(0.0), hits(10.0)])
	shots.append(hits(10.0))


func hits(x: float) -> bool:
	var ray := PhysicsRayQueryParameters3D.create(
			Vector3(x, 5, 0), Vector3(x, -5, 0), 2)
	ray.collide_with_areas = true
	return not get_world_3d().direct_space_state.intersect_ray(ray).is_empty()


class Target extends CharacterBody3D:
	func _init() -> void:
		Netw.configure_entity(self)
		Netw.configure_property(self, &"position").state()
		collision_mask = 0
		var hull := CollisionShape3D.new()
		hull.shape = BoxShape3D.new()
		add_child(hull)
		var hitbox := Area3D.new()
		hitbox.collision_layer = 2
		hitbox.collision_mask = 0
		var box := CollisionShape3D.new()
		box.shape = BoxShape3D.new()
		hitbox.add_child(box)
		add_child(hitbox)
)";

constexpr int MOVES = 8;

World range() {
    return World(RANGE, 1)
        .watch("Level", "shots")
        .watch("Target", "position")
        .at(1, spawn("Target", "Target"))
        .at(4, call("Level", "mark"))
        .at(MOVES, set("Target", "position", Vector3(10, 0, 0)))
        .at(MOVES + 4, call("Level", "fire"));
}

NETW_FRAME_SCENARIO(WorldFrames, range_frames, &range, MOVES + 8);

TEST_CASE(
    "[Networked][Compliance][Frame] P2 a lag compensated shot hits where the "
    "target was, and the present is restored after (godot#76256)"
) {
    const Seen &seen = range_frames.seen();
    const Array shots = Array::make(true, false, true);
    NETW_HOLDS(reads(seen, SERVER, "Level", "shots", shots));
    NETW_HOLDS(reads(seen, SERVER, "Target", "position", Vector3(10, 0, 0)));
    NETW_HOLDS(quiet(seen));
}

LawVerdict freed(const Seen &p_seen, int p_peer) {
    const Sight *level = p_seen.sight(p_peer, "Level");
    const int64_t api = level != nullptr ? int64_t(level->facts["api"]) : 0;
    if (api == 0 || UtilityFunctions::is_instance_id_valid(api)) {
        return law_broken(
            "the session of %s outlived its world",
            who(p_peer).c_str()
        );
    }
    return law_held();
}

constexpr const char *HALL = R"(
var api := 0
var exit_reason := -1
var crate_at_exit := false


func _init() -> void:
	var post := Prop.new()
	post.name = &"Post"
	add_child(post)
	post.owner = self


func _ready() -> void:
	api = multiplayer.get_instance_id()
	Netw.session(self).exiting.connect(_on_exiting)


func _on_exiting(reason: int, _detail: String) -> void:
	exit_reason = reason
	var crate := find_child("Crate", true, false)
	crate_at_exit = crate != null and not crate.is_queued_for_deletion()


func kill() -> void:
	var opts := NetwDespawnOpts.create(&"closing")
	opts.linger = true
	opts.linger_seconds = 5.0
	NetwEntity.of(find_child("Ghost", true, false)).despawn(opts)


class Prop extends Node3D:
	func _init() -> void:
		Netw.configure_entity(self)


class Runner extends Node3D:
	var motion := 0

	func _init() -> void:
		var e := Netw.configure_entity(self)
		e.prediction.archetype = NetwPrediction.ARCHETYPE_SCRIPTED
		Netw.configure_property(self, &"position").state()
		Netw.configure_property(self, &"motion").input()

	func _network_tick(_delta: float, _tick: int, _is_fresh: bool) -> void:
		position.x += motion
)";

constexpr int STAYS = 0;
constexpr int GOES = 1;
constexpr int GONE = 6;
constexpr int CLOSED = 14;

World hall() {
    return World(HALL, 2)
        .watch("Level", "api")
        .watch("Level", "exit_reason")
        .watch("Level", "crate_at_exit")
        .hear("Session", "exited")
        .at(1, spawn("Prop", "Crate"))
        .at(1, spawn("Prop", "Ghost"))
        .at(1, spawn("Runner", "Runner"))
        .at(2, control("Runner", STAYS))
        .at(4, on(STAYS, set("Runner", "motion", 1)))
        .at(GONE, on(GOES, leave()))
        .at(CLOSED - 2, call("Level", "kill"))
        .at(CLOSED, leave());
}

NETW_FRAME_SCENARIO(WorldFrames, hall_frames, &hall, CLOSED + 10);

TEST_CASE(
    "[Networked][Compliance][Frame] J5 ending a session frees what it "
    "spawned and nothing the game placed, and frees the session (godot#87184)"
) {
    const Seen &seen = hall_frames.seen();
    const int left = int(netw::NetwSession::LOCAL_LEAVE);
    const int shut = int(netw::NetwSession::SERVER_SHUTDOWN);
    for (const char *spawned : {"Crate", "Ghost", "Runner"}) {
        NETW_HOLDS(absent(seen, GOES, spawned, GONE + 2));
        NETW_HOLDS(reads(seen, STAYS, spawned, "parent", "Level", GONE + 4));
    }
    for (const int peer : {SERVER, STAYS, GOES}) {
        for (const char *spawned : {"Crate", "Ghost", "Runner"}) {
            NETW_HOLDS(absent(seen, peer, spawned, CLOSED + 4));
        }
        NETW_HOLDS(reads(seen, peer, "Post", "parent", "Level"));
        NETW_HOLDS(reads(seen, peer, "Level", "crate_at_exit", true));
        NETW_HOLDS(hears(seen, peer, "Session", "exited", ONCE));
        NETW_HOLDS(freed(seen, peer));
    }
    NETW_HOLDS(reads(seen, SERVER, "Ghost", "parent", "Level", CLOSED - 1));
    NETW_HOLDS(reads(seen, SERVER, "Level", "exit_reason", left));
    NETW_HOLDS(reads(seen, GOES, "Level", "exit_reason", left));
    NETW_HOLDS(reads(seen, STAYS, "Level", "exit_reason", shut));
    NETW_HOLDS(quiet(seen));
}

} // namespace CompliancePhysicsFrame

#endif
