#include "netw/api/netw_multiplayer.hpp"

#include "godot/node.hpp"
#include "godot/object.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/replication_core.hpp"
#include "netw/colors.hpp"
#include "netw/profile.hpp"
#include "netw/spawn/pipeline.hpp"
#include "netw/spawn/record.hpp"
#include "netw/spawn/spawner_compat.hpp"

using namespace godot;

namespace netw {

spawn::Pipeline *NetwMultiplayer::spawn_plane() const {
    ReplicationCore *plane = get_replication_plane();
    return plane != nullptr ? plane->get_spawn_pipeline() : nullptr;
}

bool NetwMultiplayer::spawn_fn_waits(int64_t p_route) const {
    spawn::Pipeline *pipeline = spawn_plane();
    return pipeline != nullptr
        && pipeline->get_spawn_book()->fn_waits_of(p_route);
}

void NetwMultiplayer::spawn_on_peer_connected(int64_t p_peer_id) {
    if (spawn::Pipeline *pipeline = spawn_plane()) {
        pipeline->on_peer_connected(p_peer_id);
    }
}

PackedByteArray NetwMultiplayer::spawn_encode_spawn_frame(
    int64_t p_route,
    Node *p_node
) {
    spawn::Pipeline *pipeline = spawn_plane();
    return pipeline != nullptr ? pipeline->encode_spawn_frame(p_route, p_node)
                               : PackedByteArray();
}

void NetwMultiplayer::spawn_run_visibility_sweep() {
    if (spawn::Pipeline *pipeline = spawn_plane()) {
        pipeline->run_visibility_sweep();
    }
}

void NetwMultiplayer::spawn_run_carrier_flush() {
    if (spawn::Pipeline *pipeline = spawn_plane()) {
        pipeline->run_carrier_flush();
    }
}

Error NetwMultiplayer::spawn_declare_stage(
    const RID &p_handle,
    const Dictionary &p_facts
) {
    spawn::Pipeline *pipeline = spawn_plane();
    return pipeline != nullptr ? pipeline->declare_stage(p_handle, p_facts)
                               : ERR_UNAVAILABLE;
}

void NetwMultiplayer::spawn_on_armed_tree_entered(int64_t p_route) {
    if (spawn::Pipeline *pipeline = spawn_plane()) {
        pipeline->on_armed_tree_entered(p_route);
    }
}

void NetwMultiplayer::spawn_reconcile_nested_child(int64_t p_route) {
    if (spawn::Pipeline *pipeline = spawn_plane()) {
        pipeline->reconcile_nested_child(p_route);
    }
}

void NetwMultiplayer::spawn_note_nested_candidate(int64_t p_route) {
    if (spawn::Pipeline *pipeline = spawn_plane()) {
        pipeline->note_nested_candidate(p_route);
    }
}

bool NetwMultiplayer::spawn_readmit(
    Node *p_node,
    const spawn::Record &p_recipe
) {
    spawn::Pipeline *pipeline = spawn_plane();
    return pipeline != nullptr && pipeline->readmit(p_node, p_recipe);
}

void NetwMultiplayer::spawn_flush_armed_spawn(int64_t p_route) {
    if (spawn::Pipeline *pipeline = spawn_plane()) {
        pipeline->flush_armed_spawn(p_route);
    }
}

Node *NetwMultiplayer::spawn_build_adopt(
    Object *p_parent,
    const String &p_name
) {
    spawn::Pipeline *pipeline = spawn_plane();
    return pipeline != nullptr ? pipeline->build_adopt(p_parent, p_name)
                               : nullptr;
}

Node *NetwMultiplayer::spawn_build_spawner(
    Object *p_spawner,
    int64_t p_index,
    const Variant &p_data
) {
    spawn::Pipeline *pipeline = spawn_plane();
    return pipeline != nullptr
        ? pipeline->build_spawner(p_spawner, p_index, p_data)
        : nullptr;
}

Node *NetwMultiplayer::spawn_build_fn(
    const Callable &p_fn,
    const Array &p_args
) {
    spawn::Pipeline *pipeline = spawn_plane();
    return pipeline != nullptr ? pipeline->build_fn(p_fn, p_args) : nullptr;
}

Node *NetwMultiplayer::spawn_build_host_fn(
    Object *p_host,
    const StringName &p_method,
    const Array &p_args
) {
    spawn::Pipeline *pipeline = spawn_plane();
    return pipeline != nullptr
        ? pipeline->build_host_fn(p_host, p_method, p_args)
        : nullptr;
}

void NetwMultiplayer::spawn_free_despawned(int64_t p_route) {
    if (spawn::Pipeline *pipeline = spawn_plane()) {
        pipeline->free_despawned(p_route);
    }
}

void NetwMultiplayer::spawn_handle_spawn_frame(
    const PackedByteArray &p_payload,
    int64_t p_sender
) {
    if (spawn::Pipeline *pipeline = spawn_plane()) {
        pipeline->handle_spawn_frame(p_payload, p_sender);
    }
}

void NetwMultiplayer::spawn_handle_despawn_frame(
    const PackedByteArray &p_payload,
    int64_t p_sender
) {
    if (spawn::Pipeline *pipeline = spawn_plane()) {
        pipeline->handle_despawn_frame(p_payload, p_sender);
    }
}

void NetwMultiplayer::spawn_handle_hide_frame(
    const PackedByteArray &p_payload,
    int64_t p_sender
) {
    if (spawn::Pipeline *pipeline = spawn_plane()) {
        pipeline->handle_hide_frame(p_payload, p_sender);
    }
}

void NetwMultiplayer::spawn_handle_reparent_frame(
    const PackedByteArray &p_payload,
    int64_t p_sender
) {
    if (spawn::Pipeline *pipeline = spawn_plane()) {
        pipeline->handle_reparent_frame(p_payload, p_sender);
    }
}

void NetwMultiplayer::spawn_retry_parked(int64_t p_route) {
    if (spawn::Pipeline *pipeline = spawn_plane()) {
        pipeline->retry_parked(p_route);
    }
}

void NetwMultiplayer::spawn_expire_parked(int64_t p_route) {
    if (spawn::Pipeline *pipeline = spawn_plane()) {
        pipeline->expire_parked(p_route);
    }
}

void NetwMultiplayer::spawn_retry_scene_parked_spawns(
    int64_t p_route,
    const Ref<NetwEntity> &p_entity
) {
    NETW_ZONE_NC("Spawn retry scene parked spawns", colors::LIVENESS);
    if (spawn::Pipeline *pipeline = spawn_plane()) {
        pipeline->retry_scene_parked_spawns(p_route, p_entity);
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
    const Ref<NetwEntity> &p_entity
) {
    NETW_ZONE_NC("Spawn apply action gate", colors::LIVENESS);
    if (spawn::Pipeline *pipeline = spawn_plane()) {
        pipeline->apply_action_gate(p_route, p_entity);
    }
}

void NetwMultiplayer::spawn_schedule_visibility_sweep() {
    if (spawn::Pipeline *pipeline = spawn_plane()) {
        pipeline->schedule_visibility_sweep();
    }
}

bool NetwMultiplayer::spawn_books_node(Node *p_node) const {
    spawn::Pipeline *pipeline = spawn_plane();
    return pipeline != nullptr && pipeline->books_node(p_node);
}

Ref<NetwEntity> NetwMultiplayer::spawn_arm_consumed(
    Node *p_node,
    Object *p_spawner,
    int p_scene_index,
    const Variant &p_data
) {
    spawn::Pipeline *pipeline = spawn_plane();
    return pipeline != nullptr
        ? pipeline->arm_consumed_spawn(p_node, p_spawner, p_scene_index, p_data)
        : Ref<NetwEntity>();
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
