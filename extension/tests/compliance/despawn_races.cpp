#include "tests/compliance/laws.h"

#if defined(NETW_TIER_HOSTED)

namespace ComplianceDespawnRaces {

using namespace godot;
using namespace netw_test;

constexpr const char *WAREHOUSE = R"(
class Crate extends Node3D:
	func _init() -> void:
		Netw.configure_entity(self)
		Netw.configure_property(self, &"position").state()

	func nudge(by: Vector3) -> void:
		position += by


signal assets_loaded

var loading := false


func _init() -> void:
	Netw.configure_spawn(unpack_crate)


func unpack_crate() -> Node:
	if loading:
		await assets_loaded
	var crate := Crate.new()
	crate.name = &"Crate"
	return crate


func spawn_crate() -> void:
	Netw.spawn_async(unpack_crate).then(add_child)
)";

constexpr int BUILDING = 0;
constexpr int SHOWING = 1;
constexpr int SMASHED = 8;

World smashed_crate() {
    const Array loaded = Array::make("assets_loaded");
    const Array east = Array::make(Vector3(1, 0, 0));
    return World(WAREHOUSE, 2)
        .hear("Crate", "despawning")
        .at(1, on(BUILDING, set("Level", "loading", true)))
        .at(4, call("Level", "spawn_crate"))
        .each(5, 7, call("Crate", "nudge", east))
        .at(SMASHED, despawn("Crate", "smashed"))
        .at(12, on(BUILDING, call("Level", "emit_signal", loaded)));
}

NETW_FRAME_SCENARIO(WorldFrames, smashed_crate_frames, &smashed_crate, 30);

TEST_CASE(
    "[Networked][Compliance][Frame] D2 an entity despawned while a client is "
    "still building it never shows up there, and a client that already shows "
    "it frees it"
) {
    const Seen &seen = smashed_crate_frames.seen();
    NETW_HOLDS(reads(seen, SHOWING, "Crate", "parent", "Level", SMASHED - 1));
    NETW_HOLDS(hears(seen, SHOWING, "Crate", "despawning", ONCE));
    NETW_HOLDS(
        hears_with(seen, SERVER, "Crate", "despawning", Array::make("smashed"))
    );
    NETW_HOLDS(absent(seen, BUILDING, "Crate", 1));
    for (const int peer : {SERVER, SHOWING}) {
        NETW_HOLDS(absent(seen, peer, "Crate", SMASHED + 2));
    }
    NETW_HOLDS(quiet(seen));
}

constexpr const char *DEPOT = R"(
class Truck extends Node3D:
	func _init() -> void:
		Netw.configure_entity(self)


class Crate extends Node3D:
	func _init() -> void:
		Netw.configure_entity(self)


signal arrived

var loading := false


func _init() -> void:
	Netw.configure_spawn(drive_truck)


func drive_truck() -> Node:
	if loading:
		await arrived
	var truck := Truck.new()
	truck.name = &"Truck"
	return truck


func send_truck() -> void:
	Netw.spawn_async(drive_truck).then(add_child)
)";

constexpr int UNLOADED = 9;

World unloaded_truck() {
    const Array arrived = Array::make("arrived");
    return World(DEPOT, 2)
        .at(1, on(BUILDING, set("Level", "loading", true)))
        .at(4, call("Level", "send_truck"))
        .at(6, spawn("Crate", "Crate", "Truck"))
        .at(UNLOADED, despawn("Crate"))
        .at(14, on(BUILDING, call("Level", "emit_signal", arrived)));
}

NETW_FRAME_SCENARIO(WorldFrames, unloaded_truck_frames, &unloaded_truck, 30);

TEST_CASE(
    "[Networked][Compliance][Frame] D1 a despawn that arrives while its "
    "spawn still waits for its parent cancels it, and the parent lands empty"
) {
    const Seen &seen = unloaded_truck_frames.seen();
    NETW_HOLDS(reads(seen, SHOWING, "Crate", "parent", "Level/Truck", 8));
    NETW_HOLDS(reads(seen, BUILDING, "Truck", "parent", "Level"));
    NETW_HOLDS(absent(seen, BUILDING, "Crate", 1));
    for (const int peer : {SERVER, SHOWING}) {
        NETW_HOLDS(absent(seen, peer, "Crate", UNLOADED + 2));
    }
    NETW_HOLDS(agrees(seen, BUILDING));
    NETW_HOLDS(quiet(seen));
}

constexpr const char *PITCH = R"(
class Ball extends Node3D:
	func _init() -> void:
		Netw.configure_entity(self)
		Netw.configure_property(self, &"position").broadcast()

	func nudge(by: Vector3) -> void:
		position += by
)";

constexpr int DRIVER = 0;
constexpr int POPPED = 30;

World popped_ball() {
    const Array east = Array::make(Vector3(1, 0, 0));
    return World(PITCH, 2)
        .watch("Ball", "position")
        .watch("Spare", "position")
        .at(1, spawn("Ball", "Ball"))
        .at(2, control("Ball", DRIVER))
        .at(20, impair(DRIVER, polls(3, TICK_MS)))
        .each(4, POPPED + 1, on(DRIVER, call("Ball", "nudge", east)))
        .at(POPPED, despawn("Ball"))
        .at(POPPED + 1, spawn("Ball", "Spare"));
}

NETW_FRAME_SCENARIO(WorldFrames, popped_ball_frames, &popped_ball, 50);

TEST_CASE(
    "[Networked][Compliance][Frame] D4 the last values a client sends for an "
    "entity the server freed are dropped quietly and never reach a later "
    "entity (godot#101847)"
) {
    const Seen &seen = popped_ball_frames.seen();
    for (const int peer : {SERVER, 0, 1}) {
        NETW_HOLDS(absent(seen, peer, "Ball", POPPED + 8));
        NETW_HOLDS(reads(seen, peer, "Spare", "position", Vector3()));
    }
    NETW_HOLDS(quiet(seen));
}

constexpr const char *YARD = R"(
class Crate extends Node3D:
	func _init() -> void:
		Netw.configure_entity(self)
		Netw.configure_despawn(self).before_removal(crack).linger(0.2)

	func crack() -> void:
		get_parent().cracks += 1


var cracks := 0
)";

constexpr int CRACKED = 6;

World cracked_crate() {
    return World(YARD, 2)
        .watch("Level", "cracks")
        .hear("Crate", "despawning")
        .at(1, spawn("Crate", "Crate"))
        .at(CRACKED, despawn("Crate", "smashed"));
}

NETW_FRAME_SCENARIO(WorldFrames, cracked_crate_frames, &cracked_crate, 30);

TEST_CASE(
    "[Networked][Compliance][Frame] D5 a despawn is heard once, runs "
    "before_removal on every client, and the client keeps the node for its "
    "linger before freeing it"
) {
    const Seen &seen = cracked_crate_frames.seen();
    NETW_HOLDS(
        hears_with(seen, SERVER, "Crate", "despawning", Array::make("smashed"))
    );
    for (const int client : {0, 1}) {
        NETW_HOLDS(hears(seen, client, "Crate", "despawning", ONCE));
        NETW_HOLDS(reads(seen, client, "Level", "cracks", 1));
        NETW_HOLDS(
            reads(seen, client, "Crate", "parent", "Level", CRACKED + 4)
        );
        NETW_HOLDS(absent(seen, client, "Crate", CRACKED + 16));
    }
    NETW_HOLDS(quiet(seen));
}

} // namespace ComplianceDespawnRaces

#endif
