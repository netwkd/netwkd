#include "tests/compliance/laws.h"

#if defined(NETW_TIER_HOSTED)

namespace TestBodyReplication {

using namespace godot;
using namespace netw_test;

LawVerdict close(
    const Seen &p_seen,
    int p_peer,
    const char *p_name,
    const char *p_fact,
    const Variant &p_value,
    int p_tick = LAST
) {
    const int tick = p_tick == LAST ? p_seen.ticks : p_tick;
    const Sight *held = p_seen.sight(p_peer, p_name, tick);
    if (held == nullptr) {
        return law_broken(
            "%s holds no '%s' at tick %d",
            who(p_peer).c_str(),
            p_name,
            tick
        );
    }
    const Variant is = held->facts.get(p_fact, Variant());
    const bool flat = is.get_type() == Variant::VECTOR2
        && p_value.get_type() == Variant::VECTOR2;
    const bool solid = is.get_type() == Variant::VECTOR3
        && p_value.get_type() == Variant::VECTOR3;
    const real_t apart = flat ? (Vector2(is) - Vector2(p_value)).length()
        : solid               ? (Vector3(is) - Vector3(p_value)).length()
                              : real_t(1);
    if (apart > 1e-3) {
        return law_broken(
            "%s reads '%s' %s as %s at tick %d, not %s",
            who(p_peer).c_str(),
            p_name,
            p_fact,
            said(is).c_str(),
            tick,
            said(p_value).c_str()
        );
    }
    return law_held();
}

constexpr const char *YARD = R"(
class Cube extends RigidBody3D:
	var tint := 0
	var body_at: Vector3:
		get:
			var state := PhysicsServer3D.BODY_STATE_TRANSFORM
			return PhysicsServer3D.body_get_state(get_rid(), state).origin

	func _init() -> void:
		gravity_scale = 0.0
		collision_layer = 0
		collision_mask = 0
		var shape := CollisionShape3D.new()
		shape.shape = BoxShape3D.new()
		add_child(shape)
		Netw.configure_entity(self)
		Netw.configure_property(self, &"position").state()
		Netw.configure_property(self, &"tint").state()
		Netw.configure_property(self, &"mass").state()


class Crate extends RigidBody3D:
	var body_at: Vector3:
		get:
			var state := PhysicsServer3D.BODY_STATE_TRANSFORM
			return PhysicsServer3D.body_get_state(get_rid(), state).origin

	func _init() -> void:
		freeze_mode = RigidBody3D.FREEZE_MODE_KINEMATIC
		freeze = true
		collision_layer = 0
		collision_mask = 0
		var shape := CollisionShape3D.new()
		shape.shape = BoxShape3D.new()
		add_child(shape)
		Netw.configure_entity(self)
		Netw.configure_property(self, &"position").state()


class Tile extends RigidBody2D:
	var body_at: Vector2:
		get:
			var state := PhysicsServer2D.BODY_STATE_TRANSFORM
			return PhysicsServer2D.body_get_state(get_rid(), state).origin

	func _init() -> void:
		gravity_scale = 0.0
		collision_layer = 0
		collision_mask = 0
		var shape := CollisionShape2D.new()
		shape.shape = RectangleShape2D.new()
		add_child(shape)
		Netw.configure_entity(self)
		Netw.configure_property(self, &"position").state()


class Platform extends Node3D:
	func _init() -> void:
		Netw.configure_entity(self)
		position = Vector3(2, 0, 0)
		rotation = Vector3(0, PI / 2, 0)
)";

constexpr int BOARD = 10;
constexpr int PLACE = 20;
constexpr int ASLEEP = 45;
constexpr int WRITE = ASLEEP + 1;
constexpr int END = WRITE + 14;
const Vector3 PLACED(2, 1, -3);
const Vector3 LOCAL(1, 2, 3);
const Vector2 PLACED_2D(20, -10);

World yard() {
    return World(YARD, 2)
        .watch("Still", "sleeping")
        .watch("Placed", "sleeping")
        .watch("Tile", "sleeping")
        .watch("Still", "tint")
        .watch("Heavy", "mass")
        .watch("Placed", "position")
        .watch("Placed", "body_at")
        .watch("Crate", "position")
        .watch("Crate", "body_at")
        .watch("Tile", "position")
        .watch("Tile", "body_at")
        .watch("Sled", "position")
        .watch("Sled", "body_at")
        .watch("Sled", "global_transform")
        .at(1, spawn("Cube", "Still"))
        .at(1, spawn("Cube", "Heavy"))
        .at(1, spawn("Cube", "Placed"))
        .at(1, spawn("Crate", "Crate"))
        .at(1, spawn("Tile", "Tile"))
        .at(1, spawn("Platform", "Platform"))
        .at(1, spawn("Cube", "Sled"))
        .at(BOARD, reparent("Sled", "Platform"))
        .at(PLACE, set("Sled", "position", LOCAL))
        .at(WRITE, set("Still", "tint", 7))
        .at(WRITE, set("Heavy", "mass", 3.5))
        .at(WRITE, set("Placed", "position", PLACED))
        .at(WRITE, set("Crate", "position", PLACED))
        .at(WRITE, set("Tile", "position", PLACED_2D));
}

NETW_FRAME_SCENARIO(WorldFrames, yard_frames, &yard, END);

TEST_CASE(
    "[Netw][Body][Frame] B1 a game's write to a sleeping body's "
    "position, script property or engine property reaches every client"
) {
    const Seen &seen = yard_frames.seen();
    NETW_HOLDS(reads(seen, SERVER, "Still", "sleeping", true, ASLEEP));
    NETW_HOLDS(reads(seen, SERVER, "Placed", "sleeping", true, ASLEEP));
    NETW_HOLDS(reads(seen, SERVER, "Tile", "sleeping", true, ASLEEP));
    for (const int peer : {0, 1}) {
        NETW_HOLDS(reads(seen, peer, "Still", "tint", 7));
        NETW_HOLDS(reads(seen, peer, "Heavy", "mass", 3.5));
        NETW_HOLDS(close(seen, peer, "Placed", "position", PLACED));
        NETW_HOLDS(close(seen, peer, "Tile", "position", PLACED_2D));
    }
    NETW_HOLDS(quiet(seen));
}

TEST_CASE(
    "[Netw][Body][Frame] B2 a client's copy of a sleeping, frozen or 2D "
    "rigid body stands where the server put it, in its node and in the "
    "physics engine"
) {
    const Seen &seen = yard_frames.seen();
    for (const int peer : {SERVER, 0, 1}) {
        for (const char *held : {"Placed", "Crate"}) {
            NETW_HOLDS(close(seen, peer, held, "position", PLACED));
            NETW_HOLDS(close(seen, peer, held, "body_at", PLACED));
        }
        NETW_HOLDS(close(seen, peer, "Tile", "position", PLACED_2D));
        NETW_HOLDS(close(seen, peer, "Tile", "body_at", PLACED_2D));
    }
    NETW_HOLDS(quiet(seen));
}

TEST_CASE(
    "[Netw][Body][Frame] B3 a body reparented under a turned parent "
    "keeps replicating its pose against that parent"
) {
    const Seen &seen = yard_frames.seen();
    const Sight *server = seen.sight(SERVER, "Sled");
    REQUIRE(server != nullptr);
    const Transform3D pose = server->facts["global_transform"];
    for (const int peer : {SERVER, 0, 1}) {
        NETW_HOLDS(reads(seen, peer, "Sled", "parent", "Level/Platform"));
        NETW_HOLDS(close(seen, peer, "Sled", "position", LOCAL));
        NETW_HOLDS(close(seen, peer, "Sled", "body_at", pose.origin));
    }
    NETW_HOLDS(quiet(seen));
}

} // namespace TestBodyReplication

#endif
