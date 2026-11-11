#include "tests/compliance/laws.h"

#if defined(NETW_TIER_HOSTED)

namespace ComplianceControl {

using namespace godot;
using namespace netw_test;

constexpr const char *PITCH = R"(
class Ball extends Node3D:
	func _init() -> void:
		var entity := Netw.configure_entity(self)
		entity.transfer = NetwEntity.TRANSFER_REQUESTABLE
		Netw.configure_property(self, &"position").broadcast()

	func nudge(by: Vector3) -> void:
		position += by
)";

constexpr int TAKER = 0;
constexpr int WATCHER = 1;
constexpr int HELD = 42;
constexpr int LATE = 48;
constexpr int BACK = 60;

World borrowed_ball() {
    const Array east = Array::make(Vector3(1, 0, 0));
    const Array north = Array::make(Vector3(0, 0, 1));
    const Array up = Array::make(Vector3(0, 5, 0));
    return World(PITCH, 2)
        .watch("Ball", "position")
        .hear("Ball", "control_changed")
        .at(1, spawn("Ball", "Ball"))
        .each(14, 20, call("Ball", "nudge", east))
        .at(24, on(TAKER, claim("Ball")))
        .each(26, 40, on(TAKER, call("Ball", "nudge", north)))
        .at(44, on(TAKER, release("Ball")))
        .each(45, 58, on(TAKER, call("Ball", "nudge", up)))
        .each(50, 58, call("Ball", "nudge", east))
        .at(62, on(TAKER, claim("Ball")))
        .each(64, 66, on(TAKER, call("Ball", "nudge", north)))
        .at(68, on(TAKER, leave()))
        .each(70, 72, call("Ball", "nudge", east));
}

TEST_CASE(
    "[Networked][Compliance] A1 a client that takes control of an entity "
    "moves it on every peer, what it writes after handing control back "
    "changes nothing, and control returns to the server when it leaves"
) {
    const Seen seen = borrowed_ball().run(80);
    const Vector3 carried(7, 0, 15);
    const Vector3 returned(16, 0, 15);
    NETW_HOLDS(reads(seen, SERVER, "Ball", "controller", "client 0", HELD));
    NETW_HOLDS(reads(seen, SERVER, "Ball", "position", carried, HELD));
    NETW_HOLDS(reads(seen, SERVER, "Ball", "position", carried, LATE));
    NETW_HOLDS(reads(seen, SERVER, "Ball", "controller", "the server", BACK));
    NETW_HOLDS(reads(seen, SERVER, "Ball", "position", returned, BACK));
    for (const int tick : {HELD, BACK}) {
        NETW_HOLDS(agrees(seen, TAKER, tick));
        NETW_HOLDS(agrees(seen, WATCHER, tick));
    }
    NETW_HOLDS(agrees(seen, WATCHER, LATE));
    NETW_HOLDS(reads(seen, SERVER, "Ball", "position", Vector3(19, 0, 18)));
    NETW_HOLDS(agrees(seen, WATCHER));
    for (const int peer : {SERVER, WATCHER}) {
        NETW_HOLDS(hears(seen, peer, "Ball", "control_changed", 4));
    }
    NETW_HOLDS(quiet(seen));
}

constexpr const char *GARAGE = R"(
class Kart extends Node3D:
	func _init() -> void:
		Netw.configure_entity(self)
		Netw.configure_property(self, &"position").broadcast()

	func nudge(by: Vector3) -> void:
		position += by


class Wreck extends Node3D:
	func _init() -> void:
		var entity := Netw.configure_entity(self)
		entity.on_controller_disconnect = NetwEntity.DISCONNECT_DESPAWN
)";

constexpr int DRIVER = 0;
constexpr int GONE = 20;

World abandoned_karts() {
    const Array east = Array::make(Vector3(1, 0, 0));
    const Array north = Array::make(Vector3(0, 0, 1));
    return World(GARAGE, 2)
        .watch("Kept", "position")
        .hear("Kept", "control_changed")
        .hear("Doomed", "despawning")
        .at(1, spawn("Kart", "Kept"))
        .at(1, spawn("Wreck", "Doomed"))
        .at(2, control("Kept", DRIVER))
        .at(2, control("Doomed", DRIVER))
        .each(12, 18, on(DRIVER, call("Kept", "nudge", east)))
        .at(GONE, on(DRIVER, leave()))
        .each(24, 26, call("Kept", "nudge", north));
}

NETW_FRAME_SCENARIO(WorldFrames, abandoned_karts_frames, &abandoned_karts, 36);

TEST_CASE(
    "[Networked][Compliance][Frame] A3 when its controller leaves, an "
    "entity returns to the server or despawns, as it declared, and does so "
    "once"
) {
    const Seen &seen = abandoned_karts_frames.seen();
    NETW_HOLDS(reads(seen, SERVER, "Kept", "controller", "client 0", GONE - 1));
    NETW_HOLDS(reads(seen, SERVER, "Kept", "position", Vector3(7, 0, 3)));
    NETW_HOLDS(agrees(seen, WATCHER));
    for (const int peer : {SERVER, WATCHER}) {
        NETW_HOLDS(hears(seen, peer, "Kept", "control_changed", 1, GONE));
        NETW_HOLDS(hears(seen, peer, "Doomed", "despawning", 1));
        NETW_HOLDS(absent(seen, peer, "Doomed", GONE + 6));
    }
    NETW_HOLDS(quiet(seen));
}

constexpr const char *DEPOT = R"(
func _ready() -> void:
	var shelf := Node3D.new()
	shelf.name = &"Shelf"
	add_child(shelf)


class Crate extends Node3D:
	var refusal := ""

	func _init() -> void:
		var entity := Netw.configure_entity(self)
		entity.lifecycle = NetwEntity.LIFECYCLE_CONTROLLER
		entity.lifecycle_requested.connect(judge)

	func judge(_peer_id: int, request: NetwLifecycleRequest) -> void:
		request.deny("not yours")

	func shelve() -> void:
		var shelf: Node = get_parent().get_node(^"Shelf")
		Netw.reparent(self, shelf).catch_error(
			func(_code: Error, detail: String) -> void: refusal = detail
		)
)";

constexpr int MOVED = 18;
constexpr int SCRAPPED = 26;

World refused_crates() {
    return World(DEPOT, 2)
        .watch("Crate", "refusal")
        .hear("Crate", "lifecycle_refused")
        .hear("Placed", "lifecycle_refused")
        .at(1, spawn("Crate", "Crate"))
        .at(2, control("Crate", TAKER))
        .at(4, impair(TAKER, polls(3, TICK_MS)))
        .at(14, on(TAKER, spawn("Crate", "Placed")))
        .at(MOVED, on(TAKER, call("Crate", "shelve")))
        .at(SCRAPPED, on(TAKER, despawn("Crate")));
}

NETW_FRAME_SCENARIO(WorldFrames, refused_crates_frames, &refused_crates, 44);

TEST_CASE(
    "[Networked][Compliance][Frame] A5 a spawn, move or despawn the server "
    "refuses shows at once on the client that made it, is undone there, and "
    "never reaches another peer"
) {
    const Seen &seen = refused_crates_frames.seen();
    NETW_HOLDS(reads(seen, TAKER, "Placed", "parent", "Level", 14));
    NETW_HOLDS(absent(seen, TAKER, "Placed", MOVED + 2));
    NETW_HOLDS(hears(seen, TAKER, "Placed", "lifecycle_refused", 1));
    NETW_HOLDS(reads(seen, TAKER, "Crate", "parent", "Level/Shelf", MOVED));
    NETW_HOLDS(reads(seen, TAKER, "Crate", "parent", "Level", SCRAPPED));
    NETW_HOLDS(reads(seen, TAKER, "Crate", "refusal", "not yours", SCRAPPED));
    NETW_HOLDS(hears(seen, TAKER, "Crate", "lifecycle_refused", 1));
    CHECK_MESSAGE(
        seen.sight(TAKER, "Crate", SCRAPPED + 1) == nullptr,
        "client 0 still holds the crate it despawned"
    );
    for (const int peer : {SERVER, WATCHER}) {
        NETW_HOLDS(absent(seen, peer, "Placed", 1));
        NETW_HOLDS(same_node(seen, peer, "Crate"));
        NETW_HOLDS(reads(seen, peer, "Crate", "parent", "Level", MOVED + 2));
        NETW_HOLDS(hears(seen, peer, "Crate", "lifecycle_refused", 0));
    }
    NETW_HOLDS(agrees(seen, TAKER));
    NETW_HOLDS(agrees(seen, WATCHER));
    NETW_HOLDS(quiet(seen));
}

constexpr const char *RACK[] = {"B0", "B1", "B2", "B3", "B4", "B5"};
constexpr int CLAIMED = 14;

World racked_balls() {
    World world(PITCH, 1);
    world.at(4, impair(TAKER, rough(11, TICK_MS)));
    for (const char *ball : RACK) {
        world.hear(ball, "control_requested")
            .at(1, spawn("Ball", ball))
            .at(CLAIMED, on(TAKER, claim(ball)));
    }
    return world;
}

LawVerdict claimed_in_order(const Seen &p_seen) {
    godot::PackedStringArray heard;
    for (const Heard &row : p_seen.heard) {
        if (row.peer == SERVER && row.signal == "control_requested") {
            heard.push_back(row.node);
        }
    }
    const godot::String told = godot::String(", ").join(heard);
    if (told != "B0, B1, B2, B3, B4, B5") {
        return law_broken(
            "the server heard the claims as %s",
            said(told).c_str()
        );
    }
    return law_held();
}

TEST_CASE(
    "[Networked][Compliance] A7 the claims a client makes reach the server "
    "in the order it made them, however the link reorders its packets"
) {
    const Seen seen = racked_balls().run(60);
    NETW_HOLDS(claimed_in_order(seen));
    for (const char *ball : RACK) {
        NETW_HOLDS(reads(seen, SERVER, ball, "controller", "client 0"));
    }
    NETW_HOLDS(quiet(seen));
}

} // namespace ComplianceControl

#endif
