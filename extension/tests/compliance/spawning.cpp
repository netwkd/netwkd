#include "tests/compliance/laws.h"

#if defined(NETW_TIER_HOSTED)

namespace ComplianceSpawning {

using namespace godot;
using namespace netw_test;

constexpr const char *GARAGE = R"(
class Car extends Node3D:
	func _init() -> void:
		Netw.configure_entity(self)
		Netw.configure_property(self, &"position").state()


class Wheel extends Node3D:
	func _init() -> void:
		Netw.configure_entity(self)
		Netw.configure_property(self, &"rotation").state()


func _init() -> void:
	var garage := Node3D.new()
	garage.name = &"Garage"
	add_child(garage)
)";

constexpr int LATE = 2;

TEST_CASE(
    "[Networked][Compliance] S1 a spawn reaches every peer once, with the "
    "same arguments, under that peer's own parent"
) {
    const Seen seen = World(GARAGE, 2)
                          .capture()
                          .at(1, spawn("Car", "Car", "Level"))
                          .at(1, spawn("Wheel", "Wheel", "Car"))
                          .run(12);
    for (int peer = SERVER; peer < 2; ++peer) {
        NETW_HOLDS(reads(seen, peer, "Car", "parent", "Level"));
        NETW_HOLDS(reads(seen, peer, "Wheel", "parent", "Level/Car"));
        NETW_HOLDS(same_node(seen, peer, "Car"));
        NETW_HOLDS(same_node(seen, peer, "Wheel"));
    }
    NETW_HOLDS(quiet(seen));
    NETW_HOLDS(golden(seen, "S1"));
}

World parked_car() {
    return World(GARAGE, 2)
        .watch("Car", "position")
        .watch("Wheel", "rotation")
        .at(1, spawn("Car", "Car", "Level"))
        .at(1, spawn("Wheel", "Wheel", "Car"))
        .at(4, set("Car", "position", Vector3(4, 0, 2)))
        .at(4, set("Wheel", "rotation", Vector3(0, 1, 0)))
        .at(5, reparent("Wheel", "Garage"))
        .at(6, control("Car", 0))
        .at(8, join("cy"));
}

TEST_CASE(
    "[Networked][Compliance] S3 a late joiner gets every entity with the "
    "value the server holds now, the current controller and the newest parent"
) {
    const Seen seen = parked_car().run(16);
    NETW_HOLDS(reads(seen, LATE, "Car", "position", Vector3(4, 0, 2)));
    NETW_HOLDS(reads(seen, LATE, "Car", "controller", "client 0"));
    NETW_HOLDS(reads(seen, LATE, "Wheel", "rotation", Vector3(0, 1, 0)));
    NETW_HOLDS(reads(seen, LATE, "Wheel", "parent", "Level/Garage"));
    NETW_HOLDS(agrees(seen, LATE));
    NETW_HOLDS(quiet(seen));
}

constexpr const char *WHEELS[] = {"FrontLeft", "RearLeft", "RearRight"};

World broken_car() {
    return World(GARAGE, 2)
        .at(1, spawn("Wheel", "FrontLeft", "Level"))
        .at(2, spawn("Car", "Car", "Level"))
        .at(2, spawn("Wheel", "FrontRight", "Car"))
        .at(2, spawn("Wheel", "RearLeft", "Car"))
        .at(2, spawn("Wheel", "RearRight", "Car"))
        .at(4, reparent("FrontLeft", "Car"))
        .at(6, despawn("FrontRight"))
        .at(10, join("cy"));
}

NETW_FRAME_SCENARIO(WorldFrames, broken_car_frames, &broken_car, 20);

TEST_CASE(
    "[Networked][Compliance][Frame] S4 a late joiner gets parents before "
    "children, after some children were freed (godot#91342)"
) {
    const Seen &seen = broken_car_frames.seen();
    for (const char *wheel : WHEELS) {
        NETW_HOLDS(reads(seen, LATE, wheel, "parent", "Level/Car"));
    }
    for (int peer = SERVER; peer <= LATE; ++peer) {
        NETW_HOLDS(absent(seen, peer, "FrontRight", 8));
    }
    NETW_HOLDS(agrees(seen, LATE));
    NETW_HOLDS(quiet(seen));
}

constexpr const char *DEPOT = R"(
class Crate extends Node3D:
	var tag: String:
		get:
			return String(NetwEntity.of(self).name)

	func _init(named: StringName = &"") -> void:
		var entity := Netw.configure_entity(self)
		if named:
			entity.name = named
		Netw.configure_property(self, &"position").state()


func _init() -> void:
	place(Crate.new(), &"Crate")
	place(Crate.new(&"barrel"), &"Barrel")


func place(crate: Crate, called: StringName) -> void:
	crate.name = called
	add_child(crate)
	crate.owner = self
)";

World depot() {
    return World(DEPOT, 2)
        .watch("Crate", "position")
        .watch("Crate", "tag")
        .watch("Barrel", "position")
        .watch("Barrel", "tag")
        .at(4, set("Crate", "position", Vector3(3, 0, 1)))
        .at(4, set("Barrel", "position", Vector3(0, 2, 0)))
        .at(8, join("cy"));
}

TEST_CASE(
    "[Networked][Compliance] S6 an entity placed in a scene goes online with "
    "no call, and a late joiner's own copy shows the server's state "
    "(godot#80701)"
) {
    const Seen seen = depot().run(16);
    for (int peer = SERVER; peer <= LATE; ++peer) {
        NETW_HOLDS(reads(seen, peer, "Crate", "position", Vector3(3, 0, 1)));
        NETW_HOLDS(reads(seen, peer, "Barrel", "tag", "barrel"));
        NETW_HOLDS(same_node(seen, peer, "Crate"));
        NETW_HOLDS(same_node(seen, peer, "Barrel"));
    }
    for (int client = 0; client <= LATE; ++client) {
        NETW_HOLDS(agrees(seen, client));
    }
    NETW_HOLDS(quiet(seen));
}

} // namespace ComplianceSpawning

#endif
