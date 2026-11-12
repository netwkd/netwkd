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
constexpr int FRESH = 4;
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
        .each(2, 8, call("Ball", "nudge", east))
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
    for (const int tick : {FRESH, HELD, BACK}) {
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

constexpr const char *PADDOCK = R"(
func _init() -> void:
	Netw.configure_admission(admit)
	Netw.configure_session(self).return_window(60.0)


func admit(request: NetwJoinRequest) -> Error:
	if request.username != &"":
		request.account = "test:" + request.username
	return OK


func seat(username: String, called: String) -> void:
	for player: NetwPlayer in Netw.session(self).players:
		if player.username == username:
			add_child(Netw.spawn_player(player, make_scene, "Kart", called))


func drop() -> void:
	multiplayer.multiplayer_peer.close()


class Kart extends Node3D:
	var owned_by: String:
		get:
			var player := NetwEntity.of(self).player
			return player.username if player != null else ""

	func _init() -> void:
		Netw.configure_entity(self)
		Netw.configure_property(self, &"position").broadcast()

	func nudge(by: Vector3) -> void:
		position += by
)";

constexpr int STAYED = 0;
constexpr int ANA = 1;
constexpr int STRANGER = 2;
constexpr int RETURNED = 3;
constexpr int DROPPED = 12;
constexpr int STRANGE = 20;
constexpr int BACK_AT = 28;

Step reseed() {
    return [](Live &, int) { netw::gd::seed(4242); };
}

World paddock() {
    const Array east = Array::make(Vector3(1, 0, 0));
    const Array north = Array::make(Vector3(0, 0, 1));
    return World(PADDOCK, 1)
        .watch("Ana", "position")
        .watch("Ana", "owned_by")
        .at(1, reseed())
        .at(1, join("ana"))
        .at(3, call("Level", "seat", Array::make("ana", "Ana")))
        .each(6, 8, on(ANA, call("Ana", "nudge", east)))
        .at(DROPPED, on(ANA, call("Level", "drop")))
        .each(16, 17, call("Ana", "nudge", north))
        .at(STRANGE, reseed())
        .at(STRANGE, join("cy"))
        .at(BACK_AT, join("ana"))
        .each(34, 35, on(RETURNED, call("Ana", "nudge", east)));
}

NETW_FRAME_SCENARIO(WorldFrames, paddock_frames, &paddock, 44);

TEST_CASE(
    "[Networked][Compliance][Frame] A2 a player who drops loses control to "
    "the server, gets the same entities back on return, and a stranger on "
    "the reused peer id gets none"
) {
    const Seen &seen = paddock_frames.seen();
    const int held = DROPPED - 1;
    NETW_HOLDS(reads(seen, SERVER, "Ana", "controller", "client 1", held));
    NETW_HOLDS(reads(seen, SERVER, "Ana", "position", Vector3(3, 0, 0), held));
    for (const int peer : {SERVER, STAYED}) {
        for (const int tick : {STRANGE - 1, BACK_AT - 1}) {
            NETW_HOLDS(
                reads(seen, peer, "Ana", "controller", "the server", tick)
            );
            NETW_HOLDS(reads(seen, peer, "Ana", "owned_by", "", tick));
        }
        NETW_HOLDS(
            reads(seen, peer, "Ana", "position", Vector3(3, 0, 2), BACK_AT - 1)
        );
        NETW_HOLDS(same_node(seen, peer, "Ana"));
    }
    NETW_HOLDS(reads(seen, STRANGER, "Ana", "owned_by", "", BACK_AT - 1));
    NETW_HOLDS(
        reads(seen, STRANGER, "Ana", "position", Vector3(3, 0, 2), BACK_AT - 1)
    );
    NETW_HOLDS(reads(seen, SERVER, "Ana", "position", Vector3(3, 0, 2), 33));
    NETW_HOLDS(reads(seen, SERVER, "Ana", "position", Vector3(5, 0, 2)));
    for (const int peer : {SERVER, STAYED, RETURNED}) {
        NETW_HOLDS(reads(seen, peer, "Ana", "owned_by", "ana"));
        NETW_HOLDS(reads(seen, peer, "Ana", "controller", "client 3"));
    }
    for (const int peer : {STAYED, STRANGER, RETURNED}) {
        NETW_HOLDS(agrees(seen, peer));
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
	signal refused(detail: String)

	func _init() -> void:
		var entity := Netw.configure_entity(self)
		entity.lifecycle = NetwEntity.LIFECYCLE_CONTROLLER
		entity.lifecycle_requested.connect(judge)

	func judge(_peer_id: int, request: NetwLifecycleRequest) -> void:
		request.deny("not yours")

	func shelve() -> void:
		var shelf: Node = get_parent().get_node(^"Shelf")
		Netw.reparent(self, shelf).catch_error(
			func(_code: Error, detail: String) -> void: refused.emit(detail)
		)
)";

constexpr int MOVED = 18;
constexpr int SCRAPPED = 26;

World refused_crates(bool p_slow) {
    World world(DEPOT, 2);
    if (p_slow) {
        world.at(4, impair(TAKER, polls(3, TICK_MS)));
    }
    return world.hear("Crate", "refused")
        .hear("Crate", "lifecycle_refused")
        .hear("Crate", "despawning")
        .hear("Placed", "lifecycle_refused")
        .at(1, spawn("Crate", "Crate"))
        .at(2, control("Crate", TAKER))
        .at(14, on(TAKER, spawn("Crate", "Placed")))
        .at(MOVED, on(TAKER, call("Crate", "shelve")))
        .at(SCRAPPED, on(TAKER, despawn("Crate")));
}

World slow_crates() {
    return refused_crates(true);
}

World near_crates() {
    return refused_crates(false);
}

NETW_FRAME_SCENARIO(WorldFrames, slow_crates_frames, &slow_crates, 44);
NETW_FRAME_SCENARIO(WorldFrames, near_crates_frames, &near_crates, 44);

LawVerdict put_back(const Seen &p_seen, int p_peer, const char *p_name) {
    const Sight *before = p_seen.sight(p_peer, p_name, SCRAPPED - 1);
    const Sight *after = p_seen.sight(p_peer, p_name);
    if (before == nullptr || after == nullptr || before->node != after->node) {
        return law_broken(
            "%s did not put the same '%s' back",
            who(p_peer).c_str(),
            p_name
        );
    }
    return law_held();
}

void refused_on_the_author_alone(const Seen &seen) {
    NETW_HOLDS(reads(seen, TAKER, "Placed", "parent", "Level", 14));
    NETW_HOLDS(absent(seen, TAKER, "Placed", MOVED + 2));
    NETW_HOLDS(hears(seen, TAKER, "Placed", "lifecycle_refused", 1));
    NETW_HOLDS(reads(seen, TAKER, "Crate", "parent", "Level/Shelf", MOVED));
    NETW_HOLDS(reads(seen, TAKER, "Crate", "parent", "Level", SCRAPPED - 1));
    NETW_HOLDS(hears(seen, TAKER, "Crate", "refused", 1, MOVED, SCRAPPED - 1));
    NETW_HOLDS(
        hears_with(seen, TAKER, "Crate", "refused", Array::make("not yours"))
    );
    CHECK_MESSAGE(
        seen.sight(TAKER, "Crate", SCRAPPED) == nullptr,
        "client 0 still holds the crate it despawned"
    );
    for (const int from : {MOVED, SCRAPPED}) {
        const int to = from == MOVED ? SCRAPPED - 1 : LAST;
        NETW_HOLDS(
            hears(seen, TAKER, "Crate", "lifecycle_refused", 1, from, to)
        );
    }
    NETW_HOLDS(put_back(seen, TAKER, "Crate"));
    for (const int peer : {SERVER, WATCHER}) {
        NETW_HOLDS(absent(seen, peer, "Placed", 1));
        NETW_HOLDS(same_node(seen, peer, "Crate"));
        NETW_HOLDS(reads(seen, peer, "Crate", "parent", "Level", MOVED + 2));
        NETW_HOLDS(hears(seen, peer, "Crate", "lifecycle_refused", 0));
    }
    for (const int peer : {SERVER, TAKER, WATCHER}) {
        NETW_HOLDS(hears(seen, peer, "Crate", "despawning", 0));
    }
    NETW_HOLDS(agrees(seen, TAKER));
    NETW_HOLDS(agrees(seen, WATCHER));
    NETW_HOLDS(quiet(seen));
}

TEST_CASE(
    "[Networked][Compliance][Frame] A5 a spawn, move or despawn the server "
    "refuses shows at once on the client that made it, is undone there, and "
    "never reaches another peer"
) {
    {
        CAPTURE("slow link");
        refused_on_the_author_alone(slow_crates_frames.seen());
    }
    {
        CAPTURE("no link");
        refused_on_the_author_alone(near_crates_frames.seen());
    }
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
