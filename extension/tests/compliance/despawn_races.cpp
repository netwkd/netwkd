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
    for (const int peer : {SERVER, 0, 1}) {
        NETW_HOLDS(hears_with(
            seen,
            peer,
            "Crate",
            "despawning",
            Array::make("smashed")
        ));
    }
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

constexpr const char *DOCKS = R"(
class Crate extends Node3D:
	func _init() -> void:
		Netw.configure_entity(self)


func _init() -> void:
	Netw.configure_admission(admit)
	Netw.configure_session(self).return_window(60.0)


func admit(request: NetwJoinRequest) -> void:
	if request.username != &"":
		request.account = "test:" + request.username
	request.admit()


func seat(username: String, called: String) -> void:
	for player: NetwPlayer in Netw.session(self).players:
		if player.username == username:
			add_child(Netw.spawn_player(player, make_scene, "Crate", called))


func cut(username: String, called: String) -> void:
	for player: NetwPlayer in Netw.session(self).players:
		if player.username == username:
			add_child(Netw.spawn_player(player, make_scene, "Crate", called))
			multiplayer.multiplayer_peer.disconnect_peer(player.peer_id)
)";

constexpr int HEIR = 0;
constexpr int STAYED = 1;
constexpr int CUT = 8;
constexpr int MOVED = 16;

World handed_over(Transfer p_mode) {
    return World(DOCKS, 2)
        .migrates(p_mode)
        .at(1, join("ana"))
        .at(1, spawn("Crate", "Keep"))
        .at(1, spawn("Crate", "Doomed"))
        .at(1, spawn("Crate", "Gone"))
        .at(3, call("Level", "seat", Array::make("ana", "Ana")))
        .at(CUT, call("Level", "cut", Array::make("ana", "Late")))
        .at(MOVED, despawn("Doomed"))
        .at(MOVED, spawn("Crate", "Fresh"))
        .at(MOVED, transfer(HEIR))
        .at(MOVED, despawn("Gone"));
}

World handed_over_rejoin() {
    return handed_over(netw::NetwSession::TRANSFER_REJOIN);
}

World handed_over_continue() {
    return handed_over(netw::NetwSession::TRANSFER_CONTINUE);
}

NETW_FRAME_SCENARIO(WorldFrames, rejoin_frames, &handed_over_rejoin, 36);
NETW_FRAME_SCENARIO(WorldFrames, continue_frames, &handed_over_continue, 36);

void raced_onto_no_peer(const Seen &seen) {
    for (const int peer : {SERVER, HEIR, STAYED}) {
        NETW_HOLDS(absent(seen, peer, "Late", CUT));
        for (const char *held : {"Ana", "Doomed", "Gone"}) {
            NETW_HOLDS(reads(seen, peer, held, "parent", "Level", MOVED - 1));
        }
        for (const char *gone : {"Ana", "Doomed", "Gone", "Fresh"}) {
            NETW_HOLDS(absent(seen, peer, gone, MOVED + 6));
        }
        NETW_HOLDS(reads(seen, peer, "Keep", "parent", "Level"));
    }
    NETW_HOLDS(agrees(seen, HEIR));
    NETW_HOLDS(agrees(seen, STAYED));
    NETW_HOLDS(quiet(seen));
}

TEST_CASE(
    "[Networked][Compliance][Frame] D3 a spawn or despawn that races a "
    "player dropping or a server transfer leaves the entity on no peer"
) {
    {
        CAPTURE("rejoin");
        raced_onto_no_peer(rejoin_frames.seen());
    }
    {
        CAPTURE("continue");
        raced_onto_no_peer(continue_frames.seen());
    }
}

constexpr const char *RANGE = R"(
class Target extends Node3D:
	func _init() -> void:
		Netw.configure_entity(self)
		Netw.configure_property(self, &"position").state()


var answer := Vector3(-1, -1, -1)


func _init() -> void:
	Netw.configure_lagcomp(self)


func fire(back: int) -> void:
	var target: Node3D = find_child("Target", true, false)
	var tick: int = Netw.session(self).clock.tick - back
	answer = Netw.rewind([NetwEntity.of(target)], tick, func() -> Vector3:
		return target.position)


func kill() -> void:
	var opts := NetwDespawnOpts.create(&"shot")
	opts.linger = true
	opts.linger_seconds = 0.5
	NetwEntity.of(find_child("Target", true, false)).despawn(opts)
)";

constexpr int KILLED = 10;

World lingering_target() {
    return World(RANGE, 1)
        .watch("Level", "answer")
        .at(1, spawn("Target", "Target"))
        .at(3, set("Target", "position", Vector3(5, 0, 0)))
        .at(KILLED, set("Target", "position", Vector3(9, 0, 0)))
        .at(KILLED, call("Level", "kill"))
        .at(8, call("Level", "fire", Array::make(2)))
        .at(KILLED + 2, call("Level", "fire", Array::make(5)));
}

NETW_FRAME_SCENARIO(
    WorldFrames,
    lingering_target_frames,
    &lingering_target,
    KILLED + 4
);

TEST_CASE(
    "[Networked][Compliance][Frame] P3 a shot rewound to before a lingering "
    "target despawned hits where that target stood"
) {
    const Seen &seen = lingering_target_frames.seen();
    NETW_HOLDS(reads(seen, SERVER, "Level", "answer", Vector3(5, 0, 0)));
    NETW_HOLDS(quiet(seen));
}

} // namespace ComplianceDespawnRaces

#endif
