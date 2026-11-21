#include "netw/api/netw_multiplayer.hpp"

#include "godot/node.hpp"
#include "godot/object.hpp"
#include "netw/api/despawn_config.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/replication_core.hpp"
#include "netw/colors.hpp"
#include "netw/game_call.hpp"
#include "netw/profile.hpp"
#include "netw/repl/spawn_plan.hpp"
#include "netw/script/model.hpp"
#include "netw/session/event_plane.hpp"
#include "netw/spawn/pipeline.hpp"
#include "netw/spawn/record.hpp"
#include "netw/spawn/spawner_compat.hpp"
#include "netw/wire/registry.hpp"

using namespace godot;

namespace netw {

namespace {

const char *KEY_SWEEP = "spawn-visibility-sweep";
const char *KEY_FLUSH = "spawn-carrier-flush";
const double CLOCKLESS_TICKRATE = 30.0;

} // namespace

spawn::Pipeline *NetwMultiplayer::spawn_plane() const {
    ReplicationCore *plane = get_replication_plane();
    return plane != nullptr ? plane->get_spawn_pipeline() : nullptr;
}

spawn::Book *NetwMultiplayer::spawn_held_book() const {
    spawn::Pipeline *pipeline = spawn_plane();
    return pipeline != nullptr ? pipeline->get_spawn_book() : nullptr;
}

spawn::SpawnerRoster *NetwMultiplayer::spawner_roster() const {
    spawn::SpawnerCompat *adapter = spawner_adapter();
    return adapter != nullptr ? &adapter->get_roster() : nullptr;
}

bool NetwMultiplayer::spawner_produced(int64_t p_route) const {
    spawn::SpawnerRoster *roster = spawner_roster();
    return roster != nullptr && roster->produced(p_route);
}

bool NetwMultiplayer::spawn_fn_waits(int64_t p_route) const {
    spawn::Pipeline *pipeline = spawn_plane();
    return pipeline != nullptr
        && pipeline->get_spawn_book()->fn_waits_of(p_route);
}

PackedByteArray NetwMultiplayer::spawn_encode_spawn_frame(
    int64_t p_route,
    Node *p_node
) {
    spawn::Pipeline *pipeline = spawn_plane();
    return pipeline != nullptr ? pipeline->encode_spawn_frame(p_route, p_node)
                               : PackedByteArray();
}

void NetwMultiplayer::spawn_schedule_visibility_sweep() {
    if (spawn_plane() == nullptr || !is_host()) {
        return;
    }
    session_defer(
        callable_mp(this, &NetwMultiplayer::spawn_run_visibility_sweep),
        StringName(KEY_SWEEP)
    );
}

void NetwMultiplayer::spawn_sweep_now() {
    if (spawn_plane() == nullptr || !is_host()) {
        return;
    }
    session_cancel_deferred(StringName(KEY_SWEEP));
    spawn_run_visibility_sweep();
}

void NetwMultiplayer::spawn_run_visibility_sweep() {
    NETW_ZONE_NC("Spawn visibility sweep", colors::LIVENESS);
    spawn::Pipeline *pipeline = spawn_plane();
    if (pipeline == nullptr || !pipeline->has_peer()) {
        return;
    }
    if (!session_is_online()) {
        return;
    }
    const PackedInt32Array peers = spawn_peer_ids();

    spawn_refresh_anchors(spawner_roster());

    const LocalVector<repl::SpawnOp> plan
        = repl::reconcile(spawn_reconcile_rows(peers), peers);
    if (finish_stage_verdict(EventPlane::SPAWN_RECONCILE, OK, 0) != OK) {
        interest_leave_finish_sweep();
        return;
    }
    spawn_execute_plan(plan, wire::CHANNEL_SPAWN, wire::CHANNEL_HIDE);
    interest_leave_finish_sweep();
    spawn_defer_carrier_flush();
}

void NetwMultiplayer::spawn_defer_carrier_flush() {
    session_defer(
        callable_mp(this, &NetwMultiplayer::spawn_run_carrier_flush),
        StringName(KEY_FLUSH)
    );
}

void NetwMultiplayer::spawn_run_carrier_flush() {
    NETW_ZONE_NC("Spawn carrier flush", colors::LIVENESS);
    replication_flush_all_buffers();
}

Error NetwMultiplayer::spawn_declare_stage(
    const RID &p_handle,
    const Dictionary &p_facts
) {
    spawn::Pipeline *pipeline = spawn_plane();
    return pipeline != nullptr ? pipeline->declare_stage(p_handle, p_facts)
                               : ERR_UNAVAILABLE;
}

void NetwMultiplayer::spawn_free_despawned(int64_t p_route) {
    if (spawn_plane() == nullptr) {
        return;
    }
    const Ref<NetwEntity> entity = wrapper_for_route(p_route);
    if (entity.is_null()) {
        return;
    }
    spawn_free_node(p_route, entity->get_owner());
}

void NetwMultiplayer::spawn_act(spawn::Ops &p_ops) {
    for (; p_ops.acted < p_ops.rows.size(); ++p_ops.acted) {
        const spawn::NodeOp op = p_ops.rows[p_ops.acted];
        bool held = true;
        switch (op.kind) {
            case spawn::NodeOp::BUILD: {
                const spawn::Built::Outcome outcome
                    = spawn_build(p_ops.build(), op.sender, p_ops);
                if (op.requested) {
                    spawn_rule_request(op, outcome, p_ops.build());
                } else if (spawn::Pipeline *pipeline = spawn_plane()) {
                    pipeline->built(op, outcome, p_ops);
                }
                break;
            }
            case spawn::NodeOp::PLACE:
                spawn_place(p_ops);
                break;
            case spawn::NodeOp::MOVE:
                held = spawn_move(op);
                break;
            case spawn::NodeOp::FREE:
                held = spawn_free(op);
                break;
            case spawn::NodeOp::HIDE:
                held = spawn_hide(op);
                break;
            case spawn::NodeOp::DISCARD:
                spawn_discard(
                    Object::cast_to<Node>(gd::instance_from_id(op.node))
                );
                break;
            case spawn::NodeOp::ORPHAN:
                free_returned(
                    Variant(gd::instance_from_id(op.node)),
                    Variant()
                );
                break;
        }
        if (held) {
            continue;
        }
        if (spawn::Pipeline *pipeline = spawn_plane()) {
            pipeline->missed(op);
        }
    }
}

bool NetwMultiplayer::spawn_free(const spawn::NodeOp &p_op) {
    const int64_t route = p_op.route;
    const Ref<NetwEntity> entity = wrapper_for_route(route);
    if (entity.is_null() || entity->get_owner() == nullptr) {
        return false;
    }
    const RID handle = entity->get_rid_handle();
    const bool withheld = lifecycle_forget_withheld(handle);
    const double hooked = spawn_despawn_hook(entity->get_owner());
    const double linger = p_op.lingers ? hooked : 0.0;
    entity_remote_despawn(handle, p_op.reason, linger);
    if (withheld) {
        spawn_free_despawned(route);
        entity_settle_captured(entity->get_rid_handle());
        return true;
    }
    if (linger > 0.0) {
        double rate = CLOCKLESS_TICKRATE;
        const Clock &clock = clock_engine();
        if (clock.get_configured()) {
            rate = double(clock.get_tickrate());
        }
        session_defer_after(
            callable_mp(this, &NetwMultiplayer::spawn_free_despawned)
                .bind(route),
            StringName(vformat("spawn-free?%d", route)),
            int(Clock::pumps_for(linger, rate))
        );
        return true;
    }
    spawn_free_despawned(route);
    return true;
}

bool NetwMultiplayer::spawn_hide(const spawn::NodeOp &p_op) {
    const Ref<NetwEntity> entity = wrapper_for_route(p_op.route);
    if (!liveness_hide(p_op.route)) {
        return false;
    }
    if (entity.is_null()) {
        return true;
    }
    entity->_remote_hide();
    if (!p_op.keeps_node) {
        spawn_free_node(p_op.route, entity->get_owner());
    }
    return true;
}

void NetwMultiplayer::spawn_free_node(int64_t p_route, Node *p_node) {
    if (p_node == nullptr) {
        return;
    }
    spawn_carry_land_out_of(p_node);
    spawn_unplace_node(p_node);
    if (spawn::SpawnerCompat *adapter = spawner_adapter()) {
        adapter->emit_despawned(p_route, p_node);
    }
    p_node->queue_free();
}

void NetwMultiplayer::spawn_discard(Node *p_node) {
    if (p_node == nullptr) {
        return;
    }
    if (p_node->get_parent() != nullptr) {
        p_node->get_parent()->remove_child(p_node);
    }
    p_node->queue_free();
}

double NetwMultiplayer::spawn_despawn_hook(Node *p_node) {
    const Ref<NetwDespawnConfig> cfg
        = netw::script::model::get_despawn_config(p_node->get_script());
    if (cfg.is_null()) {
        return 0.0;
    }
    const StringName hook = cfg->get_hook_method();
    if (!String(hook).is_empty() && p_node->has_method(hook)) {
        game_call(Callable(p_node, hook), Array(), game_calls::DESPAWN_HOOK);
    }
    return cfg->get_linger_seconds();
}

void NetwMultiplayer::spawn_handle_spawn_frame(
    const PackedByteArray &p_payload,
    int64_t p_sender
) {
    if (spawn::Pipeline *pipeline = spawn_plane()) {
        spawn::Ops ops;
        pipeline->receive_spawn(p_payload, p_sender, ops);
        spawn_act(ops);
    }
}

void NetwMultiplayer::spawn_handle_despawn_frame(
    const PackedByteArray &p_payload,
    int64_t p_sender
) {
    spawn::Pipeline *pipeline = spawn_plane();
    if (pipeline == nullptr) {
        return;
    }
    if (is_host() && p_sender != SERVER_PEER) {
        spawn_admit_controller_op(
            lifecycle::Kind::DESPAWN,
            p_payload,
            p_sender
        );
        return;
    }
    spawn::Ops ops;
    pipeline->handle_despawn_frame(p_payload, p_sender, ops);
    spawn_act(ops);
}

void NetwMultiplayer::spawn_admit_controller_op(
    lifecycle::Kind p_kind,
    const PackedByteArray &p_payload,
    int64_t p_sender
) {
    spawn::Pipeline *pipeline = spawn_plane();
    const Error admitted
        = pipeline->admit_controller_frame(p_kind, p_sender, p_payload);
    if (admitted != OK) {
        pipeline->count_lifecycle_op(false);
        lifecycle_refuse_op(p_sender, p_kind, p_payload, admitted);
        return;
    }
    pipeline->count_lifecycle_op(
        p_kind == lifecycle::Kind::DESPAWN
            ? lifecycle_admit_despawn(p_sender, p_payload)
            : lifecycle_admit_move(p_sender, p_payload)
    );
}

void NetwMultiplayer::spawn_wait_reparent(
    int64_t p_anchor_route,
    const PackedByteArray &p_payload,
    int64_t p_sender,
    int64_t p_timeout_ticks
) {
    liveness_when_live(
        p_anchor_route,
        callable_mp(this, &NetwMultiplayer::spawn_handle_reparent_frame)
            .bind(p_payload, p_sender),
        p_timeout_ticks,
        Callable()
    );
}

bool NetwMultiplayer::spawn_move(const spawn::NodeOp &p_op) {
    const Ref<NetwEntity> entity = wrapper_for_route(p_op.route);
    if (entity.is_null() || entity->get_owner() == nullptr) {
        return false;
    }
    Node *parent = anchor_resolve(p_op.parent);
    if (parent == nullptr
        && lifecycle_note_detached_anchor(
            p_op.route,
            p_op.revision,
            p_op.author
        )) {
        return true;
    }
    if (parent == nullptr) {
        fault::raise(
            this,
            NETW_FAULT_SPAWN_MISMATCH,
            {String(),
             String(),
             String("its new parent, route ")
                 + String::num_int64(p_op.parent.route),
             p_op.sender,
             p_op.route}
        );
        return false;
    }
    spawn_reparent_node(
        entity->get_owner(),
        parent,
        callable_mp(this, &NetwMultiplayer::scene_adopt_entity),
        p_op.route,
        p_op.revision,
        p_op.author
    );
    return true;
}

void NetwMultiplayer::spawn_handle_hide_frame(
    const PackedByteArray &p_payload,
    int64_t p_sender
) {
    if (spawn::Pipeline *pipeline = spawn_plane()) {
        spawn::Ops ops;
        pipeline->handle_hide_frame(p_payload, p_sender, ops);
        spawn_act(ops);
    }
}

void NetwMultiplayer::spawn_handle_reparent_frame(
    const PackedByteArray &p_payload,
    int64_t p_sender
) {
    spawn::Pipeline *pipeline = spawn_plane();
    if (pipeline == nullptr) {
        return;
    }
    if (is_host() && p_sender != SERVER_PEER) {
        spawn_admit_controller_op(
            lifecycle::Kind::REPARENT,
            p_payload,
            p_sender
        );
        return;
    }
    spawn::Ops ops;
    pipeline->handle_reparent_frame(p_payload, p_sender, ops);
    spawn_act(ops);
}

void NetwMultiplayer::spawn_retry_parked(int64_t p_route) {
    if (spawn::Pipeline *pipeline = spawn_plane()) {
        spawn::Ops ops;
        pipeline->retry_parked(p_route, ops);
        spawn_act(ops);
    }
}

void NetwMultiplayer::spawn_wait_route(
    int64_t p_dep_route,
    int64_t p_route,
    int64_t p_timeout_ticks
) {
    liveness_when_live(
        p_dep_route,
        callable_mp(this, &NetwMultiplayer::spawn_retry_parked).bind(p_route),
        p_timeout_ticks,
        callable_mp(this, &NetwMultiplayer::spawn_expire_parked).bind(p_route)
    );
}

void NetwMultiplayer::spawn_expire_parked(int64_t p_route) {
    if (spawn::Pipeline *pipeline = spawn_plane()) {
        spawn::Ops ops;
        pipeline->expire_parked(p_route, ops);
        spawn_act(ops);
    }
}

void NetwMultiplayer::spawn_expire_spawner_parked(
    spawn::Pipeline *p_pipeline
) {
    const PackedInt64Array waiting
        = p_pipeline->get_park().waiting_on(spawn::Park::WAIT_SPAWNER);
    if (waiting.is_empty()) {
        return;
    }
    const int64_t now = p_pipeline->park_now();
    for (int at = 0; at < waiting.size(); ++at) {
        spawn::Ops ops;
        p_pipeline->expire_spawner_route(waiting[at], now, ops);
        spawn_act(ops);
    }
}

void NetwMultiplayer::spawn_retry_scene_parked_spawns() {
    NETW_ZONE_NC("Spawn retry scene parked spawns", colors::LIVENESS);
    spawn::Pipeline *pipeline = spawn_plane();
    if (pipeline == nullptr) {
        return;
    }
    spawn_expire_spawner_parked(pipeline);
    const PackedInt64Array waiting
        = pipeline->get_park().waiting_on(spawn::Park::WAIT_SPAWNER);
    for (int at = 0; at < waiting.size(); ++at) {
        spawn::Ops ops;
        pipeline->retry_spawner_route(waiting[at], ops);
        spawn_act(ops);
    }
    pipeline->settle_spawner_retry();
}

void NetwMultiplayer::spawn_retry_adopt() {
    spawn::Pipeline *pipeline = spawn_plane();
    if (pipeline == nullptr) {
        return;
    }
    const PackedInt64Array waiting
        = pipeline->get_park().waiting_on(spawn::Park::WAIT_ADOPT);
    if (waiting.is_empty()) {
        return;
    }
    const int64_t now = pipeline->park_now();
    for (int at = 0; at < waiting.size(); ++at) {
        spawn::Ops ops;
        const bool applied = pipeline->retry_adopt(waiting[at], now, ops);
        spawn_act(ops);
        if (applied) {
            pipeline->settle_adopt_retry(waiting[at]);
        }
    }
}

void NetwMultiplayer::spawn_retry_builds() {
    spawn::Pipeline *pipeline = spawn_plane();
    if (pipeline == nullptr || pipeline->get_park().size() == 0) {
        return;
    }
    spawn_expire_spawner_parked(pipeline);
    const PackedInt64Array waiting
        = pipeline->get_park().waiting_on(spawn::Park::WAIT_BUILD);
    for (int at = 0; at < waiting.size(); ++at) {
        spawn::Ops ops;
        pipeline->retry_build(waiting[at], ops);
        spawn_act(ops);
    }
}

void NetwMultiplayer::spawn_on_action_reveal_tick(
    double p_delta,
    int64_t p_tick
) {
    if (spawn::Pipeline *pipeline = spawn_plane()) {
        pipeline->on_action_reveal_tick(p_delta, p_tick);
    }
}

void NetwMultiplayer::spawn_apply_action_gate(
    int64_t p_route,
    const RID &p_entity
) {
    NETW_ZONE_NC("Spawn apply action gate", colors::LIVENESS);
    if (spawn::Pipeline *pipeline = spawn_plane()) {
        pipeline->apply_action_gate(p_route, p_entity);
    }
}

bool NetwMultiplayer::spawn_books_node(Node *p_node) const {
    spawn::Pipeline *pipeline = spawn_plane();
    return pipeline != nullptr && pipeline->books_node(p_node);
}

spawn::SpawnerCompat *NetwMultiplayer::spawner_adapter() const {
    ReplicationCore *plane = get_replication_plane();
    return plane != nullptr ? plane->get_spawner_compat() : nullptr;
}

void NetwMultiplayer::spawner_session_entered() {
    if (spawn::SpawnerCompat *adapter = spawner_adapter()) {
        adapter->on_session_entered();
    }
}

void NetwMultiplayer::spawner_session_ended() {
    if (spawn::SpawnerCompat *adapter = spawner_adapter()) {
        adapter->on_session_ended();
    }
}

void NetwMultiplayer::spawner_node_added(Node *p_node) {
    if (spawn::SpawnerCompat *adapter = spawner_adapter()) {
        adapter->on_node_added(p_node);
    }
}

void NetwMultiplayer::spawner_wrap_when_ready(Node *p_spawner) {
    if (spawn::SpawnerCompat *adapter = spawner_adapter()) {
        adapter->wrap_spawner(p_spawner);
        adapter->wrap_enrolled();
    }
}

void NetwMultiplayer::spawner_clear_session_state() {
    if (spawn::SpawnerCompat *adapter = spawner_adapter()) {
        adapter->clear_session_state();
    }
}

Node *NetwMultiplayer::spawner_wrapped_spawn(
    const Variant &p_data,
    const Callable &p_original
) {
    spawn::SpawnerCompat *adapter = spawner_adapter();
    return adapter != nullptr ? adapter->wrapped_spawn(p_data, p_original)
                              : nullptr;
}

} // namespace netw
