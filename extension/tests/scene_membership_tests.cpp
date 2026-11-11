#include "tests/compliance/laws.h"

#if defined(NETW_TIER_HOSTED)

namespace TestSceneMembership {

using namespace godot;
using namespace netw_test;

constexpr const char *WINGS = R"(
var hand := -1
var roll := -1


func named(username: String) -> NetwPlayer:
	for player: NetwPlayer in Netw.session(self).players:
		if player.username == username:
			return player
	return null


func at(called: String) -> Node:
	return find_child(called, true, false)


func open(called: String) -> void:
	add_child(Netw.spawn(make_scene, "Hall", called))


func seat(username: String, called: String, level: String) -> void:
	var pawn := Netw.spawn_player(named(username), make_scene, "Pawn", called)
	pawn.name = called
	at(level).add_child(pawn)


func admit(username: String, level: String) -> void:
	Netw.scene(at(level)).watch(named(username))


func dismiss(username: String, level: String) -> void:
	Netw.scene(at(level)).unwatch(named(username))


func move(called: String, under: String) -> void:
	at(called).reparent(at(under))


func drop(username: String, called: String) -> void:
	var scene := Netw.scene(at(called))
	Netw.despawn(at(called))
	hand = named(username).entities.size()
	roll = scene.player_entities.size()


class Hall extends Node3D:
	var edges := [0, 0, 0, 0, 0]

	var viewers: String:
		get:
			var names := PackedStringArray()
			for player: NetwPlayer in Netw.scene(self).viewers:
				names.append(player.username)
			names.sort()
			return ",".join(names)

	var members: String:
		get:
			var names := PackedStringArray()
			for entity: NetwEntity in Netw.scene(self).entities:
				if entity.root != self:
					names.append(entity.root.name)
			names.sort()
			return ",".join(names)

	var tally: String:
		get:
			return "%d %d %d %d %d" % edges

	func _init() -> void:
		Netw.configure_scene(self)

	func _ready() -> void:
		var scene := Netw.scene(self)
		scene.entity_entered.connect(_on_edge.bind(0))
		scene.entity_exited.connect(_on_edge.bind(1))
		scene.player_entity_entered.connect(_on_edge.bind(2))
		scene.player_entity_left.connect(_on_edge.bind(3))

	func _on_edge(entity: NetwEntity, edge: int) -> void:
		edges[edge] += 1
		if entity.root == null:
			edges[4] += 1


class Pawn extends Node3D:
	pass


class Crate extends Node3D:
	func _init() -> void:
		Netw.configure_entity(self)
)";

constexpr int ANA = 0;
constexpr int BO = 1;
constexpr int CY = 2;
constexpr int DEE = 3;
constexpr int OPEN = 6;
constexpr int LATE = 14;
constexpr int WATCH = 18;
constexpr int CARRY = 20;
constexpr int SECOND = 24;
constexpr int WALK = 26;
constexpr int UNWATCH = 30;
constexpr int DROP = 34;
constexpr int END = 45;

Step level(const char *p_method, const Array &p_args) {
    return call("Level", p_method, p_args);
}

World wings() {
    World world(WINGS, 0);
    world.watch("Level", "hand").watch("Level", "roll");
    for (const char *hall : {"Arena", "Annex", "First", "Second"}) {
        world.watch(hall, "viewers")
            .watch(hall, "members")
            .watch(hall, "tally")
            .at(OPEN, level("open", Array::make(hall)));
    }
    const char *watches[][2] = {
        {"bo", "Arena"},
        {"bo", "Annex"},
        {"cy", "First"},
    };
    for (const auto &row : watches) {
        world.at(OPEN + 1, level("admit", Array::make(row[0], row[1])));
    }
    return world.at(1, join("ana"))
        .at(2, join("bo"))
        .at(3, join("cy"))
        .at(OPEN + 1, level("seat", Array::make("ana", "Ana", "Arena")))
        .at(OPEN + 1, spawn("Crate", "Crate", "Arena"))
        .at(OPEN + 1, spawn("Crate", "Gem", "First"))
        .at(OPEN + 1, spawn("Crate", "Orb", "Second"))
        .at(LATE, join("dee"))
        .at(WATCH, level("admit", Array::make("dee", "Arena")))
        .at(CARRY, reparent("Crate", "Annex"))
        .at(SECOND, level("admit", Array::make("dee", "Annex")))
        .at(WALK, level("move", Array::make("Ana", "Annex")))
        .at(UNWATCH, level("dismiss", Array::make("dee", "Annex")))
        .at(DROP, level("drop", Array::make("ana", "Ana")));
}

NETW_FRAME_SCENARIO(WorldFrames, wings_frames, &wings, END);

TEST_CASE(
    "[Networked][Scene][Frame] MS1 two instances of one level are watched "
    "apart, so a player watching one never holds the other or what is in it"
) {
    const Seen &seen = wings_frames.seen();
    NETW_HOLDS(reads(seen, CY, "Gem", "parent", "Level/First"));
    for (const char *hidden : {"Second", "Orb"}) {
        NETW_HOLDS(absent(seen, CY, hidden, 1));
    }
    for (const char *hidden : {"First", "Second", "Gem", "Orb"}) {
        NETW_HOLDS(absent(seen, BO, hidden, 1));
    }
    NETW_HOLDS(reads(seen, SERVER, "First", "viewers", "cy"));
    NETW_HOLDS(reads(seen, SERVER, "Second", "viewers", ""));
    NETW_HOLDS(quiet(seen));
}

TEST_CASE(
    "[Networked][Scene][Frame] MS2 an entity belongs to the level above it, "
    "and moving it with Netw.reparent or Node.reparent moves it between the "
    "levels' entities on every peer"
) {
    const Seen &seen = wings_frames.seen();
    for (const int peer : {SERVER, BO}) {
        NETW_HOLDS(reads(seen, peer, "Arena", "members", "Ana,Crate", WATCH));
        NETW_HOLDS(reads(seen, peer, "Arena", "members", "Ana", SECOND));
        NETW_HOLDS(reads(seen, peer, "Annex", "members", "Crate", SECOND));
        NETW_HOLDS(reads(seen, peer, "Arena", "members", "", UNWATCH));
        NETW_HOLDS(reads(seen, peer, "Annex", "members", "Ana,Crate", UNWATCH));
    }
    NETW_HOLDS(reads(seen, ANA, "Annex", "members", "Ana,Crate", UNWATCH));
}

TEST_CASE(
    "[Networked][Scene][Frame] MS3 a second watch adds a level to what a "
    "player holds, and an unwatch takes away only that level"
) {
    const Seen &seen = wings_frames.seen();
    const int both = UNWATCH - 1;
    NETW_HOLDS(reads(seen, DEE, "Arena", "viewers", "bo,dee", both));
    NETW_HOLDS(reads(seen, DEE, "Annex", "viewers", "ana,bo,dee", both));
    NETW_HOLDS(absent(seen, DEE, "Annex", UNWATCH + 2));
    NETW_HOLDS(reads(seen, DEE, "Arena", "viewers", "bo,dee"));
    NETW_HOLDS(reads(seen, SERVER, "Annex", "viewers", "ana,bo", DROP));
}

TEST_CASE(
    "[Networked][Scene][Frame] MS4 a player who joins late and watches a "
    "level reads every viewer the level already had"
) {
    const Seen &seen = wings_frames.seen();
    for (const int peer : {SERVER, DEE}) {
        NETW_HOLDS(reads(seen, peer, "Arena", "viewers", "ana,bo,dee", CARRY));
    }
}

TEST_CASE(
    "[Networked][Scene][Frame] MS5 a level reports each entity and each "
    "player entity entering and leaving once while its root stands, on the "
    "server and on a client holding it"
) {
    const Seen &seen = wings_frames.seen();
    for (const int peer : {SERVER, BO}) {
        NETW_HOLDS(reads(seen, peer, "Arena", "tally", "2 2 1 1 0"));
        NETW_HOLDS(reads(seen, peer, "Annex", "tally", "2 1 1 1 0"));
    }
}

TEST_CASE(
    "[Networked][Scene][Frame] MS6 a despawned player entity leaves its "
    "player's entities and its level's player entities in the same call"
) {
    const Seen &seen = wings_frames.seen();
    NETW_HOLDS(reads(seen, SERVER, "Level", "hand", 0));
    NETW_HOLDS(reads(seen, SERVER, "Level", "roll", 0));
}

} // namespace TestSceneMembership

#endif
