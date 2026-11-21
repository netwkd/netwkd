#include "support/loopback_rig.h"

#if defined(NETW_TIER_HOSTED)

#include "netw/api/entity.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/prediction.hpp"
#include "netw/predict/drive.hpp"
#include "netw/predict/engine.hpp"
#include "support/fault_log.h"
#include "support/internals.h"
#include "support/minted_script.h"
#include "support/netw_call_log.h"
#include "support/netw_cells.h"

#include <godot_cpp/classes/node2d.hpp>

namespace TestPredictInput {

using namespace godot;
using netw::NetwMultiplayer;
using netw::NetwPrediction;
using netw_test::Internals;
using netw_test::law_broken;
using netw_test::law_held;
using netw_test::LawRowFor;
using netw_test::LawVerdict;
using netw_test::LoopbackRig;

constexpr int TICKRATE = 15;
constexpr int WARM_TICKS = 12;
constexpr int HELD_TICKS = 3;
constexpr int SETTLE_TICKS = 24;
constexpr int SERVER = -1;

TEST_CASE("[Netw][Predict][Input] a predictor receives its configured entity") {
    netw_test::CallLog seen;
    Ref<netw::NetwEntity> entity;
    entity.instantiate();
    entity->get_prediction()->set_predict_input(
        seen.answering("input", Dictionary())
    );
    netw::NetwPredictionEngine pool;
    const int64_t slot = Internals::seat(&pool, entity);
    REQUIRE(slot >= 0);

    REQUIRE(pool.predict_command(slot, 12));

    const Array args = seen.args("input");
    REQUIRE(args.size() == 2);
    const Ref<netw::NetwEntity> subject = args[0];
    CHECK(subject.ptr() == entity.ptr());
    CHECK(int64_t(args[1]) == 12);
}

const char *WALKER_SOURCE = R"(extends Node2D

var beat := 0
var motion := 0


func _init() -> void:
	Netw.configure_entity(self)
	Netw.configure_property(self, &"beat").state()
	Netw.configure_property(self, &"motion").input()


func _network_process(_delta: float, _tick: int, _is_fresh: bool) -> void:
	beat += motion
)";

const char *AIMER_SOURCE = R"(extends Node2D

var beat := 0
var strays := 0
var held = []


func _init() -> void:
	Netw.configure_entity(self)
	Netw.configure_property(self, &"beat").state()
	Netw.configure_property(self, &"aim").input()


func _get_property_list() -> Array[Dictionary]:
	var client := is_inside_tree() and not multiplayer.is_server()
	var kind := TYPE_NIL if client else TYPE_ARRAY
	return [{"name": "aim", "type": kind, "usage": PROPERTY_USAGE_DEFAULT}]


func _get(property: StringName) -> Variant:
	return held if property == &"aim" else null


func _set(property: StringName, value: Variant) -> bool:
	if property != &"aim":
		return false
	held = value
	return true


func _network_process(_delta: float, _tick: int, _is_fresh: bool) -> void:
	if typeof(held) != TYPE_ARRAY:
		strays += 1
	beat += 1
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
	Netw.configure_entity(self)
	Netw.configure_property(self, &"beat").state()
	Netw.configure_property(self, &"inputs:move").input()


func _network_process(_delta: float, tick: int, is_fresh: bool) -> void:
	if is_fresh:
		fresh[tick] = inputs.move
	else:
		replayed[tick] = inputs.move
	beat += inputs.move
)";

struct World {
    String label;
};

typedef LawRowFor<World> Row;

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

    Walk(
        int p_lobby_ticks,
        bool p_steers,
        bool p_client_controls,
        const char *p_source = WALKER_SOURCE
    ) :
            rig(1) {
        script = netw_test::minted_script(
            p_steers ? STEERER_SOURCE : p_source
        );
        inputs = netw_test::minted_script(INPUTS_SOURCE);
        const Callable build = p_steers
            ? callable_mp_static(&build_steerer).bind(script, inputs)
            : callable_mp_static(&build_walker).bind(script);
        rig.mount();
        Node *arena = rig.mirror_child("Arena");
        Array types;
        types.push_back(int(Variant::STRING));
        for (int at = SERVER; at < 1; ++at) {
            rig.register_constructor(
                seat(at),
                StringName("walker"),
                build,
                types
            );
        }
        rig.declare_world(netw_test::WorldDecl().clocked(TICKRATE, 2));
        rig.join(SERVER, StringName("host"));
        rig.pump(6);
        rig.step_ticks(p_lobby_ticks);
        NetwMultiplayer *server = rig.server();
        Array args;
        args.push_back(String("Walker"));
        const RID entity = server->spawn_registered(
            StringName("walker"),
            args,
            p_client_controls ? rig.player(0)->get_id() : 0
        );
        Node *node = server->entity_get_node(entity);
        if (node != nullptr && arena != nullptr) {
            arena->add_child(node);
            route = int(server->entity_get_route(entity));
        }
        rig.pump(6);
        NETW_REQUIRE_EQ(int(route > 0), 1);
        NETW_REQUIRE_EQ(int(walker(0) != nullptr), 1);
        NETW_REQUIRE_EQ(int(walker(SERVER) != nullptr), 1);
    }

    NetwMultiplayer *seat(int p_at) const {
        return p_at < 0 ? rig.server() : rig.client(p_at);
    }

    Node *walker(int p_at) const {
        NetwMultiplayer *api = seat(p_at);
        return api->entity_get_node(Internals::entity_at(api, route));
    }

    Ref<NetwPrediction> prediction(int p_at) const {
        return netw::NetwEntity::of(walker(p_at))->get_prediction();
    }

    int64_t beat(int p_at) const {
        return int64_t(walker(p_at)->get(StringName("beat")));
    }

    int64_t corrections(int p_at) const {
        return prediction(p_at)->get_correction_count();
    }

    void step(int p_at, int p_motion) {
        walker(p_at)->set(StringName("motion"), p_motion);
        rig.step_ticks(1);
    }
};

LawVerdict burst_is_caught_up(const World &) {
    Walk walk(0, false, true);
    const int per_tick = walk.prediction(SERVER)->get_max_inputs_per_tick();
    if (per_tick <= 1) {
        return law_broken("the server consumes %d input a tick", per_tick);
    }
    const int bound = (HELD_TICKS + per_tick - 2) / (per_tick - 1);
    for (int tick = 0; tick < WARM_TICKS; ++tick) {
        walk.step(0, 1);
    }
    const int64_t lead = walk.beat(0) - walk.beat(SERVER);
    const int64_t corrected = walk.corrections(0);

    walk.rig.hold(SERVER);
    for (int tick = 0; tick < HELD_TICKS; ++tick) {
        walk.step(0, 1);
    }
    walk.rig.release(SERVER);

    for (int tick = 0; tick < SETTLE_TICKS; ++tick) {
        walk.step(0, 1);
        const int64_t now = walk.beat(0) - walk.beat(SERVER);
        if (tick >= bound && now != lead) {
            return law_broken(
                "%d ticks after the burst the client leads by %d, not %d",
                tick,
                int(now),
                int(lead)
            );
        }
    }
    if (walk.corrections(0) != corrected) {
        return law_broken(
            "the burst corrected the client %d time(s)",
            int(walk.corrections(0) - corrected)
        );
    }
    return law_held();
}

LawVerdict late_spawn_moves(const World &) {
    Walk walk(netw::predict::ACK_AGE_MAX + 16, false, true);
    walk.prediction(SERVER)->set_input_buffer_ticks(2);
    for (int tick = 0; tick < SETTLE_TICKS; ++tick) {
        walk.step(0, 1);
    }
    return walk.beat(SERVER) > 0
        ? law_held()
        : law_broken("the server never stepped the late entity");
}

LawVerdict resource_input_lands(const World &) {
    Walk walk(0, true, true);
    Node *client = walk.walker(0);
    Node *server = walk.walker(SERVER);
    Object *held = netw::gd::live_object(client->get(StringName("inputs")));
    if (held == nullptr) {
        return law_broken("the client's walker holds no input resource");
    }
    for (int tick = 0; tick < WARM_TICKS + SETTLE_TICKS; ++tick) {
        if (tick == WARM_TICKS) {
            server->set(StringName("beat"), walk.beat(SERVER) + 100);
        }
        held->set(StringName("move"), tick % 3 + 1);
        walk.rig.step_ticks(1);
    }

    const Dictionary sent = client->get(StringName("fresh"));
    const Dictionary landed = server->get(StringName("fresh"));
    const Dictionary replayed = client->get(StringName("replayed"));
    if (replayed.is_empty()) {
        return law_broken("the server's write never made the client replay");
    }
    int matched = 0;
    const Array ticks = landed.keys();
    for (int at = 0; at < ticks.size(); ++at) {
        if (!sent.has(ticks[at])) {
            continue;
        }
        if (int64_t(landed[ticks[at]]) != int64_t(sent[ticks[at]])) {
            return law_broken(
                "the server ran tick %d on move %d, the client sent %d",
                int(ticks[at]),
                int(landed[ticks[at]]),
                int(sent[ticks[at]])
            );
        }
        matched += 1;
    }
    if (matched <= WARM_TICKS) {
        return law_broken("only %d tick(s) reached the server", matched);
    }
    const Array again = replayed.keys();
    for (int at = 0; at < again.size(); ++at) {
        if (sent.has(again[at])
            && int64_t(replayed[again[at]]) != int64_t(sent[again[at]])) {
            return law_broken(
                "the replay of tick %d ran move %d, the client sent %d",
                int(again[at]),
                int(replayed[again[at]]),
                int(sent[again[at]])
            );
        }
    }
    if (netw::gd::live_object(client->get(StringName("inputs"))) != held) {
        return law_broken("the replay replaced the client's resource");
    }
    if (int64_t(client->get(StringName("assigns"))) != 1
        || int64_t(server->get(StringName("assigns"))) != 1) {
        return law_broken("the input resource was assigned again");
    }
    return law_held();
}

LawVerdict mistyped_input_is_refused(const World &) {
    netw_test::FaultWatch faults;
    faults.expect_warning("*'aim'*[NW1044]*", 1);
    Walk walk(0, false, true, AIMER_SOURCE);
    for (int tick = 0; tick < WARM_TICKS; ++tick) {
        walk.walker(0)->set(StringName("aim"), Array());
        walk.rig.step_ticks(1);
    }
    if (walk.beat(SERVER) <= 0) {
        return law_broken("the server never stepped on a typed input");
    }
    for (int tick = 0; tick < SETTLE_TICKS; ++tick) {
        walk.walker(0)->set(StringName("aim"), Dictionary());
        walk.rig.step_ticks(1);
    }
    const int64_t strays = walk.walker(SERVER)->get(StringName("strays"));
    if (strays != 0) {
        return law_broken(
            "the server ran %d tick(s) on a Dictionary",
            int(strays)
        );
    }
    const int64_t told = faults.warnings_containing("[NW1044]");
    return told == 1
        ? law_held()
        : law_broken("the server reported the refusal %d times", int(told));
}

LawVerdict server_body_moves(const World &) {
    Walk walk(0, false, false);
    for (int tick = 0; tick < SETTLE_TICKS; ++tick) {
        walk.step(SERVER, 1);
    }
    if (walk.beat(SERVER) <= 0) {
        return law_broken("the server never stepped its own body");
    }
    if (walk.beat(0) <= 0) {
        return law_broken("the client never saw the server's body move");
    }
    return law_held();
}

const Row ROWS[] = {
    {"a burst of input the server held arrives at once",
     "the server runs it as extra steps, is back at its lead within what "
     "max_inputs_per_tick allows, and the client is never corrected",
     &burst_is_caught_up},
    {"an entity spawned after the session ran past the acknowledgement "
     "horizon",
     "the server still steps it",
     &late_spawn_moves},
    {"an input property inside a resource",
     "the server runs each tick on the input the client sent for it, a "
     "replay restores it into the same resource, and the resource is never "
     "assigned again",
     &resource_input_lands},
    {"a client sends an input property a value of the wrong type",
     "the server refuses it, keeps its own typed value and reports it once",
     &mistyped_input_is_refused},
    {"a predicted body nobody controls, driven by the server",
     "it moves on the server and on the client",
     &server_body_moves},
};

TEST_CASE(
    "[Netw][Predict][SceneTree] a player's input reaches the server on "
    "its own tick, late or in a burst, and a body the server controls moves"
) {
    const World world = {String("a server and one client on a loopback")};
    for (const Row &row : ROWS) {
        NETW_CELL(row, world);
        NETW_LAW_HOLDS(row, world);
    }
}

} // namespace TestPredictInput

#endif
