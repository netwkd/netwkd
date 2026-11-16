#include "support/link.h"
#include "support/netw_test.h"

#if defined(NETW_TIER_HOSTED)

#include "support/fault_log.h"
#include "support/loopback_rig.h"
#include "support/minted_script.h"
#include "support/netw_call_log.h"
#include "support/netw_cells.h"

#include "godot/node.hpp"
#include "godot/variant.hpp"
#include "netw/api/action_config.hpp"
#include "netw/api/action_request.hpp"
#include "netw/api/context.hpp"
#include "netw/api/lag_compensation_config.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/session/frames.hpp"
#include "netw/wire/registry.hpp"

namespace TestAction {

using namespace godot;
using netw::Netw;
using netw::NetwActionConfig;
using netw_test::CallLog;
using netw_test::Carrier;
using netw_test::EntityDecl;
using netw_test::FaultWatch;
using netw_test::law_broken;
using netw_test::law_held;
using netw_test::LawRowFor;
using netw_test::LawVerdict;
using netw_test::LoopbackRig;
using netw_test::WorldDecl;

constexpr int ANSWER_TICKS = 30;

constexpr const char *GUN = R"(extends Node2D

var ghosts := 0
var confirmed := 0

func _init() -> void:
	Netw.configure_entity(self)
	Netw.configure_spawn(make_bolt)
	Netw.configure_action(fire).predict(draw_ghost).confirm(keep)

func make_bolt() -> Node:
	var bolt := Node2D.new()
	bolt.name = &"Bolt"
	return bolt

func shoot(tick: int) -> void:
	Netw.action(fire, tick)

func draw_ghost() -> Node:
	var ghost := Node2D.new()
	ghost.name = &"Ghost"
	add_child(ghost)
	ghosts += 1
	return ghost

func keep(ghost: Node) -> void:
	confirmed += 1
	ghost.queue_free()

func fire(ctx: NetwActionRequest) -> void:
	var bolt: Node = Netw.spawn(make_bolt)
	ctx.bind(bolt)
	add_child(bolt)

func bolt() -> Node2D:
	for child in get_children():
		if child.name != &"Ghost":
			return child as Node2D
	return null
)";

Script *gun_script = nullptr;

Node *build_gun(const Variant &p_name) {
    Node *made = Object::cast_to<Node>(gun_script->call("new"));
    made->set_name(String(p_name));
    return made;
}

struct World {
    String label;
};

typedef LawRowFor<World> Row;

typedef void (*Declare)(LoopbackRig &);

void declare_nothing(LoopbackRig &) {
}

Node *child_of(Node *p_parent, const char *p_name) {
    Node *made = memnew(Node);
    made->set_name(StringName(p_name));
    p_parent->add_child(made);
    return made;
}

struct Shot {
    LoopbackRig rig;
    Carrier *owner = nullptr;
    Carrier *authority = nullptr;
    Node *orphan = nullptr;

    explicit Shot(
        Declare p_declare = &declare_nothing,
        bool p_predicted = true,
        const char *p_authority = "_server_action",
        bool p_server_armed = true,
        int p_send_rate = 0
    )
        : rig(1) {
        rig.mount();
        p_declare(rig);
        EntityDecl player = EntityDecl()
                                .named("P")
                                .on_schema("ShooterPose")
                                .synced("position")
                                .placed_at(Vector2())
                                .mounted();
        if (p_predicted) {
            player.predicted(0.01);
        }
        WorldDecl world;
        world.clocked(30, 3).sending(p_send_rate).player(player, 0);
        rig.declare_world(world);
        owner = Object::cast_to<Carrier>(
            rig.node_of(rig.entity_of(StringName("P"), 0), 0)
        );
        authority = Object::cast_to<Carrier>(rig.node_of(rig.entity_of("P")));
        REQUIRE(owner != nullptr);
        REQUIRE(authority != nullptr);
        owner->arm_action(rig.client(0), StringName(p_authority));
        if (p_server_armed) {
            authority->arm_action(rig.server(), StringName(p_authority));
        }
    }

    ~Shot() {
        if (orphan != nullptr) {
            memdelete(orphan);
        }
    }

    void declare_timing(NetwActionConfig::Timing p_mode) {
        authority->declare_timing(p_mode);
        owner->declare_timing(p_mode);
    }

    int64_t server_tick() const {
        return rig.server()->clock_get_tick();
    }

    int64_t client_tick() const {
        return rig.client(0)->clock_get_tick();
    }

    int64_t fallbacks() const {
        const Dictionary metrics = rig.server()->lagcomp_metrics();
        return int64_t(metrics.get(StringName("gate_fallbacks"), -1));
    }

    void step_until_run(int p_budget) {
        for (int at = 0; at < p_budget && authority->requests() == 0; ++at) {
            rig.step_ticks(1);
        }
    }
};

LawVerdict denied_reverts(const World &) {
    Shot shot;
    shot.owner->fire(shot.client_tick());
    if (shot.owner->ghosts() != 1 || shot.owner->ghost() == nullptr) {
        return law_broken("predict made %d ghost(s)", shot.owner->ghosts());
    }
    shot.rig.step_ticks(ANSWER_TICKS);
    if (shot.authority->requests() != 1 || shot.owner->denials() != 1) {
        return law_broken(
            "%d run(s) and %d denial(s)",
            shot.authority->requests(),
            shot.owner->denials()
        );
    }
    if (!shot.owner->ghost()->is_queued_for_deletion()) {
        return law_broken("the denied ghost was kept");
    }
    if (shot.owner->denial_code() != int(ERR_BUSY)
        || shot.owner->denial_reason() != String("no ammunition")) {
        return law_broken(
            "revert got code %d and reason '%s'",
            shot.owner->denial_code(),
            shot.owner->denial_reason().utf8().get_data()
        );
    }
    return law_held();
}

LawVerdict accepted_confirms(const World &) {
    Shot shot;
    shot.authority->set_denies(false);
    shot.owner->fire(shot.client_tick());
    shot.rig.step_ticks(ANSWER_TICKS);
    if (shot.owner->confirmations() != 1 || shot.owner->denials() != 0) {
        return law_broken(
            "%d confirmation(s) and %d denial(s)",
            shot.owner->confirmations(),
            shot.owner->denials()
        );
    }
    if (shot.owner->ghost() == nullptr
        || !shot.owner->ghost()->is_queued_for_deletion()) {
        return law_broken("the confirmed ghost was not freed");
    }
    return law_held();
}

enum HostControl {
    HOST_GIVEN,
    HOST_GIVEN_BEFORE_JOINING,
    HOST_RELEASED_TO,
};

LawVerdict host_fires(HostControl p_way, const char *p_label) {
    Shot shot;
    shot.authority->set_denies(false);
    if (p_way == HOST_GIVEN_BEFORE_JOINING) {
        shot.rig.server()->entity_set_controller(shot.rig.entity_of("P"), 1);
    }
    shot.rig.join(-1, StringName("host"));
    if (p_way == HOST_GIVEN) {
        shot.rig.server()->entity_set_controller(shot.rig.entity_of("P"), 1);
    }
    if (p_way == HOST_RELEASED_TO) {
        const Ref<netw::NetwPromise> released
            = shot.rig.client(0)->entity_release_control(
                shot.rig.entity_of(StringName("P"), 0),
                1
            );
        shot.rig.step_ticks(ANSWER_TICKS);
        if (!released->get_is_completed()) {
            return law_broken("the release did not complete");
        }
    }
    shot.authority->fire(shot.server_tick());
    if (shot.authority->requests() != 1
        || shot.authority->confirmations() != 1) {
        return law_broken(
            "%s, %d run(s) and %d confirmation(s) as the request returned",
            p_label,
            shot.authority->requests(),
            shot.authority->confirmations()
        );
    }
    return law_held();
}

LawVerdict host_controlled_confirms(const World &) {
    const LawVerdict given = host_fires(HOST_GIVEN, "given");
    if (!given.held) {
        return given;
    }
    const LawVerdict early
        = host_fires(HOST_GIVEN_BEFORE_JOINING, "given before joining");
    if (!early.held) {
        return early;
    }
    return host_fires(HOST_RELEASED_TO, "released to peer 1");
}

LawVerdict confirm_keeps(const World &) {
    Shot shot;
    shot.authority->set_denies(false);
    const CallLog log;
    shot.owner->armed_action()->confirm(log.callable(StringName("confirmed")));
    shot.owner->fire(shot.client_tick());
    shot.rig.step_ticks(ANSWER_TICKS);
    if (log.count(StringName("confirmed")) != 1) {
        return law_broken(
            "confirm ran %d time(s)",
            log.count(StringName("confirmed"))
        );
    }
    if (shot.owner->ghost() == nullptr
        || shot.owner->ghost()->is_queued_for_deletion()) {
        return law_broken("the ghost confirm kept was freed");
    }
    return law_held();
}

LawVerdict arguments_in_order(const World &) {
    Shot shot(&declare_nothing, true, "_server_action_carrying");
    shot.owner->fire(
        shot.client_tick(),
        netw::gd::array_of(Vector2(3.0, -4.0), 7)
    );
    shot.rig.step_ticks(ANSWER_TICKS);
    const Array carried = shot.authority->carried_arguments();
    if (shot.authority->requests() != 1 || carried.size() != 2) {
        return law_broken("%d argument(s) arrived", int(carried.size()));
    }
    if (Vector2(carried[0]) != Vector2(3.0, -4.0) || int(carried[1]) != 7) {
        return law_broken("the arguments arrived out of order");
    }
    return law_held();
}

LawVerdict two_actions_apart(const World &) {
    Shot shot;
    const CallLog log;
    const Callable second(shot.owner, StringName("_predict_ghost"));
    Ref<NetwActionConfig> other;
    other.instantiate();
    other->set_authority(second);
    netw::session_decl::declare_action(
        shot.owner,
        second.get_method(),
        other
    );
    other->predict(second)
        ->confirm(log.callable(StringName("confirmed")))
        ->revert(log.callable(StringName("denied")));
    const int64_t tick = shot.client_tick();
    shot.owner->fire(tick);
    shot.rig.client(0)->action_ask(second, tick, Array());
    shot.rig.step_ticks(ANSWER_TICKS);
    if (shot.owner->ghosts() != 2 || shot.owner->denials() != 1) {
        return law_broken(
            "%d ghost(s) and %d denial(s) on the first action",
            shot.owner->ghosts(),
            shot.owner->denials()
        );
    }
    const int answered = log.count(StringName("denied"))
        + log.count(StringName("confirmed"));
    if (answered != 1) {
        return law_broken("the second action heard %d answer(s)", answered);
    }
    return law_held();
}

LawVerdict aligned_runs_at_tick(const World &) {
    Shot shot;
    shot.declare_timing(NetwActionConfig::TIMING_TICK_ALIGNED);
    const int64_t view = shot.server_tick() + 3;
    shot.owner->fire(view);
    shot.rig.step_ticks(3);
    if (shot.authority->requests() != 0) {
        return law_broken("the server ran it before its tick");
    }
    shot.rig.step_ticks(1);
    if (shot.authority->requests() != 1) {
        return law_broken(
            "the server ran it %d time(s)",
            shot.authority->requests()
        );
    }
    if (shot.authority->executed_tick() != view
        || shot.authority->viewed_tick() != view) {
        return law_broken(
            "ran at %d viewing %d for %d",
            int(shot.authority->executed_tick()),
            int(shot.authority->viewed_tick()),
            int(view)
        );
    }
    return law_held();
}

LawVerdict aligned_runs_past_a_send(const World &) {
    Shot shot(&declare_nothing, true, "_server_action", true, 3);
    shot.authority->set_denies(false);
    shot.declare_timing(NetwActionConfig::TIMING_TICK_ALIGNED_STATE_READY);
    shot.rig.step_ticks(ANSWER_TICKS);
    const int64_t view = shot.server_tick() + 9;
    shot.owner->fire(view);
    shot.rig.step_ticks(ANSWER_TICKS);
    if (shot.authority->requests() != 1 || shot.owner->denials() != 0
        || shot.authority->viewed_tick() != view || shot.fallbacks() != 0) {
        return law_broken(
            "%d run(s), %d denial(s) and %d fallback(s), viewing %d for %d",
            shot.authority->requests(),
            shot.owner->denials(),
            int(shot.fallbacks()),
            int(shot.authority->viewed_tick()),
            int(view)
        );
    }
    return law_held();
}

LawVerdict immediate_runs_on_arrival(const World &) {
    Shot shot;
    shot.declare_timing(NetwActionConfig::TIMING_IMMEDIATE);
    const int64_t ahead = shot.server_tick() + 6;
    shot.owner->fire(ahead);
    shot.step_until_run(ANSWER_TICKS);
    if (shot.authority->requests() != 1) {
        return law_broken("the server never ran it");
    }
    if (shot.authority->requested_tick() != ahead
        || shot.authority->executed_tick() >= ahead) {
        return law_broken(
            "asked %d, ran at %d",
            int(shot.authority->requested_tick()),
            int(shot.authority->executed_tick())
        );
    }
    return law_held();
}

void declare_patient_gate(LoopbackRig &p_rig) {
    Netw::configure_lagcomp(p_rig.branch(-1))->input_gate_deadline(24);
}

LawVerdict state_ready_waits(const World &) {
    Shot shot(&declare_patient_gate);
    shot.rig.conditions(-1, netw_test::polls(8), shot.rig.peer_id(0));
    shot.declare_timing(NetwActionConfig::TIMING_TICK_ALIGNED_STATE_READY);
    const int64_t view = shot.server_tick() + 3;
    shot.owner->fire(view);
    shot.rig.step_ticks(5);
    if (shot.authority->requests() != 0) {
        return law_broken("the server ran it before the input arrived");
    }
    shot.step_until_run(20);
    if (shot.authority->requests() != 1
        || shot.authority->viewed_tick() != view) {
        return law_broken(
            "%d run(s) viewing %d",
            shot.authority->requests(),
            int(shot.authority->viewed_tick())
        );
    }
    if (shot.fallbacks() != 0) {
        return law_broken("%d fallback(s)", int(shot.fallbacks()));
    }
    return law_held();
}

void declare_short_gate(LoopbackRig &p_rig) {
    Netw::configure_lagcomp(p_rig.branch(-1))->input_gate_deadline(2);
}

LawVerdict state_ready_gives_up(const World &) {
    Shot shot(&declare_short_gate);
    shot.declare_timing(NetwActionConfig::TIMING_TICK_ALIGNED_STATE_READY);
    const int64_t view = shot.server_tick() + 3;
    shot.owner->fire(view);
    shot.rig.step_ticks(6);
    if (shot.authority->requests() != 1
        || shot.authority->viewed_tick() != view) {
        return law_broken(
            "%d run(s) viewing %d",
            shot.authority->requests(),
            int(shot.authority->viewed_tick())
        );
    }
    if (shot.fallbacks() != 1) {
        return law_broken("%d fallback(s)", int(shot.fallbacks()));
    }
    return law_held();
}

LawVerdict future_denied(const World &) {
    Shot shot;
    shot.declare_timing(NetwActionConfig::TIMING_TICK_ALIGNED);
    shot.owner->fire(shot.server_tick() + 60);
    shot.rig.step_ticks(ANSWER_TICKS);
    if (shot.authority->requests() != 0 || shot.owner->denials() != 1) {
        return law_broken(
            "%d run(s) and %d denial(s)",
            shot.authority->requests(),
            shot.owner->denials()
        );
    }
    return law_held();
}

LawVerdict former_controller_refused(const World &) {
    FaultWatch faults;
    faults.expect_warning("*[NW0021]*", 1);
    faults.expect_warning("*[NW0007]*", 1);
    Shot shot;
    shot.rig.conditions(-1, netw_test::polls(4), shot.rig.peer_id(0));
    shot.owner->fire(shot.client_tick());
    shot.rig.server()->entity_set_controller(shot.rig.entity_of("P"), 1);
    shot.rig.step_ticks(ANSWER_TICKS);
    if (shot.authority->requests() != 0) {
        return law_broken("the server ran a shot from a former controller");
    }
    return law_held();
}

Ref<netw::NetwLagCompensationConfig> tight_future;

void declare_tight_future(LoopbackRig &p_rig) {
    tight_future = Netw::configure_lagcomp(p_rig.branch(-1));
    tight_future->max_future_action(2);
}

LawVerdict settings_sealed(const World &) {
    FaultWatch faults;
    faults.expect_error("*Netw.configure_lagcomp*[NW0019]*", 1);
    Shot shot(&declare_tight_future);
    const Ref<netw::NetwLagCompensationConfig> draft = tight_future;
    tight_future.unref();
    shot.declare_timing(NetwActionConfig::TIMING_TICK_ALIGNED);
    shot.owner->fire(shot.server_tick() + 5);
    shot.rig.step_ticks(ANSWER_TICKS);
    draft->max_future_action(60);
    shot.owner->fire(shot.server_tick() + 5);
    shot.rig.step_ticks(ANSWER_TICKS);
    if (shot.authority->requests() != 0 || shot.owner->denials() != 2) {
        return law_broken(
            "%d run(s) and %d denial(s)",
            shot.authority->requests(),
            shot.owner->denials()
        );
    }
    return law_held();
}

LawVerdict refused_before_predict(Shot &p_shot) {
    p_shot.owner->fire(0);
    p_shot.owner->fire(1);
    p_shot.rig.step_ticks(5);
    if (p_shot.owner->ghosts() != 0 || p_shot.authority->requests() != 0) {
        return law_broken(
            "%d ghost(s) and %d run(s)",
            p_shot.owner->ghosts(),
            p_shot.authority->requests()
        );
    }
    if (p_shot.owner->denials() != 0) {
        return law_broken("a refused request was denied too");
    }
    return law_held();
}

LawVerdict two_declarations_refuse(const World &) {
    FaultWatch faults;
    Shot shot(&declare_nothing, false);
    netw::Netw::configure_lagcomp(child_of(shot.rig.branch(0), "One"))
        ->max_future_action(4);
    netw::Netw::configure_lagcomp(child_of(shot.rig.branch(0), "Two"))
        ->max_future_action(5);
    const LawVerdict refused = refused_before_predict(shot);
    if (!refused.held) {
        return refused;
    }
    if (faults.errors_containing("[NW0034]") < 1) {
        return law_broken("no NW0034 named the two declarations");
    }
    return law_held();
}

LawVerdict departed_declaration_refuses(const World &) {
    FaultWatch faults;
    Shot shot(&declare_nothing, false);
    Node *scope = child_of(shot.rig.branch(0), "Settings");
    netw::Netw::configure_lagcomp(scope)->max_future_action(4);
    scope->get_parent()->remove_child(scope);
    shot.orphan = scope;
    const LawVerdict refused = refused_before_predict(shot);
    if (!refused.held) {
        return refused;
    }
    if (faults.errors_containing("[NW0041]") < 1) {
        return law_broken("no NW0041 named the departed declaration");
    }
    return law_held();
}

Node *twice_declared = nullptr;

void declare_twice(LoopbackRig &p_rig) {
    twice_declared = child_of(p_rig.branch(0), "Settings");
    Netw::configure_lagcomp(twice_declared)->max_future_action(4);
}

LawVerdict second_declaration_null(const World &) {
    FaultWatch faults;
    faults.expect_error("*Netw.configure_lagcomp*[NW0034]*", 1);
    Shot shot(&declare_twice);
    const Ref<netw::NetwLagCompensationConfig> again
        = Netw::configure_lagcomp(twice_declared);
    twice_declared = nullptr;
    if (again.is_valid()) {
        return law_broken("the second declaration answered a draft");
    }
    shot.owner->fire(shot.client_tick());
    shot.rig.step_ticks(ANSWER_TICKS);
    if (shot.owner->ghosts() != 1 || shot.owner->denials() != 1) {
        return law_broken(
            "%d ghost(s) and %d denial(s)",
            shot.owner->ghosts(),
            shot.owner->denials()
        );
    }
    return law_held();
}

LawVerdict bound_spawn_shown_at_tick(const World &) {
    const Ref<Script> script = netw_test::minted_script(GUN);
    if (script.is_null()) {
        return law_broken("the gun script did not compile");
    }
    gun_script = script.ptr();
    LoopbackRig rig(2);
    rig.mount();
    rig.declare_world(WorldDecl().clocked(30, 3).lag_compensated());
    Array args;
    args.push_back(String("Gun"));
    Array types;
    types.push_back(int(Variant::STRING));
    const int route = rig.spawn_registered(
        StringName("gun"),
        callable_mp_static(&build_gun),
        args,
        types,
        nullptr,
        rig.player(0)->get_id()
    );
    gun_script = nullptr;
    rig.server()->entity_set_controller(rig.spawned_entity(), rig.peer_id(0));
    rig.step_ticks(2);

    Node *owner = rig.route_node(route, 0);
    Node *bystander = rig.route_node(route, 1);
    Node *served = rig.route_node(route, -1);
    if (owner == nullptr || bystander == nullptr || served == nullptr) {
        return law_broken("a peer holds no gun");
    }
    const int64_t view = rig.client(0)->clock_get_tick() + 2;
    owner->call(StringName("shoot"), view);
    if (int(owner->get(StringName("ghosts"))) != 1) {
        return law_broken("the owner drew no ghost");
    }

    Node2D *hidden = nullptr;
    for (int at = 0; at < ANSWER_TICKS && hidden == nullptr; ++at) {
        rig.step_ticks(1);
        hidden = Object::cast_to<Node2D>(
            bystander->call(StringName("bolt")).operator Object *()
        );
    }
    if (hidden == nullptr) {
        return law_broken("the bystander never received the bolt");
    }
    if (rig.client(1)->clock_get_tick() - 3 >= view) {
        return law_broken("the bolt arrived after its tick, so none was held");
    }
    if (hidden->is_visible()) {
        return law_broken("the bystander drew the bolt before its tick");
    }
    rig.step_ticks(6);
    if (!hidden->is_visible()) {
        return law_broken("the bystander never drew the bolt");
    }
    if (int(owner->get(StringName("confirmed"))) != 1) {
        return law_broken(
            "the owner heard %d confirmation(s)",
            int(owner->get(StringName("confirmed")))
        );
    }
    return law_held();
}

struct Reentry {
    Shot *shot = nullptr;
    int calls = 0;
    bool applying = false;
};

Reentry reentry;

void fire_from_confirmed() {
    ++reentry.calls;
    netw::NetwMultiplayer *client = reentry.shot->rig.client(0);
    reentry.applying = client->config_is_applying();
    reentry.shot->owner->fire(reentry.shot->client_tick() + 1);
}

LawVerdict confirmed_fires_after_setup(const World &) {
    FaultWatch faults;
    Shot shot(&declare_nothing, false);
    shot.authority->set_denies(false);
    REQUIRE(shot.rig.server()->lagcomp_require("the server") == OK);
    netw::NetwMultiplayer *client = shot.rig.client(0);
    if (client->lagcomp_is_configured()) {
        return law_broken("lag compensation was set up before the scene");
    }
    const StringName entity("Gun");
    netw::NetwEntity::of(shot.owner)->set_name(entity);
    reentry = Reentry{&shot};
    client->lagcomp_effect_arm(entity, Callable(), 0);
    client->lagcomp_effect_watch(
        entity,
        callable_mp_static(&fire_from_confirmed),
        Callable()
    );
    shot.owner->fire(shot.client_tick());
    shot.rig.step_ticks(ANSWER_TICKS);
    const Reentry seen = reentry;
    reentry = Reentry();
    if (seen.calls != 1 || seen.applying) {
        return law_broken(
            "confirmed ran %d time(s), inside the setup %d",
            seen.calls,
            int(seen.applying)
        );
    }
    if (faults.errors_containing("[NW1008]") != 0) {
        return law_broken("the shot from confirmed reported NW1008");
    }
    if (shot.owner->ghosts() != 2 || shot.authority->requests() != 2
        || shot.owner->confirmations() != 2) {
        return law_broken(
            "%d ghost(s), %d run(s) and %d confirmation(s) for two shots",
            shot.owner->ghosts(),
            shot.authority->requests(),
            shot.owner->confirmations()
        );
    }
    return law_held();
}

LawVerdict declared_server_runs(const World &) {
    Shot shot(&declare_nothing, false, "_server_action", false);
    const StringName method("_server_action");
    const Ref<NetwActionConfig> declared
        = Netw::configure_action(Callable(shot.authority, method));
    if (declared.is_null()) {
        return law_broken("the declaration answered nothing");
    }
    declared->timing(NetwActionConfig::TIMING_TICK_ALIGNED)->timeout_ticks(9);
    const Ref<NetwActionConfig> found
        = netw::action_declared_on(shot.authority, method);
    if (found != declared
        || found->get_timing() != NetwActionConfig::TIMING_TICK_ALIGNED
        || found->get_timeout_ticks() != 9) {
        return law_broken("the declaration did not read back as written");
    }
    shot.rig.server()->config_settle();
    shot.authority->set_denies(false);
    shot.owner->fire(shot.client_tick());
    shot.rig.step_ticks(ANSWER_TICKS);
    if (shot.authority->requests() != 1 || shot.owner->confirmations() != 1) {
        return law_broken(
            "%d run(s) and %d confirmation(s)",
            shot.authority->requests(),
            shot.owner->confirmations()
        );
    }
    return law_held();
}

LawVerdict undeclared_method_denied(const World &) {
    Shot shot;
    shot.authority->set_denies(false);
    netw::NetwMultiplayer *client = shot.rig.client(0);
    REQUIRE(client->lagcomp_require("the client") == OK);
    const CallLog log;
    netw::session::ActionRequest forged;
    forged.method = StringName("_server_action_carrying");
    forged.view_tick = shot.client_tick();
    const StringName key = client->lagcomp_action_key(
        netw::NetwEntity::of(shot.owner)->get_rid_handle(),
        forged.view_tick,
        forged.method
    );
    client->lagcomp_effect_arm(key, Callable(), 0);
    client->lagcomp_effect_watch(
        key,
        log.callable(StringName("confirmed")),
        log.callable(StringName("denied"))
    );
    forged.data
        = netw::gd::var_to_bytes(netw::gd::array_of(Vector2(3.0, -4.0), 7));
    client->send_to(
        MultiplayerPeer::TARGET_PEER_SERVER,
        netw::NetwEntity::of(shot.owner)->get_route(),
        netw::wire::builtin_channel("ACTION"),
        netw::session::frame_write(forged),
        true,
        0,
        String()
    );
    shot.rig.step_ticks(ANSWER_TICKS);
    if (shot.authority->requests() != 0) {
        return law_broken("the server ran the undeclared method");
    }
    if (log.count(StringName("denied")) != 1
        || log.count(StringName("confirmed")) != 0) {
        return law_broken(
            "the asker heard %d denial(s) and %d confirmation(s)",
            log.count(StringName("denied")),
            log.count(StringName("confirmed"))
        );
    }
    return law_held();
}

LawVerdict refused_call_denied(const World &) {
    FaultWatch faults;
    faults.expect_error("*The Netw.configure_action method*[NW1002]*", 1);
    Shot shot;
    shot.authority->set_denies(false);
    shot.owner->fire(shot.client_tick(), netw::gd::array_of(7));
    shot.rig.step_ticks(ANSWER_TICKS);
    if (shot.authority->requests() != 0 || shot.owner->denials() != 1
        || shot.owner->confirmations() != 0) {
        return law_broken(
            "%d run(s), %d denial(s) and %d confirmation(s)",
            shot.authority->requests(),
            shot.owner->denials(),
            shot.owner->confirmations()
        );
    }
    return law_held();
}

constexpr const char *PATIENT = R"(extends Node2D

var entered := 0
var returned := 0

func slow_action(ctx: NetwActionRequest) -> void:
	entered += 1
	await get_tree().process_frame
	returned += 1
)";

LawVerdict awaiting_confirms_on_return(const World &) {
    const Ref<Script> script = netw_test::minted_script(PATIENT);
    if (script.is_null()) {
        return law_broken("the patient script did not compile");
    }
    Shot shot(&declare_nothing, true, "slow_action", false);
    shot.authority->set_script(script);
    shot.authority->arm_action(shot.rig.server(), StringName("slow_action"));
    shot.rig.server()->config_settle();
    shot.owner->fire(shot.client_tick());
    shot.rig.step_ticks(ANSWER_TICKS);
    if (int(shot.authority->get(StringName("entered"))) != 1
        || shot.owner->confirmations() != 0 || shot.owner->denials() != 0) {
        return law_broken(
            "%d run(s), %d confirmation(s) and %d denial(s) while it waits",
            int(shot.authority->get(StringName("entered"))),
            shot.owner->confirmations(),
            shot.owner->denials()
        );
    }
    netw::gd::scene_tree()->emit_signal(StringName("process_frame"));
    shot.rig.step_ticks(ANSWER_TICKS);
    if (int(shot.authority->get(StringName("returned"))) != 1
        || shot.owner->confirmations() != 1) {
        return law_broken(
            "%d return(s) and %d confirmation(s) one frame later",
            int(shot.authority->get(StringName("returned"))),
            shot.owner->confirmations()
        );
    }
    return law_held();
}

const Row ROWS[] = {
    {"a shot the server denies",
     "the owner draws a ghost at once, hears denied, and the ghost is freed",
     &denied_reverts},
    {"a shot the server runs without denying it",
     "the owner hears confirmed and the ghost is freed",
     &accepted_confirms},
    {"a shot the host fires for a player given to it, given before it "
     "joined, or released to peer 1",
     "the host runs it and hears confirmed before the request returns",
     &host_controlled_confirms},
    {"a shot whose confirm callback is set",
     "confirm runs once and the ghost it was handed stays",
     &confirm_keeps},
    {"a shot carrying a Vector2 and an int",
     "the server method receives both after the context, in order",
     &arguments_in_order},
    {"two actions on one player fired at one tick",
     "each hears its own answer",
     &two_actions_apart},
    {"a tick aligned shot three ticks ahead of the server",
     "the server runs it at that tick and not before",
     &aligned_runs_at_tick},
    {"a state ready shot nine ticks ahead of the server while the session "
     "sends on every tenth tick",
     "the server runs it viewing that tick with no denial and no fallback",
     &aligned_runs_past_a_send},
    {"an immediate shot six ticks ahead of the server",
     "the server runs it on arrival and still reads the asked tick",
     &immediate_runs_on_arrival},
    {"a state ready shot over a slow uplink under a long gate deadline",
     "the server waits for the input and runs it at its tick, with no "
     "fallback",
     &state_ready_waits},
    {"a state ready shot under a two tick gate deadline",
     "the server runs it at its tick once the deadline passes, and counts "
     "one fallback",
     &state_ready_gives_up},
    {"a shot sixty ticks ahead of the server",
     "the server never runs it and the owner hears denied",
     &future_denied},
    {"a shot in flight when control passes to the server",
     "the server never runs it and warns NW0021 once",
     &former_controller_refused},
    {"max_future_action written after the declaring call, then changed "
     "once applied",
     "a shot past the first limit is denied both times and the change "
     "reports NW0019",
     &settings_sealed},
    {"two nodes on the client declaring lag compensation",
     "a request draws no ghost, sends nothing and reports NW0034",
     &two_declarations_refuse},
    {"the client's declaring node leaving the tree before a request",
     "a request draws no ghost, sends nothing and reports NW0041",
     &departed_declaration_refuses},
    {"one node declaring lag compensation twice",
     "the second answers null with NW0034, and a shot still runs",
     &second_declaration_null},
    {"a shot whose server method binds a node it spawns, seen by a second "
     "client",
     "the second client holds the node hidden until its display reaches the "
     "shot's tick, and the owner hears confirmed",
     &bound_spawn_shown_at_tick},
    {"an effect confirmed by the setup of lag compensation, whose confirmed "
     "callback fires a shot",
     "the callback runs after the setup and its shot is drawn and run with "
     "no NW1008",
     &confirmed_fires_after_setup},
    {"a shot at a server that only declared the action, tick aligned with a "
     "nine tick timeout",
     "the declaration reads back as written, and the server runs the shot",
     &declared_server_runs},
    {"a request naming a method of the player that the game never declared "
     "as an action",
     "the server does not run the method and the asker hears denied",
     &undeclared_method_denied},
    {"a shot carrying one argument more than its server method takes",
     "the server reports NW1002 once and the owner hears denied",
     &refused_call_denied},
    {"a shot whose server method awaits one frame before it returns",
     "the owner hears nothing while it waits, and confirmed once it returns",
     &awaiting_confirms_on_return},
};

TEST_CASE(
    "[Networked][LagComp][Action][SceneTree] a shot is predicted, then "
    "confirmed or reverted, runs on the tick its timing names, and a shot "
    "the server cannot take is refused"
) {
    const World world = {String("a server and one client on a loopback")};
    for (const Row &row : ROWS) {
        NETW_CELL(row, world);
        NETW_LAW_HOLDS(row, world);
    }
}

} // namespace TestAction

#endif
