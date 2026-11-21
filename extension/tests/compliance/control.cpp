#include "tests/compliance/laws.h"

#if defined(NETW_TIER_HOSTED)

namespace ComplianceControl {

using namespace godot;
using namespace netw_test;

constexpr const char *PITCH = R"(
class Ball extends Node3D:
	signal claim_judged

	func _init() -> void:
		var entity := Netw.configure_entity(self)
		entity.transfer = NetwEntity.TRANSFER_REQUESTABLE
		entity.control_handler = judge
		Netw.configure_property(self, &"position").broadcast()

	func judge(_request: NetwControlRequest) -> void:
		claim_judged.emit()

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
    "[Netw][Compliance] A1 a client that takes control of an entity "
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


func admit(request: NetwJoinRequest) -> void:
	if request.username != &"":
		request.account = "test:" + request.username
	request.admit()


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
    "[Netw][Compliance][Frame] A2 a player who drops loses control to "
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
    "[Netw][Compliance][Frame] A3 when its controller leaves, an "
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
	signal refused_under(parent: String)

	func _init() -> void:
		var entity := Netw.configure_entity(self)
		entity.lifecycle = NetwEntity.LIFECYCLE_CONTROLLER
		entity.lifecycle_handler = judge
		entity.lifecycle_refused.connect(
			func(request: NetwLifecycleRequest) -> void:
				if request.kind == NetwLifecycleRequest.KIND_REPARENT:
					refused_under.emit(String(get_parent().name))
		)

	func judge(request: NetwLifecycleRequest) -> void:
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
        .hear("Crate", "refused_under")
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
    NETW_HOLDS(
        hears_with(seen, TAKER, "Crate", "refused_under", Array::make("Level"))
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
    "[Netw][Compliance][Frame] A5 a spawn, move or despawn the server "
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
        world.hear(ball, "claim_judged")
            .at(1, spawn("Ball", ball))
            .at(CLAIMED, on(TAKER, claim(ball)));
    }
    return world;
}

LawVerdict claimed_in_order(const Seen &p_seen) {
    godot::PackedStringArray heard;
    for (const Heard &row : p_seen.heard) {
        if (row.peer == SERVER && row.signal == "claim_judged") {
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
    "[Netw][Compliance] A7 the claims a client makes reach the server "
    "in the order it made them, however the link reorders its packets"
) {
    const Seen seen = racked_balls().run(60);
    NETW_HOLDS(claimed_in_order(seen));
    for (const char *ball : RACK) {
        NETW_HOLDS(reads(seen, SERVER, ball, "controller", "client 0"));
    }
    NETW_HOLDS(quiet(seen));
}

constexpr const char *CORRIDOR = R"(
class Walker extends Node3D:
	var push := 0
	var wall := false

	func _init() -> void:
		Netw.configure_entity(self)
		Netw.configure_property(self, &"position").state()
		Netw.configure_property(self, &"push").input()

	func _network_process(_delta: float, _tick: int, _fresh: bool) -> void:
		if push == 1 and not wall:
			position.x += 1.0
)";

World walker_meets_a_wall() {
    return World(CORRIDOR, 1)
        .watch("Walker", "position")
        .at(1, spawn("Walker", "Walker"))
        .at(3, control("Walker", 0))
        .at(10, on(0, set("Walker", "push", 1)))
        .at(20, set("Walker", "wall", true))
        .at(40, on(0, set("Walker", "push", 0)));
}

TEST_CASE(
    "[Netw][Compliance] A8 a player who walked on where the server "
    "stopped them is brought back to where the server holds them"
) {
    const Seen seen = walker_meets_a_wall().run(80);
    NETW_HOLDS(agrees(seen, 0));
    NETW_HOLDS(quiet(seen));
}

constexpr const char *SLOW_JUDGE = R"(
class Ball extends Node3D:
	func _init() -> void:
		var entity := Netw.configure_entity(self)
		entity.transfer = NetwEntity.TRANSFER_REQUESTABLE
		entity.control_handler = judge

	func judge(request: NetwControlRequest) -> void:
		await get_tree().process_frame
		request.deny("too late")
)";

World slowly_judged_ball() {
    return World(SLOW_JUDGE, 1)
        .at(1, spawn("Ball", "Ball"))
        .at(8, on(TAKER, claim("Ball")));
}

TEST_CASE(
    "[Netw][Compliance] A9 a control handler that awaits before it "
    "denies is reported, and the claim is granted as the request stood "
    "when the handler returned"
) {
    const Seen seen = slowly_judged_ball().run(30);
    NETW_HOLDS(reads(seen, SERVER, "Ball", "controller", "client 0"));
    NETW_HOLDS(agrees(seen, TAKER));
    REQUIRE(seen.faults.size() == 1);
    CHECK(seen.faults[0].contains("[NW1043]"));
}

constexpr const char *PUPPET = R"(
class Rig extends AnimationTree:
	func _init() -> void:
		var root := AnimationNodeBlendTree.new()
		root.add_node(&"blend", AnimationNodeBlend2.new())
		tree_root = root
		Netw.configure_entity(self)
		Netw.configure_property(self, &"parameters/blend/blend_amount") \
			.broadcast()
)";

constexpr const char *BLEND = "parameters/blend/blend_amount";

World blended_puppet(double p_client_first, double p_client_second) {
    return World(PUPPET, 1)
        .watch("Rig", BLEND)
        .at(1, spawn("Rig", "Rig"))
        .at(3, set("Rig", BLEND, 0.25))
        .at(8, on(0, set("Rig", BLEND, p_client_first)))
        .at(10, set("Rig", BLEND, 0.5))
        .at(14, on(0, set("Rig", BLEND, p_client_second)))
        .at(16, set("Rig", BLEND, 0.75));
}

TEST_CASE(
    "[Netw][Compliance] A10 a peer that changes a value it does not "
    "control is told once, and one that rewrites the value it was sent is "
    "told nothing"
) {
    const Seen fought = blended_puppet(1.0, 1.0).run(24);
    NETW_HOLDS(reads(fought, 0, "Rig", BLEND, 0.75));
    REQUIRE(fought.faults.size() == 1);
    CHECK(fought.faults[0].contains("'Rig'"));
    CHECK(fought.faults[0].contains("[NW0046]"));
    const Seen followed = blended_puppet(0.25, 0.5).run(24);
    NETW_HOLDS(reads(followed, 0, "Rig", BLEND, 0.75));
    NETW_HOLDS(quiet(followed));
}

constexpr const char *DOCK = R"(
class Crate extends Node3D:
	var held = Array([], TYPE_INT, &"", null)

	func _init() -> void:
		Netw.configure_entity(self)
		Netw.configure_property(self, &"cargo").broadcast()

	func _get_property_list() -> Array[Dictionary]:
		if is_inside_tree() and not multiplayer.is_server():
			return [{"name": "cargo", "type": TYPE_NIL,
				"usage": PROPERTY_USAGE_DEFAULT}]
		return [{"name": "cargo", "type": TYPE_ARRAY,
			"hint": PROPERTY_HINT_ARRAY_TYPE, "hint_string": "int",
			"usage": PROPERTY_USAGE_DEFAULT}]

	func _get(property: StringName) -> Variant:
		return held if property == &"cargo" else null

	func _set(property: StringName, value: Variant) -> bool:
		if property != &"cargo":
			return false
		held = value
		return true
)";

TEST_CASE(
    "[Netw][Compliance] A11 a peer that sends a value of the wrong type, "
    "or an array of the wrong element type, into a typed property is "
    "refused"
) {
    Array words;
    words.set_typed(Variant::STRING, StringName(), Variant());
    words.push_back("tea");
    const Variant cargoes[] = {Dictionary(), words};
    for (const Variant &cargo : cargoes) {
        const Seen seen
            = World(DOCK, 1)
                  .watch("Crate", "cargo")
                  .at(1, spawn("Crate", "Crate"))
                  .at(3, control("Crate", 0))
                  .each(8, 12, on(0, set("Crate", "cargo", cargo)))
                  .run(20);
        NETW_HOLDS(reads(seen, SERVER, "Crate", "cargo", Array()));
        int refused = 0;
        for (const String &fault : seen.faults) {
            refused
                += fault.contains("'cargo'") && fault.contains("[NW1044]");
        }
        CHECK(refused == 1);
        CHECK(seen.faults.size() == refused);
    }
}

} // namespace ComplianceControl

#endif
