#include "support/loopback_rig.h"

#if defined(NETW_TIER_HOSTED)

#include "netw/api/entity.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/predict_stats.hpp"
#include "netw/api/prediction.hpp"
#include "support/minted_script.h"

#include <godot_cpp/classes/node2d.hpp>

namespace TestPredictCatchUpLaws {

using namespace godot;
using netw::NetwMultiplayer;
using netw::NetwPredictStats;
using netw_test::LoopbackRig;

constexpr int TICKRATE = 15;
constexpr int WARM_TICKS = 12;
constexpr int HELD_TICKS = 3;
constexpr int SETTLE_TICKS = 24;

const char *WALKER_SOURCE = R"(extends Node2D

var beat := 0
var motion := 0


func _init() -> void:
	var e := Netw.configure_entity(self)
	e.prediction.archetype = NetwPrediction.ARCHETYPE_SCRIPTED
	Netw.configure_property(self, &"beat").state()
	Netw.configure_property(self, &"motion").input()


func _network_tick(_delta: float, _tick: int, _is_fresh: bool) -> void:
	beat += motion
)";

const char *INPUTS_SOURCE = R"(extends Resource

var move := 0
)";

const char *STEERER_SOURCE = R"(extends Node2D

var beat := 0
var assigns := 0
var fresh := {}
var replayed := {}
var inputs: Resource:
	set(value):
		assigns += 1
		inputs = value


func _init() -> void:
	var e := Netw.configure_entity(self)
	e.prediction.archetype = NetwPrediction.ARCHETYPE_SCRIPTED
	Netw.configure_property(self, &"beat").state()
	Netw.configure_property(self, &"inputs:move").input()


func _network_tick(_delta: float, tick: int, is_fresh: bool) -> void:
	if is_fresh:
		fresh[tick] = inputs.move
	else:
		replayed[tick] = inputs.move
	beat += inputs.move
)";

Node *build_walker(const Variant &p_name, const Variant &p_script) {
    const Ref<Script> script = p_script;
    Node *made
        = Object::cast_to<Node>(netw::gd::live_object(script->call("new")));
    made->set_name(String(p_name));
    return made;
}

Node *build_steerer(
    const Variant &p_name,
    const Variant &p_script,
    const Variant &p_inputs
) {
    Node *made = build_walker(p_name, p_script);
    const Ref<Script> inputs = p_inputs;
    made->set(StringName("inputs"), inputs->call("new"));
    return made;
}

struct Walk {
    LoopbackRig rig;
    Ref<Script> script;
    Ref<Script> inputs;
    int route = 0;

    explicit Walk(int p_lobby_ticks = 0, bool p_steers = false) : rig(1) {
        script = netw_test::minted_script(
            p_steers ? STEERER_SOURCE : WALKER_SOURCE
        );
        inputs = netw_test::minted_script(INPUTS_SOURCE);
        const Callable build = p_steers
            ? callable_mp_static(&build_steerer).bind(script, inputs)
            : callable_mp_static(&build_walker).bind(script);
        rig.mount();
        Node *arena = rig.mirror_child("Arena");
        Array types;
        types.push_back(int(Variant::STRING));
        for (int at = -1; at < 1; ++at) {
            rig.register_constructor(
                seat(at),
                StringName("walker"),
                build,
                types
            );
        }
        rig.declare_world(netw_test::WorldDecl().clocked(TICKRATE, 2));
        rig.join(-1, StringName("host"));
        rig.pump(6);
        rig.step_ticks(p_lobby_ticks);
        NetwMultiplayer *server = rig.server();
        Array args;
        args.push_back(String("Walker"));
        const RID entity = server->spawn_registered(
            StringName("walker"),
            args,
            server->peer_get_player(rig.peer_id(0)).ptr()
        );
        Node *node = server->entity_get_node(entity);
        if (node != nullptr && arena != nullptr) {
            arena->add_child(node);
            route = int(server->entity_get_route(entity));
        }
        rig.pump(6);
        NETW_REQUIRE_EQ(int(route > 0), 1);
        NETW_REQUIRE_EQ(int(walker(0) != nullptr), 1);
        NETW_REQUIRE_EQ(int(walker(-1) != nullptr), 1);
    }

    int64_t beat(int p_at) const {
        return int64_t(walker(p_at)->get(StringName("beat")));
    }

    void buffer(int p_ticks) const {
        netw::NetwEntity::of(walker(-1))
            ->get_prediction()
            ->set_consume_buffer_ticks(p_ticks);
    }

    NetwMultiplayer *seat(int p_at) const {
        return p_at < 0 ? rig.server() : rig.client(p_at);
    }

    Node *walker(int p_at) const {
        NetwMultiplayer *api = seat(p_at);
        return api->entity_get_node(api->entity_from_route(route));
    }

    Ref<NetwPredictStats> stats(int p_at) const {
        Node *node = walker(p_at);
        if (node == nullptr) {
            return Ref<NetwPredictStats>();
        }
        return seat(p_at)->predict_get_stats(seat(p_at)->entity_of(node));
    }

    int64_t fact(int p_at, int p_fact) const {
        const Ref<NetwPredictStats> held = stats(p_at);
        return held.is_valid() ? held->get_int_fact(p_fact) : int64_t(-1);
    }

    int64_t lead() const {
        return beat(0) - beat(-1);
    }

    void walk() {
        walker(0)->set(StringName("motion"), 1);
        rig.step_ticks(1);
    }

    void idle() {
        walker(0)->set(StringName("motion"), 0);
        rig.step_ticks(1);
    }

    int ack_age() const {
        return netw::NetwEntity::of(walker(0))
            ->get_prediction()
            ->get_ack_age_ticks();
    }

    void hold_server(bool p_hold) {
        netw::LocalMultiplayerPeer *server_peer
            = rig.session()->get_server_peer().ptr();
        if (p_hold) {
            rig.session()->hold_inbound_packets(server_peer);
        } else {
            rig.session()->release_inbound_packets(server_peer);
        }
    }

    void burst() {
        hold_server(true);
        for (int tick = 0; tick < HELD_TICKS; ++tick) {
            walk();
        }
        hold_server(false);
    }
};

TEST_CASE(
    "[Networked][Predict][SceneTree] PCU1 a server that received a burst of "
    "held input runs the backlog as extra steps and returns to its lead "
    "within the ticks max_consume_per_tick allows, skipping nothing"
) {
    Walk walk;
    const Ref<netw::NetwPrediction> handle
        = netw::NetwEntity::of(walk.walker(-1))->get_prediction();
    NETW_REQUIRE_EQ(int(handle.is_valid()), 1);
    const int per_tick = handle->get_max_consume_per_tick();
    NETW_REQUIRE_EQ(int(per_tick > 1), 1);
    const int bound = (HELD_TICKS + per_tick - 2) / (per_tick - 1);

    for (int tick = 0; tick < WARM_TICKS; ++tick) {
        walk.walk();
    }
    const int64_t lead_before = walk.lead();
    const int64_t corrections_before
        = walk.fact(0, NetwPredictStats::FACT_CORRECTIONS);

    walk.burst();

    for (int tick = 0; tick < SETTLE_TICKS; ++tick) {
        walk.walk();
        if (tick < bound) {
            continue;
        }
        NETW_FORMAT_INT(tick_text, tick);
        CAPTURE(tick_text);
        NETW_CHECK_EQ(walk.lead(), lead_before);
    }
    NETW_CHECK_EQ(
        walk.fact(0, NetwPredictStats::FACT_CORRECTIONS),
        corrections_before
    );
    NETW_CHECK_EQ(walk.fact(-1, NetwPredictStats::FACT_SKIPPED), int64_t(0));
    NETW_CHECK_EQ(walk.fact(-1, NetwPredictStats::FACT_MISSING), int64_t(0));
}

TEST_CASE(
    "[Networked][Predict][SceneTree] PCU2 an idle client keeps getting "
    "acknowledged after the server runs a backlog"
) {
    Walk walk;
    for (int tick = 0; tick < WARM_TICKS; ++tick) {
        walk.walk();
    }
    walk.burst();
    for (int tick = 0; tick < netw::NetwPrediction::ACK_AGE_MAX + 16; ++tick) {
        walk.idle();
    }
    NETW_CHECK_EQ(int(walk.ack_age() < SETTLE_TICKS), 1);
}

TEST_CASE(
    "[Networked][Predict][SceneTree] PCU3 an entity spawned late still moves"
) {
    Walk walk(netw::NetwPrediction::ACK_AGE_MAX + 16);
    walk.buffer(2);
    for (int tick = 0; tick < SETTLE_TICKS; ++tick) {
        walk.walk();
    }
    NETW_CHECK_EQ(int(walk.beat(-1) > 0), 1);
}

TEST_CASE(
    "[Networked][Predict][SceneTree] PCU4 a filled input buffer runs every "
    "tick while an input is late"
) {
    Walk walk;
    walk.buffer(2);
    for (int tick = 0; tick < WARM_TICKS; ++tick) {
        walk.walk();
    }
    walk.hold_server(true);
    const int64_t before = walk.beat(-1);
    walk.walk();
    walk.walk();
    walk.hold_server(false);
    NETW_CHECK_EQ(walk.beat(-1), before + 2);
}

TEST_CASE(
    "[Networked][Predict][SceneTree] PCU5 an input inside a resource reaches "
    "the server's resource on its own tick, and a replay restores it into "
    "the controller's resource"
) {
    Walk walk(0, true);
    Node *client = walk.walker(0);
    Node *server = walk.walker(-1);
    Object *held = netw::gd::live_object(client->get(StringName("inputs")));
    REQUIRE(held != nullptr);
    for (int tick = 0; tick < WARM_TICKS + SETTLE_TICKS; ++tick) {
        if (tick == WARM_TICKS) {
            server->set(StringName("beat"), walk.beat(-1) + 100);
        }
        held->set(StringName("move"), tick % 3 + 1);
        walk.rig.step_ticks(1);
    }

    const Dictionary sent = client->get(StringName("fresh"));
    const Dictionary landed = server->get(StringName("fresh"));
    const Dictionary replayed = client->get(StringName("replayed"));
    REQUIRE(!replayed.is_empty());
    int matched = 0;
    const Array ticks = landed.keys();
    for (int at = 0; at < ticks.size(); ++at) {
        if (sent.has(ticks[at])) {
            NETW_CHECK_EQ(int64_t(landed[ticks[at]]), int64_t(sent[ticks[at]]));
            matched += 1;
        }
    }
    NETW_CHECK_GT(matched, WARM_TICKS);
    const Array again = replayed.keys();
    for (int at = 0; at < again.size(); ++at) {
        if (sent.has(again[at])) {
            NETW_CHECK_EQ(
                int64_t(replayed[again[at]]),
                int64_t(sent[again[at]])
            );
        }
    }
    CHECK(netw::gd::live_object(client->get(StringName("inputs"))) == held);
    NETW_CHECK_EQ(int64_t(client->get(StringName("assigns"))), int64_t(1));
    NETW_CHECK_EQ(int64_t(server->get(StringName("assigns"))), int64_t(1));
}

} // namespace TestPredictCatchUpLaws

#endif
