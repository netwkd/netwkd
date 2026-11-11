#include "tests/compliance/laws.h"

#if defined(NETW_TIER_HOSTED)

namespace ComplianceReparenting {

using namespace godot;
using namespace netw_test;

constexpr const char *ROAD = R"(
class Car extends Node3D:
	func _init() -> void:
		Netw.configure_entity(self)
		Netw.configure_property(self, &"position").state()

	func nudge(by: Vector3) -> void:
		position += by


class Rider extends Node3D:
	var odometer := 0

	func _init() -> void:
		Netw.configure_entity(self)
		Netw.configure_property(self, &"position").state()
		Netw.configure_property(self, &"odometer").state()

	func ride() -> void:
		odometer += 1
)";

constexpr int ROUGH = 0;
constexpr int CLEAN = 1;
constexpr int LATE = 2;

World passenger() {
    const Array east = Array::make(Vector3(1, 0, 0));
    const Array north = Array::make(Vector3(0, 0, 1));
    return World(ROAD, 2)
        .watch("Rider", "position")
        .watch("Rider", "odometer")
        .at(1, spawn("Car", "Taxi"))
        .at(1, spawn("Car", "Bus"))
        .at(1, spawn("Rider", "Rider"))
        .each(2, 60, call("Taxi", "nudge", east))
        .each(2, 60, call("Bus", "nudge", north))
        .each(2, 60, call("Rider", "ride"))
        .at(16, impair(ROUGH, rough(7, TICK_MS)))
        .at(24, reparent("Rider", "Taxi"))
        .at(25, reparent("Rider", "Bus"))
        .at(44, heal(ROUGH))
        .at(46, join("late"));
}

TEST_CASE(
    "[Networked][Compliance] R1 a reparented entity is the same node on "
    "every peer and keeps updating, including when the move crosses its own "
    "updates on the link (godot#86501, godot#88729)"
) {
    const Seen seen = passenger().run(70);
    NETW_HOLDS(reads(seen, SERVER, "Rider", "parent", "Level/Bus"));
    NETW_HOLDS(reads(seen, SERVER, "Rider", "odometer", 59));
    for (const int client : {ROUGH, CLEAN}) {
        NETW_HOLDS(same_node(seen, client, "Rider"));
        NETW_HOLDS(agrees(seen, client));
    }
    NETW_HOLDS(agrees(seen, LATE));
    NETW_HOLDS(quiet(seen));
}

constexpr const char *PARK = R"(
var shelf := {}


func hop(called: String) -> void:
	var node := find_child(called, true, false)
	var parent := node.get_parent()
	parent.remove_child(node)
	parent.add_child(node)


func shelve(called: String) -> void:
	var node := find_child(called, true, false)
	shelf[called] = node
	node.get_parent().remove_child(node)


func restore(called: String) -> void:
	add_child(shelf[called])
	shelf.erase(called)


func scrap(called: String) -> void:
	shelf[called].free()
	shelf.erase(called)


func _notification(what: int) -> void:
	if what == NOTIFICATION_PREDELETE:
		for node: Node in shelf.values():
			node.free()


class Pad extends Node3D:
	func _init() -> void:
		Netw.configure_entity(self)


class Runner extends Node3D:
	var motion := 0
	var beat := 0

	func _init() -> void:
		var entity := Netw.configure_entity(self)
		if scripted():
			entity.prediction.archetype = NetwPrediction.ARCHETYPE_SCRIPTED
		Netw.configure_property(self, &"position").state()
		Netw.configure_property(self, &"motion").input()

	func scripted() -> bool:
		return true

	func _network_tick(_delta: float, _tick: int, is_fresh: bool) -> void:
		if is_fresh:
			beat += 1
		position.x += 0.25 * motion


class Declared extends Runner:
	func scripted() -> bool:
		return false

	func _ready() -> void:
		var session := multiplayer as NetwMultiplayer
		if session.predict_declare(session.entity_of(self)) != OK:
			push_error("predict_declare refused %s" % name)
)";

constexpr int DRIVER = 0;
constexpr int HOPS = 12;
constexpr int SHELVED = 20;
constexpr int RESTORED = 32;

struct Parked {
    const char *name;
    const char *scene;
    int peer;
    const char *ending;
};

constexpr Parked PARKED[] = {
    {"Ann", "Runner", SERVER, "restore"},
    {"Bob", "Runner", DRIVER, "restore"},
    {"Dee", "Declared", SERVER, "restore"},
    {"Eve", "Declared", DRIVER, "restore"},
    {"Fay", "Runner", SERVER, "scrap"},
    {"Gus", "Runner", DRIVER, "scrap"},
};

World parking() {
    World world(PARK, 1);
    world.at(1, spawn("Pad", "Pad"))
        .watch("Ada", "beat")
        .at(1, spawn("Runner", "Ada"))
        .at(2, control("Ada", DRIVER))
        .at(4, on(DRIVER, set("Ada", "motion", 1)));
    for (const Parked &parked : PARKED) {
        const Array called = Array::make(String(parked.name));
        const Step ending = call("Level", parked.ending, called);
        world.watch(parked.name, "beat")
            .at(1, spawn(parked.scene, parked.name))
            .at(2, control(parked.name, DRIVER))
            .at(4, on(DRIVER, set(parked.name, "motion", 1)))
            .at(SHELVED, on(parked.peer, call("Level", "shelve", called)))
            .at(RESTORED, on(parked.peer, ending));
    }
    const Array ada = Array::make(String("Ada"));
    return world.at(HOPS, reparent("Ada", "Pad"))
        .at(HOPS + 2, call("Level", "hop", ada))
        .at(HOPS + 4, on(DRIVER, call("Level", "hop", ada)));
}

LawVerdict grows(
    const Seen &p_seen,
    int p_peer,
    const char *p_name,
    const char *p_fact,
    int p_from
) {
    const Sight *first = p_seen.sight(p_peer, p_name, p_from);
    const Sight *last = p_seen.sight(p_peer, p_name);
    if (first == nullptr || last == nullptr) {
        return law_broken(
            "%s does not hold '%s' from tick %d to the end",
            who(p_peer).c_str(),
            p_name,
            p_from
        );
    }
    if (int64_t(last->facts[p_fact]) <= int64_t(first->facts[p_fact])) {
        return law_broken(
            "%s's '%s' %s stayed at %s from tick %d",
            who(p_peer).c_str(),
            p_name,
            p_fact,
            said(first->facts[p_fact]).c_str(),
            p_from
        );
    }
    return law_held();
}

LawVerdict missing(
    const Seen &p_seen,
    int p_peer,
    const char *p_name,
    int p_tick
) {
    if (p_seen.sight(p_peer, p_name, p_tick) != nullptr) {
        return law_broken(
            "%s holds '%s' at tick %d",
            who(p_peer).c_str(),
            p_name,
            p_tick
        );
    }
    return law_held();
}

NETW_FRAME_SCENARIO(WorldFrames, parking_frames, &parking, RESTORED + 16);

TEST_CASE(
    "[Networked][Compliance][Frame] R5 a predicted entity reparented "
    "mid-flight keeps predicting, one taken out of the tree stops without an "
    "error, and one put back predicts again"
) {
    const Seen &seen = parking_frames.seen();
    for (const int peer : {SERVER, DRIVER}) {
        NETW_HOLDS(reads(seen, peer, "Ada", "parent", "Level/Pad"));
        NETW_HOLDS(grows(seen, peer, "Ada", "beat", HOPS + 6));
    }
    NETW_HOLDS(same_node(seen, DRIVER, "Ada"));
    for (const Parked &parked : PARKED) {
        const bool restored = std::string(parked.ending) == "restore";
        if (parked.peer == DRIVER) {
            NETW_HOLDS(reads(seen, SERVER, parked.name, "parent", "Level"));
        } else if (restored) {
            NETW_HOLDS(missing(seen, DRIVER, parked.name, RESTORED - 1));
        } else {
            NETW_HOLDS(absent(seen, DRIVER, parked.name, RESTORED - 1));
        }
        for (const int peer : {SERVER, DRIVER}) {
            if (restored) {
                NETW_HOLDS(
                    grows(seen, peer, parked.name, "beat", RESTORED + 4)
                );
            }
        }
    }
    NETW_HOLDS(quiet(seen));
}

constexpr const char *VENUES = R"(
var here: String:
	get:
		var player := Netw.session(self).local_player
		return player.scene.label if player and player.scene else ""


func open(level: String) -> void:
	add_child(Netw.spawn(make_scene, "Arena", level))
	var root := Netw.scene(find_child(level, false, false)).root
	var players := Netw.session(self).players
	for at in players.size():
		var pawn := Netw.spawn_player(
			players[at], make_scene, "Pawn", "%s%d" % [level, at]
		)
		root.add_child(pawn)


func swap(old: String, level: String, old_first: bool) -> void:
	if old_first:
		Netw.despawn(find_child(old, false, false))
	open(level)
	if not old_first:
		Netw.despawn(find_child(old, false, false))


class Arena extends Node3D:
	func _init() -> void:
		Netw.configure_scene(self)


class Pawn extends Node3D:
	pass
)";

constexpr int OLD_FIRST = 8;
constexpr int NEW_FIRST = 16;
constexpr const char *LEVELS[] = {"Lobby", "Arena", "Annex"};

World venues() {
    return World(VENUES, 2)
        .watch("Level", "here")
        .at(1, call("Level", "open", Array::make(String("Lobby"))))
        .at(OLD_FIRST,
            call("Level", "swap", Array::make("Lobby", "Arena", true)))
        .at(NEW_FIRST,
            call("Level", "swap", Array::make("Arena", "Annex", false)));
}

NETW_FRAME_SCENARIO(WorldFrames, venues_frames, &venues, NEW_FIRST + 10);

LawVerdict alone(const Seen &p_seen, int p_peer) {
    for (int tick = 1; tick <= p_seen.ticks; ++tick) {
        int held = 0;
        for (const char *level : LEVELS) {
            held += p_seen.sight(p_peer, level, tick) != nullptr;
        }
        if (held > 1) {
            return law_broken(
                "%s holds %d levels at tick %d",
                who(p_peer).c_str(),
                held,
                tick
            );
        }
    }
    return law_held();
}

TEST_CASE(
    "[Networked][Compliance][Frame] R6 moving players between levels leaves "
    "each client in exactly one level, in either order"
) {
    const Seen &seen = venues_frames.seen();
    for (const int client : {0, 1}) {
        NETW_HOLDS(alone(seen, client));
        NETW_HOLDS(
            reads(seen, client, "Level", "here", "Arena", NEW_FIRST - 1)
        );
        NETW_HOLDS(reads(seen, client, "Level", "here", "Annex"));
        NETW_HOLDS(absent(seen, client, "Lobby", OLD_FIRST + 2));
        NETW_HOLDS(absent(seen, client, "Arena", NEW_FIRST + 2));
        NETW_HOLDS(reads(seen, client, "Annex1", "parent", "Level/Annex"));
    }
    NETW_HOLDS(quiet(seen));
}

} // namespace ComplianceReparenting

#endif
