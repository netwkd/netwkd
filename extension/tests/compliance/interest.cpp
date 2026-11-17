#include "tests/compliance/laws.h"

#if defined(NETW_TIER_HOSTED)

namespace ComplianceInterest {

using namespace godot;
using namespace netw_test;

LawVerdict missing(
    const Seen &p_seen,
    int p_peer,
    const char *p_name,
    int p_from,
    int p_to
) {
    for (int tick = p_from; tick <= p_to; ++tick) {
        if (p_seen.sight(p_peer, p_name, tick) != nullptr) {
            return law_broken(
                "%s holds '%s' at tick %d",
                who(p_peer).c_str(),
                p_name,
                tick
            );
        }
    }
    return law_held();
}

constexpr const char *HALLS = R"(
func open() -> void:
	for level: String in ["Arena", "Lobby"]:
		add_child(Netw.spawn(make_scene, "Hall", level))
	$Arena.add_child(Netw.spawn(make_scene, "Crate", "Prize"))
	$Lobby.add_child(Netw.spawn(make_scene, "Crate", "Bench"))
	$Lobby.add_child(Netw.spawn(make_scene, "Hall", "Den"))
	$Lobby/Den.add_child(Netw.spawn(make_scene, "Crate", "Rug"))


func seat(username: String, called: String, level: String) -> void:
	var pawn := Netw.spawn_player(named(username), make_scene, "Crate", called)
	find_child(level, false, false).add_child(pawn)


func admit(username: String, level: String) -> void:
	Netw.scene(find_child(level, true, false)).watch(named(username))


func named(username: String) -> NetwPlayer:
	for player: NetwPlayer in Netw.session(self).players:
		if player.username == username:
			return player
	return null


class Hall extends Node3D:
	func _init() -> void:
		Netw.configure_scene(self)


class Crate extends Node3D:
	func _init() -> void:
		Netw.configure_entity(self)
)";

constexpr int OUTSIDER = 0;
constexpr int PLAYER = 1;
constexpr int WATCHER = 2;
constexpr int OPEN = 6;
constexpr int MOVE = 10;

World halls() {
    return World(HALLS, 1)
        .at(1, join("ana"))
        .at(2, join("bo"))
        .at(OPEN, call("Level", "open"))
        .at(OPEN, call("Level", "seat", Array::make("ana", "Ana", "Arena")))
        .at(OPEN, call("Level", "admit", Array::make("bo", "Lobby")))
        .at(OPEN, call("Level", "admit", Array::make("bo", "Den")))
        .at(MOVE, reparent("Ana", "Lobby"));
}

TEST_CASE(
    "[Netw][Compliance] I1 a level's content reaches only the players "
    "in it or watching it, and a session ending over nested levels frees them"
) {
    const Seen seen = halls().run(MOVE + 4);
    for (const char *held :
         {"Arena", "Prize", "Lobby", "Bench", "Den", "Rug", "Ana"}) {
        NETW_HOLDS(absent(seen, OUTSIDER, held, 1));
    }
    NETW_HOLDS(reads(seen, PLAYER, "Prize", "parent", "Level/Arena", MOVE - 1));
    NETW_HOLDS(missing(seen, PLAYER, "Bench", 1, MOVE - 1));
    NETW_HOLDS(reads(seen, WATCHER, "Bench", "parent", "Level/Lobby"));
    NETW_HOLDS(reads(seen, WATCHER, "Rug", "parent", "Level/Lobby/Den"));
    NETW_HOLDS(missing(seen, PLAYER, "Rug", 1, MOVE - 1));
    NETW_HOLDS(absent(seen, WATCHER, "Arena", 1));
    NETW_HOLDS(absent(seen, WATCHER, "Prize", 1));
    NETW_HOLDS(missing(seen, WATCHER, "Ana", 1, MOVE - 1));
    NETW_HOLDS(reads(seen, WATCHER, "Ana", "parent", "Level/Lobby"));
    NETW_HOLDS(reads(seen, PLAYER, "Ana", "parent", "Level/Lobby"));
    NETW_HOLDS(absent(seen, PLAYER, "Arena", MOVE + 2));
    NETW_HOLDS(absent(seen, PLAYER, "Prize", MOVE + 2));
    NETW_HOLDS(reads(seen, PLAYER, "Bench", "parent", "Level/Lobby"));
    NETW_HOLDS(quiet(seen));
    const Seen closed
        = World(HALLS, 1).at(OPEN, call("Level", "open")).run(MOVE);
    NETW_HOLDS(quiet(closed));
}

constexpr const char *BUOYS = R"(
func turn() -> void:
	for buoy: String in ["Kept", "Dropped", "Mine"]:
		find_child(buoy, false, false).turns += 1


func show_layer(layer: StringName, shown: bool) -> void:
	var held := Netw.session(self).interest_layer(layer)
	for player: NetwPlayer in Netw.session(self).players:
		if player.peer_id == multiplayer.get_unique_id():
			continue
		if shown:
			held.add_viewer(player.peer_id)
		else:
			held.remove_viewer(player.peer_id)


class Kept extends Node3D:
	var turns := 0

	func _init() -> void:
		Netw.configure_entity(self).interest.join(
			&"kept", NetwInterestLayer.LEAVE_POLICY_RETAIN
		)
		Netw.configure_property(self, &"turns").state()


class Dropped extends Node3D:
	var turns := 0

	func _init() -> void:
		Netw.configure_entity(self).interest.join(
			&"dropped", NetwInterestLayer.LEAVE_POLICY_HIDE
		)
		Netw.configure_property(self, &"turns").state()
)";

constexpr int STEERS = 0;
constexpr int ASIDE = 1;
constexpr int HIDE = 8;
constexpr int SHOW = 16;
constexpr const char *LAYERS[] = {"kept", "dropped"};

Step layer(const char *p_layer, bool p_shown) {
    return call("Level", "show_layer", Array::make(p_layer, p_shown));
}

World buoys() {
    World world(BUOYS, 2);
    for (const char *buoy : {"Kept", "Dropped", "Mine"}) {
        world.watch(buoy, "turns");
        world.hear(buoy, "hidden");
        world.hear(buoy, "despawning");
        world.hear(buoy, "despawned");
    }
    world.at(1, spawn("Kept", "Kept"))
        .at(1, spawn("Dropped", "Dropped"))
        .at(1, spawn("Dropped", "Mine"))
        .at(1, control("Mine", STEERS))
        .each(1, SHOW + 8, call("Level", "turn"));
    for (const char *name : LAYERS) {
        world.at(2, layer(name, true))
            .at(HIDE, layer(name, false))
            .at(SHOW, layer(name, true));
    }
    return world;
}

LawVerdict still(
    const Seen &p_seen,
    int p_peer,
    const char *p_name,
    int p_from,
    int p_to
) {
    const Sight *first = p_seen.sight(p_peer, p_name, p_from);
    if (first == nullptr) {
        return law_broken(
            "%s holds no '%s' at tick %d",
            who(p_peer).c_str(),
            p_name,
            p_from
        );
    }
    const Variant turns = first->facts["turns"];
    for (int tick = p_from + 1; tick <= p_to; ++tick) {
        const LawVerdict held
            = reads(p_seen, p_peer, p_name, "turns", turns, tick);
        if (!held.held) {
            return held;
        }
    }
    return law_held();
}

TEST_CASE(
    "[Netw][Compliance] I2 an entity hidden and shown again is the same "
    "entity with current values, and its updates stop within a tick of the "
    "hide"
) {
    const Seen seen = buoys().run(SHOW + 8);
    for (const int client : {STEERS, ASIDE}) {
        NETW_HOLDS(same_node(seen, client, "Kept"));
        NETW_HOLDS(still(seen, client, "Kept", HIDE + 1, SHOW - 1));
        NETW_HOLDS(missing(seen, client, "Dropped", HIDE + 1, SHOW - 1));
        NETW_HOLDS(hears(seen, client, "Dropped", "hidden", ONCE));
        NETW_HOLDS(hears(seen, client, "Dropped", "despawning", NEVER));
        NETW_HOLDS(hears(seen, client, "Dropped", "despawned", NEVER));
        NETW_HOLDS(hears(seen, client, "Kept", "hidden", NEVER));
        NETW_HOLDS(agrees(seen, client));
    }
    NETW_HOLDS(quiet(seen));
}

TEST_CASE(
    "[Netw][Compliance] I3 a player is never hidden from an entity they "
    "control"
) {
    const Seen seen = buoys().run(SHOW + 8);
    NETW_HOLDS(same_node(seen, STEERS, "Mine"));
    NETW_HOLDS(hears(seen, STEERS, "Mine", "hidden", NEVER));
    NETW_HOLDS(reads(seen, STEERS, "Mine", "turns", SHOW - 1, SHOW - 1));
    NETW_HOLDS(missing(seen, ASIDE, "Mine", HIDE + 1, SHOW - 1));
    NETW_HOLDS(quiet(seen));
}

} // namespace ComplianceInterest

#endif
