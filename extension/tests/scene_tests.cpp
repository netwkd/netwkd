#include "tests/compliance/laws.h"

#if defined(NETW_TIER_HOSTED)

namespace TestScene {

using namespace godot;
using namespace netw_test;

constexpr const char *HALLS = R"(
static var broken := 0
static var bare := 0
var shelf := {}
var changes := ""
var cleared := ""
var carried: NetwPromise
var dropped: NetwPromise

var here: String:
	get:
		var player := Netw.session(self).local_player
		return player.scene.label if player and player.scene else ""

var listed: String:
	get:
		var names := PackedStringArray()
		for scene: NetwScene in Netw.session(self).scenes:
			names.append(scene.label)
		names.sort()
		return ",".join(names)

var carry_code: int:
	get:
		return carried.code if carried and carried.is_settled else -1

var drop_code: int:
	get:
		return dropped.code if dropped and dropped.is_settled else -1

var shelved: String:
	get:
		var told := PackedStringArray()
		for called: String in shelf:
			told.append("%s %d" % [called, shelf[called].gone])
		return ",".join(told)


func _ready() -> void:
	Netw.configure_spawn(make_level)
	(multiplayer as NetwMultiplayer).spawn_register_constructor(
		&"crate", make_crate, [TYPE_STRING]
	)


func _notification(what: int) -> void:
	if what == NOTIFICATION_PREDELETE:
		for node: Node in shelf.values():
			node.free()


func make_level(kind: String, called: String) -> Node:
	if called == "Lobby" and multiplayer.get_unique_id() == broken:
		return null
	return make_scene(kind, called)


func make_crate(called: String) -> Node:
	if multiplayer.get_unique_id() == bare:
		return null
	var crate := Node3D.new()
	crate.name = called
	return crate


func named(username: String) -> NetwPlayer:
	for player: NetwPlayer in Netw.session(self).players:
		if player.username == username:
			return player
	return null


func at(called: String) -> Node:
	return find_child(called, true, false)


func break_for(username: String, crates: String) -> void:
	broken = named(username).peer_id
	bare = named(crates).peer_id


func open(kind: String, called: String) -> void:
	add_child(Netw.spawn(make_level, kind, called))


func crate() -> void:
	var session := multiplayer as NetwMultiplayer
	add_child(session.entity_get_node(
		session.spawn_registered(&"crate", ["Crate"])
	))


func seat(username: String, called: String, level: String) -> void:
	var pawn := Netw.spawn_player(named(username), make_scene, "Pawn", called)
	pawn.name = called
	at(level).add_child(pawn)


func admit(username: String, level: String) -> void:
	Netw.scene(at(level)).watch(named(username))


func listen() -> void:
	Netw.session(self).local_player.scene_changed.connect(_on_scene_changed)


func _on_scene_changed(from: NetwScene, to: NetwScene) -> void:
	changes += "%s>%s;" % [from.label if from else "", to.label if to else ""]


func carry(called: String, level: String) -> void:
	carried = Netw.reparent(at(called), at(level))


func drop(called: String, level: String) -> void:
	var body := at(called)
	dropped = Netw.reparent(body, at(level))
	body.free()


func close(called: String) -> void:
	Netw.despawn(at(called))
	cleared = listed


func move(called: String, under: String) -> void:
	at(called).reparent(at(under))


func shelve(called: String) -> void:
	var node := at(called)
	shelf[called] = node
	node.get_parent().remove_child(node)


func pull(called: String) -> void:
	at(called).reparent(self)


func scrap(called: String, under: String) -> void:
	var node := at(called)
	Netw.despawn(node)
	node.reparent(at(under))


class Hall extends Node3D:
	var gone := 0

	var viewers: String:
		get:
			var names := PackedStringArray()
			var scene := Netw.scene(self)
			if scene == null:
				return "off"
			for player: NetwPlayer in scene.viewers:
				names.append(player.username)
			names.sort()
			return ",".join(names)

	func _init() -> void:
		var config := Netw.configure_scene(self)
		if isolated():
			config.isolated()

	func isolated() -> bool:
		return false

	func _ready() -> void:
		Netw.scene(self).viewer_exited.connect(_on_viewer_exited)

	func _on_viewer_exited(_player: NetwPlayer) -> void:
		gone += 1


class Room extends Hall:
	func isolated() -> bool:
		return true


class Pawn extends Node3D:
	pass


class Toy extends Node3D:
	var scene_label: String:
		get:
			var scene := Netw.scene(self)
			return scene.label if scene else ""

	var same: bool:
		get:
			var scene := Netw.scene(self)
			return scene == Netw.session(self).find_scene(scene.label)

	var seen: bool:
		get:
			return multiplayer.interest_player_sees(
				multiplayer.get_unique_id(), NetwEntity.of(self)
			)

	func _init() -> void:
		Netw.configure_entity(self)


class Shelf extends Toy:
	pass


class Cart extends CharacterBody3D:
	func _init() -> void:
		Netw.configure_entity(self)
		Netw.configure_property(self, &"position").state()
)";

constexpr int ANA = 0;
constexpr int BO = 1;
constexpr int CY = 2;
constexpr int ALONE = 6;
constexpr int OPEN = 10;
constexpr int WALK = 20;
constexpr int CARRY = 25;
constexpr int MOVE = 30;
constexpr int CLOSE = 45;
constexpr int SPLIT = 50;
constexpr int SHELVE = 55;
constexpr int SCRAP = 58;
constexpr int PULL = 60;
constexpr int END = 75;

Step level(const char *p_method, const Array &p_args = Array()) {
    return call("Level", p_method, p_args);
}

World halls() {
    World world(HALLS, 0);
    world.watch("Level", "here")
        .watch("Level", "listed")
        .watch("Level", "changes")
        .watch("Level", "cleared")
        .watch("Level", "carry_code")
        .watch("Level", "drop_code")
        .watch("Level", "shelved")
        .watch("Toy", "scene_label")
        .watch("Toy", "same")
        .watch("Toy", "seen")
        .watch("Shelf", "scene_label")
        .watch("Shelf", "seen");
    for (const char *hall : {"Lobby", "Yard", "Den", "Pit", "Arena", "Annex",
                             "Vault", "Cell"}) {
        world.watch(hall, "viewers").watch(hall, "gone");
    }
    for (const char *view : {"ArenaView", "AnnexView", "VaultView",
                             "CellView"}) {
        world.watch(view, "visible");
    }
    world.at(1, join("ana"))
        .at(2, join("bo"))
        .at(3, join("cy"))
        .at(ALONE + 2, level("break_for", Array::make("cy", "ana")))
        .at(ALONE, level("open", Array::make("Room", "Arena")))
        .at(OPEN, spawn("Shelf", "Shelf"));
    for (const char *hall : {"Lobby", "Yard", "Den", "Pit"}) {
        world.at(OPEN, level("open", Array::make("Hall", hall)));
    }
    for (const char *room : {"Annex", "Vault", "Cell"}) {
        world.at(OPEN, level("open", Array::make("Room", room)));
    }
    const char *watches[][2] = {
        {"ana", "Arena"},
        {"ana", "Annex"},
        {"ana", "Lobby"},
        {"ana", "Vault"},
        {"ana", "Den"},
        {"bo", "Yard"},
        {"bo", "Pit"},
        {"bo", "Cell"},
        {"cy", "Lobby"},
    };
    for (const auto &row : watches) {
        world.at(OPEN + 1, level("admit", Array::make(row[0], row[1])));
    }
    for (const int client : {ANA, BO}) {
        world.at(OPEN, on(client, level("listen")));
    }
    return world
        .at(OPEN + 1, level("seat", Array::make("ana", "Ana", "Arena")))
        .at(OPEN + 1, level("seat", Array::make("bo", "Bo", "Lobby")))
        .at(OPEN + 1, spawn("Toy", "Toy", "Yard"))
        .at(OPEN + 1, spawn("Cart", "Cart", "Yard"))
        .at(OPEN + 1, spawn("Cart", "Wagon", "Yard"))
        .at(OPEN + 1, spawn("Toy", "Bell", "Lobby"))
        .at(OPEN + 1, level("crate"))
        .at(OPEN + 4, level("seat", Array::make("cy", "Cy", "Den")))
        .at(WALK, reparent("Ana", "Annex"))
        .at(CARRY, level("carry", Array::make("Cart", "Lobby")))
        .at(CARRY, level("drop", Array::make("Wagon", "Lobby")))
        .at(MOVE, level("move", Array::make("Lobby", "Shelf")))
        .at(MOVE, reparent("Yard", "Shelf"))
        .at(MOVE, level("move", Array::make("AnnexView", "Shelf")))
        .at(MOVE, reparent("Arena", "Shelf"))
        .at(CLOSE, level("close", Array::make("Yard")))
        .at(CLOSE, level("close", Array::make("VaultView")))
        .at(SPLIT, level("seat", Array::make("bo", "Bo2", "Annex")))
        .at(SHELVE, level("shelve", Array::make("Den")))
        .at(SCRAP, level("scrap", Array::make("Pit", "Shelf")))
        .at(PULL, level("pull", Array::make("Cell")));
}

NETW_FRAME_SCENARIO(WorldFrames, halls_frames, &halls, END);

LawVerdict faulted(const Seen &p_seen, const char *p_text) {
    int found = 0;
    for (const String &line : p_seen.faults) {
        found += line.contains(p_text);
    }
    if (found != 1) {
        return law_broken("'%s' was printed %d times, not once", p_text, found);
    }
    return law_held();
}

LawVerdict ends(const Seen &p_seen, int p_peer, const char *p_changes) {
    const Sight *held = p_seen.sight(p_peer, "Level");
    const String told = held != nullptr ? String(held->facts["changes"]) : "";
    if (!told.ends_with(p_changes)) {
        return law_broken(
            "%s heard its scene change as '%s', which does not end '%s'",
            who(p_peer).c_str(),
            said(told).c_str(),
            p_changes
        );
    }
    return law_held();
}

constexpr int SEATED = OPEN + 4;
constexpr int MOVED = CLOSE - 1;

TEST_CASE(
    "[Networked][Scene][Frame] SC1 despawning a level drops its scene from "
    "the server in the same call and from every client that held it, and "
    "nothing in it ever reaches a client that did not"
) {
    const Seen &seen = halls_frames.seen();
    const char *left = "Annex,Arena,Cell,Den,Lobby,Pit";
    const char *held = "Cell,Lobby,Pit,Yard";
    const char *vault = "Level/VaultView/VaultWorld";
    NETW_HOLDS(reads(seen, SERVER, "Level", "cleared", left));
    NETW_HOLDS(reads(seen, BO, "Level", "listed", held, MOVED));
    NETW_HOLDS(reads(seen, ANA, "Vault", "parent", vault, MOVED));
    for (const char *gone : {"Yard", "Toy", "Vault"}) {
        NETW_HOLDS(absent(seen, SERVER, gone, CLOSE + 1));
        NETW_HOLDS(absent(seen, ANA, gone, CLOSE + 3));
        NETW_HOLDS(absent(seen, BO, gone, CLOSE + 3));
    }
    NETW_HOLDS(absent(seen, ANA, "Toy", 1));
    NETW_HOLDS(absent(seen, BO, "Vault", 1));
    NETW_HOLDS(reads(seen, SERVER, "Shelf", "parent", "Level"));
}

TEST_CASE(
    "[Networked][Scene][Frame] SC2 a client whose spawn function or "
    "registered constructor returns no node is reported once on each side, "
    "is no longer a viewer, and is sent nothing inside, while other clients "
    "keep the level"
) {
    const Seen &seen = halls_frames.seen();
    NETW_HOLDS(absent(seen, CY, "Lobby", 1));
    NETW_HOLDS(absent(seen, CY, "Bell", 1));
    NETW_HOLDS(absent(seen, ANA, "Crate", 1));
    NETW_HOLDS(reads(seen, BO, "Crate", "parent", "Level"));
    NETW_HOLDS(reads(seen, SERVER, "Lobby", "gone", 1, SEATED));
    for (const int peer : {SERVER, ANA, BO}) {
        NETW_HOLDS(reads(seen, peer, "Lobby", "viewers", "ana,bo", SEATED));
    }
    for (const int peer : {ANA, BO}) {
        NETW_HOLDS(reads(seen, peer, "Bell", "parent", "Level/Lobby", SEATED));
    }
    NETW_HOLDS(faulted(seen, "spawn function 'make_level' was dropped"));
    NETW_HOLDS(faulted(seen, "spawn constructor 'crate' was dropped"));
    NETW_HOLDS(faulted(seen, "'cy' was not spawned, because of 'make_level'"));
    NETW_HOLDS(faulted(seen, "'ana' was not spawned, because of 'crate'"));
}

TEST_CASE(
    "[Networked][Scene][Frame] SC3 a level moved with Node.reparent or "
    "Netw.reparent keeps its scene, its viewers and every player's scene on "
    "each peer, and an isolated level moves as its view"
) {
    const Seen &seen = halls_frames.seen();
    NETW_HOLDS(reads(
        seen,
        SERVER,
        "Level",
        "listed",
        "Annex,Arena,Cell,Den,Lobby,Pit,Vault,Yard",
        MOVED
    ));
    NETW_HOLDS(reads(seen, SERVER, "Lobby", "viewers", "ana,bo", MOVED));
    NETW_HOLDS(reads(seen, SERVER, "Yard", "viewers", "bo", MOVED));
    NETW_HOLDS(reads(seen, ANA, "Level", "here", "Annex", MOVED));
    NETW_HOLDS(reads(seen, BO, "Level", "here", "Lobby", MOVED));
    for (const int peer : {SERVER, ANA, BO}) {
        NETW_HOLDS(reads(seen, peer, "Lobby", "parent", "Level/Shelf", MOVED));
    }
    for (const int peer : {SERVER, BO}) {
        NETW_HOLDS(reads(seen, peer, "Yard", "parent", "Level/Shelf", MOVED));
    }
    for (const int peer : {SERVER, ANA}) {
        NETW_HOLDS(reads(
            seen,
            peer,
            "Arena",
            "parent",
            "Level/Shelf/ArenaView/ArenaWorld",
            MOVED
        ));
        NETW_HOLDS(reads(
            seen,
            peer,
            "Annex",
            "parent",
            "Level/Shelf/AnnexView/AnnexWorld",
            MOVED
        ));
    }
    NETW_HOLDS(same_node(seen, ANA, "Annex"));
    NETW_HOLDS(same_node(seen, BO, "Lobby"));
}

TEST_CASE(
    "[Networked][Scene][Frame] SC4 a level removed from the tree closes its "
    "scene and tells its viewer, one despawned and moved in one call raises "
    "no error, and pulling a level out of its view warns once"
) {
    const Seen &seen = halls_frames.seen();
    const char *open = "Annex,Arena,Cell,Lobby,Pit";
    NETW_HOLDS(reads(seen, SERVER, "Level", "listed", open, SHELVE + 1));
    NETW_HOLDS(reads(seen, SERVER, "Den", "viewers", "ana,cy", SHELVE - 1));
    NETW_HOLDS(reads(seen, SERVER, "Level", "shelved", "Den 2"));
    NETW_HOLDS(absent(seen, CY, "Den", SHELVE + 3));
    for (const int peer : {SERVER, BO}) {
        NETW_HOLDS(absent(seen, peer, "Pit", SCRAP + 2));
    }
    NETW_HOLDS(faulted(seen, "'Cell' was moved to outside the SubViewport"));
    NETW_CHECK_EQ(int(seen.faults.size()), 6);
}

TEST_CASE(
    "[Networked][Scene][Frame] SC5 a client shows the view of the level its "
    "player stands in and hides the rest, turning over when the player moves, "
    "and a server with nobody in it shows its one level until a second opens"
) {
    const Seen &seen = halls_frames.seen();
    NETW_HOLDS(reads(seen, SERVER, "ArenaView", "visible", true, ALONE + 1));
    NETW_HOLDS(reads(seen, SERVER, "ArenaView", "visible", false, SEATED));
    NETW_HOLDS(reads(seen, ANA, "ArenaView", "visible", true, WALK - 1));
    NETW_HOLDS(reads(seen, ANA, "AnnexView", "visible", false, WALK - 1));
    NETW_HOLDS(reads(seen, ANA, "VaultView", "visible", false, WALK - 1));
    NETW_HOLDS(reads(seen, ANA, "ArenaView", "visible", false, MOVED));
    NETW_HOLDS(reads(seen, ANA, "AnnexView", "visible", true, MOVED));
    NETW_HOLDS(reads(seen, BO, "CellView", "visible", false, MOVED));
}

TEST_CASE(
    "[Networked][Scene][Frame] SC6 the local player's scene_changed names the "
    "scene left and the scene taken, and a player standing in two levels "
    "presents neither and warns once"
) {
    const Seen &seen = halls_frames.seen();
    NETW_HOLDS(ends(seen, ANA, "Arena>Annex;"));
    NETW_HOLDS(reads(seen, ANA, "Level", "here", "Annex"));
    NETW_HOLDS(ends(seen, BO, "Lobby>;"));
    NETW_HOLDS(reads(seen, BO, "Level", "here", "", SPLIT + 2));
    NETW_HOLDS(faulted(seen, "players in 2 scenes at once, 'Lobby', 'Annex'"));
}

TEST_CASE(
    "[Networked][Scene][Frame] SC7 a body moved into another level with "
    "Netw.reparent lands there on every peer holding both, and its promise "
    "answers OK, or ERR_UNAVAILABLE when the body is freed on the way"
) {
    const Seen &seen = halls_frames.seen();
    NETW_HOLDS(reads(seen, SERVER, "Level", "carry_code", int(OK), MOVED));
    NETW_HOLDS(reads(
        seen,
        SERVER,
        "Level",
        "drop_code",
        int(ERR_UNAVAILABLE),
        MOVED
    ));
    for (const int peer : {SERVER, ANA, BO}) {
        NETW_HOLDS(
            reads(seen, peer, "Cart", "parent", "Level/Shelf/Lobby", MOVED)
        );
    }
}

TEST_CASE(
    "[Networked][Scene][Frame] SC8 Netw.scene of an entity is the scene "
    "find_scene answers for its level, null outside every level, and a client "
    "reads that it sees what it holds"
) {
    const Seen &seen = halls_frames.seen();
    for (const int peer : {SERVER, BO}) {
        NETW_HOLDS(reads(seen, peer, "Toy", "scene_label", "Yard", MOVED));
        NETW_HOLDS(reads(seen, peer, "Toy", "same", true, MOVED));
    }
    NETW_HOLDS(reads(seen, BO, "Toy", "seen", true, MOVED));
    for (const int peer : {SERVER, ANA, BO, CY}) {
        NETW_HOLDS(reads(seen, peer, "Shelf", "scene_label", ""));
    }
    for (const int client : {ANA, BO, CY}) {
        NETW_HOLDS(reads(seen, client, "Shelf", "seen", true));
    }
}

} // namespace TestScene

#endif
