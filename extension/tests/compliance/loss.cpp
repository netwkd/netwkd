#include "tests/compliance/laws.h"

#include "netw/schema_model.hpp"

#if defined(NETW_TIER_HOSTED)

namespace ComplianceLoss {

using namespace godot;
using namespace netw_test;

constexpr const char *HARBOUR = R"(
class Lamp extends Node3D:
	var turns_back := 0
	var turns := 0:
		set(value):
			if value < turns:
				turns_back += 1
			turns = value
	var fuel_back := 0
	var fuel := 0:
		set(value):
			if value < fuel:
				fuel_back += 1
			fuel = value

	func _init() -> void:
		Netw.configure_entity(self)
		Netw.configure_property(self, &"turns").state()
		Netw.configure_property(self, &"fuel").unreliable()

	func turn() -> void:
		turns += 1

	func refuel() -> void:
		fuel += 1
		Netw.sync_property(self, &"fuel")
)";

constexpr int ROUGH = 0;
constexpr int CLEAN = 1;

Ref<netw::NetwLinkConditions> dark() {
    Ref<netw::NetwLinkConditions> storm = link(5);
    storm->set_loss_chance(1.0);
    return storm;
}

World lamp_at_sea() {
    return World(HARBOUR, 2)
        .capture()
        .watch("Lamp", "turns")
        .watch("Lamp", "turns_back")
        .watch("Lamp", "fuel")
        .watch("Lamp", "fuel_back")
        .at(1, spawn("Lamp", "Lamp"))
        .each(2, 60, call("Lamp", "turn"))
        .each(2, 80, call("Lamp", "refuel"))
        .at(4, impair(ROUGH, rough(7, TICK_MS)))
        .at(72, heal(ROUGH));
}

TEST_CASE(
    "[Networked][Compliance] L1 under loss, duplication and reordering a "
    "value never moves backward and ends on the server's final value"
) {
    const Seen seen = lamp_at_sea().run(90);
    NETW_HOLDS(reads(seen, SERVER, "Lamp", "turns", 59));
    NETW_HOLDS(reads(seen, SERVER, "Lamp", "fuel", 79));
    for (const int client : {ROUGH, CLEAN}) {
        NETW_HOLDS(reads(seen, client, "Lamp", "turns_back", 0));
        NETW_HOLDS(reads(seen, client, "Lamp", "fuel_back", 0));
        NETW_HOLDS(agrees(seen, client));
    }
    NETW_HOLDS(quiet(seen));
    NETW_HOLDS(golden(seen, "L1"));
}

constexpr const char *TRACK = R"(
class Runner extends Node3D:
	var pressed := 0
	var last := 0
	var gaps := 0

	func _init() -> void:
		Netw.configure_entity(self)
		Netw.configure_property(self, &"position").state()
		Netw.configure_property(self, &"pressed").input().windowed(WINDOW)

	func press() -> void:
		pressed += 1

	func _network_process(_delta: float, _tick: int, is_fresh: bool) -> void:
		if not multiplayer.is_server() or not is_fresh or pressed == last:
			return
		if last != 0:
			gaps += pressed - last - 1
		last = pressed
)";

std::string windowed_track(int p_window) {
    std::string text(TRACK);
    text.replace(text.find("WINDOW"), 6, std::to_string(p_window));
    return text;
}

World runner_in_a_tunnel(const std::string &p_track, int p_lost) {
    return World(p_track.c_str(), 1)
        .watch("Runner", "pressed")
        .watch("Runner", "gaps")
        .at(1, spawn("Runner", "Runner"))
        .at(3, control("Runner", 0))
        .each(10, 50, on(0, call("Runner", "press")))
        .at(30, impair(0, dark()))
        .at(30 + p_lost, heal(0));
}

TEST_CASE(
    "[Networked][Compliance] L3 lost input packets in a row lose no input "
    "while the window covers them, and one more loses exactly one"
) {
    for (const int window : {1, 3}) {
        const std::string track = windowed_track(window);
        for (const int missed : {0, 1}) {
            const int lost = window + 1 + missed;
            CAPTURE(window);
            CAPTURE(lost);
            const Seen seen = runner_in_a_tunnel(track, lost).run(70);
            NETW_HOLDS(reads(seen, SERVER, "Runner", "pressed", 41));
            NETW_HOLDS(reads(seen, SERVER, "Runner", "gaps", missed));
            NETW_HOLDS(quiet(seen));
        }
    }
}

constexpr const char *YARD = R"(
class Crate extends Node3D:
	func _init() -> void:
		Netw.configure_entity(self)
		Netw.configure_property(self, &"position").state()


func _init() -> void:
	var shed := Node3D.new()
	shed.name = &"Shed"
	add_child(shed)
)";

World yard_in_a_gale() {
    return World(YARD, 2)
        .at(2, impair(ROUGH, rough(3, TICK_MS)))
        .at(4, spawn("Crate", "Ash"))
        .at(5, spawn("Crate", "Birch"))
        .at(6, spawn("Crate", "Cedar"))
        .at(7, spawn("Crate", "Dogwood"))
        .at(9, impair(ROUGH, dark()))
        .at(10, reparent("Birch", "Shed"))
        .at(11, impair(ROUGH, rough(3, TICK_MS)))
        .at(12, despawn("Cedar"))
        .at(14, spawn("Crate", "Elm", "Shed"))
        .at(16, reparent("Ash", "Shed"))
        .at(17, set("Ash", "position", Vector3(2, 0, 0)))
        .at(18, reparent("Ash", "Level"))
        .at(20, despawn("Dogwood"))
        .at(21, reparent("Elm", "Level"))
        .at(40, heal(ROUGH));
}

NETW_FRAME_SCENARIO(WorldFrames, yard_in_a_gale_frames, &yard_in_a_gale, 56);

TEST_CASE(
    "[Networked][Compliance][Frame] L5 spawn, reparent and despawn under "
    "loss and reordering end with the same tree on every peer"
) {
    const Seen &seen = yard_in_a_gale_frames.seen();
    for (int peer = SERVER; peer <= CLEAN; ++peer) {
        NETW_HOLDS(reads(seen, peer, "Ash", "parent", "Level"));
        NETW_HOLDS(reads(seen, peer, "Birch", "parent", "Level/Shed"));
        NETW_HOLDS(reads(seen, peer, "Elm", "parent", "Level"));
        NETW_HOLDS(absent(seen, peer, "Cedar"));
        NETW_HOLDS(absent(seen, peer, "Dogwood"));
    }
    for (const int client : {ROUGH, CLEAN}) {
        NETW_HOLDS(same_node(seen, client, "Ash"));
        NETW_HOLDS(agrees(seen, client));
    }
    NETW_HOLDS(quiet(seen));
}

constexpr const char *STATION = R"(
class Radio extends Node:
	var sent := 0
	var chatter := 0
	var orders := PackedInt32Array()

	func _init() -> void:
		Netw.configure_entity(self)
		Netw.configure_rpc(chat).unreliable()
		Netw.configure_rpc(order).reliable()

	func broadcast() -> void:
		sent += 1
		Netw.rpc(chat, sent)
		Netw.rpc(order, sent)

	func chat(_number: int) -> void:
		chatter += 1

	@rpc("authority", "call_remote", "unreliable")
	func order(number: int) -> void:
		orders.push_back(number)
)";

constexpr int DARK = 0;

PackedInt32Array counted(int p_last) {
    PackedInt32Array numbers;
    for (int number = 1; number <= p_last; ++number) {
        numbers.push_back(number);
    }
    return numbers;
}

World radio_in_a_storm() {
    return World(STATION, 2)
        .watch("Radio", "chatter")
        .watch("Radio", "orders")
        .at(1, spawn("Radio", "Radio"))
        .each(4, 33, call("Radio", "broadcast"))
        .at(10, impair(DARK, dark()))
        .at(20, heal(DARK));
}

TEST_CASE(
    "[Networked][Compliance] L6 an unreliable RPC is lost with its packet, "
    "and a reliable one arrives once and in order"
) {
    const Seen seen = radio_in_a_storm().run(50);
    NETW_HOLDS(reads(seen, DARK, "Radio", "chatter", 19));
    NETW_HOLDS(reads(seen, CLEAN, "Radio", "chatter", 30));
    for (const int client : {DARK, CLEAN}) {
        NETW_HOLDS(reads(seen, client, "Radio", "orders", counted(30)));
    }
    NETW_HOLDS(quiet(seen));
}

constexpr const char *PASTURE = R"(
class Herd extends Node:
	static var schema := Netw.configure_schema(&"L7Herd").reliable()
	static var pos := schema.vector3(&"pos")
	static var hp := schema.i32(&"hp")
	var mobs: NetwTable
	var routes := PackedInt64Array()
	var health := PackedInt32Array()
	var counted := PackedInt32Array()
	var culled := PackedInt64Array()
	var returned := 0

	func _ready() -> void:
		mobs = Netw.session(self).table(schema.schema_name)
		mobs.received.connect(_on_mobs_received)

	func breed() -> void:
		routes = mobs.claim_routes(3)
		health = PackedInt32Array([10, 20, 30])
		commit()

	func hurt() -> void:
		for row in health.size():
			health[row] += 1
		commit()

	func cull() -> void:
		mobs.release_routes(routes.slice(0, 1))
		if mobs.read_routes().has(routes[0]):
			returned += 1
		routes = routes.slice(1)
		health = health.slice(1)

	func commit() -> void:
		var spots := PackedVector3Array()
		spots.resize(routes.size())
		mobs.write_routes(routes)
		mobs.write_column(pos, spots)
		mobs.write_column(hp, health)
		mobs.commit()

	func _on_mobs_received(_tick: int) -> void:
		culled.append_array(mobs.read_deaths())
		for route in mobs.read_routes():
			if culled.has(route):
				returned += 1
		counted = mobs.read_column(hp)
		counted.sort()


func _init() -> void:
	var herd := Herd.new()
	herd.name = &"Herd"
	add_child(herd)
)";

constexpr int LATE = 2;

World herd_in_the_rain() {
    return World(PASTURE, 2)
        .watch("Herd", "counted")
        .watch("Herd", "returned")
        .at(2, call("Herd", "breed"))
        .at(4, impair(ROUGH, rough(9, TICK_MS)))
        .at(26, impair(ROUGH, dark()))
        .each(6, 30, call("Herd", "hurt"))
        .at(12, call("Herd", "cull"))
        .at(34, join("cy"))
        .at(40, heal(ROUGH));
}

TEST_CASE(
    "[Networked][Compliance] L7 a table heals a late peer, keeps a removed "
    "row removed, and survives loss"
) {
    const Seen seen = herd_in_the_rain().run(50);
    netw::schema_model::clear();
    PackedInt32Array last;
    last.push_back(45);
    last.push_back(55);
    NETW_HOLDS(reads(seen, SERVER, "Herd", "counted", last));
    for (const int client : {ROUGH, CLEAN, LATE}) {
        NETW_HOLDS(reads(seen, client, "Herd", "returned", 0));
        NETW_HOLDS(agrees(seen, client));
    }
    NETW_HOLDS(quiet(seen));
}

} // namespace ComplianceLoss

#endif
