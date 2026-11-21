#pragma once

#include <cstdint>

#include "godot/callable.hpp"
#include "godot/node.hpp"
#include "godot/object.hpp"
#include "godot/rid.hpp"
#include "godot/variant.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/participant.hpp"
#include "netw/channel_book.hpp"
#include "netw/repl/property_binding.hpp"
#include "netw/repl/sync_compat.hpp"
#include "netw/repl/sync_model.hpp"
#include "netw/repl/sync_pipeline.hpp"
#include "netw/session/frames.hpp"
#include "netw/spawn/pipeline.hpp"
#include "netw/spawn/spawner_compat.hpp"
#include "netw/wire/registry.hpp"

namespace netw {

class NetwMultiplayer;

class ReplicationCore {
    friend class NetwMultiplayer;

private:
    NetwMultiplayer *session = nullptr;
    Clock *clock_engine = nullptr;
    store::Store *store_world = nullptr;
    NetwLivenessCore *liveness_core = nullptr;
#if defined(DEBUG_ENABLED)
    wire::NetworkProfiler *network_profiler = nullptr;
#endif

    NetwChannelBook *channels = nullptr;
    godot::LocalVector<godot::ObjectID> control_waiting;
    godot::LocalVector<godot::ObjectID> claim_imaging;
    NetwSyncModel sync_model;
    SyncPipeline sync_pipeline;
    spawn::Pipeline spawn_pipeline;
    spawn::SpawnerCompat spawner_compat;
    SyncCompat sync_compat;

    bool control_requests_ordered = true;

    godot::Error finish_receive_pass(
        int64_t p_frames,
        godot::Error p_verdict,
        int64_t p_sender
    );
    void dispatch_frame(
        int64_t p_route,
        int64_t p_comp,
        int64_t p_channel,
        const godot::PackedByteArray &p_payload,
        const godot::String &p_path,
        int64_t p_sender,
        bool p_reliable,
        int64_t p_seq
    );
    void receive_protocol(
        NetwMultiplayer *p_session,
        wire::Channel p_channel,
        const godot::PackedByteArray &p_payload,
        int64_t p_sender
    );
    void receive_routed(
        NetwMultiplayer *p_session,
        wire::Channel p_channel,
        const godot::Ref<NetwEntity> &p_entity,
        godot::Node *p_node,
        int64_t p_comp,
        const godot::PackedByteArray &p_payload,
        int64_t p_sender
    );
    void dispatch_reply(
        const godot::PackedByteArray &p_payload,
        int64_t p_sender
    );
    void settle_reply(
        int64_t p_txn,
        int64_t p_sender,
        int64_t p_route,
        int64_t p_comp,
        const godot::String &p_path
    );
    bool defers_unknown_route(int64_t p_channel, bool p_reliable) const;
    bool channel_addresses_a_set(int64_t p_channel) const;
#if defined(DEBUG_ENABLED)
    bool handler_names_arrival(int64_t p_channel) const;
#endif
    godot::Error admit_entity_frame(
        int64_t p_sender,
        int64_t p_route,
        int64_t p_channel,
        const godot::PackedByteArray &p_payload
    );
    void broadcast_table_frames(
        const godot::TypedArray<godot::PackedByteArray> &p_frames,
        bool p_reliable
    );
    void handle_table_frame(
        const godot::PackedByteArray &p_payload,
        int64_t p_sender
    );

public:
    ReplicationCore();

    void install(
        NetwMultiplayer *p_session,
        Clock *p_clock,
        store::Store *p_store,
        NetwLivenessCore *p_liveness,
        NetwChannelBook *p_channels,
        repl::BindingBook *p_bindings,
        repl::SetBook *p_sets,
        NetwPredictionEngine *p_prediction
    );
#if defined(DEBUG_ENABLED)
    void seat_profiler(
        wire::NetworkProfiler *p_profiler,
        uint32_t *p_outgoing
    );
#endif
#if defined(NETW_TESTS)
    void seat_idle(session::IdleWork *p_idle);
#endif
    void unseat();

    NetwSyncModel *get_sync_model() {
        return &sync_model;
    }
    SyncPipeline *get_sync_pipeline() {
        return &sync_pipeline;
    }
    spawn::Pipeline *get_spawn_pipeline() {
        return &spawn_pipeline;
    }
    spawn::SpawnerCompat *get_spawner_compat() {
        return &spawner_compat;
    }
    SyncCompat *get_sync_compat() {
        return &sync_compat;
    }
    bool get_is_applying_remote_frame() const;

    repl::PropertyBindingPtr derived_binding(
        godot::Node *p_node,
        int64_t p_record
    );
    godot::LocalVector<repl::PropertyBindingPtr> derived_group(int64_t p_route);
    void note_peer_ack(
        int64_t p_peer_id,
        int64_t p_acked_seq,
        uint32_t p_history
    );

    void broadcast_control(
        const godot::Ref<NetwEntity> &p_entity,
        const session::ControlApply &p_decision,
        int64_t p_issuer
    );
    void reply_control(
        const godot::Ref<NetwEntity> &p_entity,
        const session::ControlApply &p_decision,
        int64_t p_issuer
    );
    void request_control(
        const godot::Ref<NetwEntity> &p_entity,
        const session::ControlRequest &p_request
    );
    void watch_control(const godot::Ref<NetwEntity> &p_entity);
    void watch_claim_image(const godot::Ref<NetwEntity> &p_entity);
    void send_claim_images(int64_t p_tick);
    bool lanes_stream_to(
        const godot::Ref<NetwEntity> &p_entity,
        int64_t p_peer_id
    );

    void send_to(
        int64_t p_peer_id,
        int64_t p_route,
        int64_t p_channel,
        const godot::PackedByteArray &p_payload,
        bool p_reliable,
        int64_t p_comp,
        const godot::String &p_path
    );
    bool is_live_for(int64_t p_peer_id, const godot::Ref<NetwEntity> &p_entity);
    bool policy_admits(
        int64_t p_policy,
        int64_t p_sender,
        godot::Node *p_node,
        const godot::Ref<NetwEntity> &p_entity
    );
    godot::PackedInt32Array live_peers(const godot::Ref<NetwEntity> &p_entity);
    void flush_all_buffers();
    void flush_echoes();
    bool upkeep_rides() const;
    void expire_control(int64_t p_tick);
    void note_staged(int64_t p_peer_id, int64_t p_seq);

    godot::Error dispatch(
        int64_t p_route,
        int64_t p_comp,
        int64_t p_channel,
        const godot::PackedByteArray &p_payload,
        const godot::String &p_path,
        int64_t p_sender,
        bool p_reliable,
        int64_t p_seq,
        int64_t p_frame_bytes = -1
    );

    godot::Error receive_carrier(
        const godot::PackedByteArray &p_framed,
        int64_t p_sender,
        bool p_reliable,
        int64_t p_seq,
        int64_t p_base_tick
    );

    godot::Node *resolve_comp_node(
        const godot::Ref<NetwEntity> &p_entity,
        int64_t p_comp,
        const godot::String &p_path
    );

    void on_clock_tick(int64_t p_tick);
    void pump_tables(int64_t p_tick);
    void send_table_removals(int64_t p_tick);
    void replay_tables(int64_t p_peer_id);
    void on_frame_end();
    void on_poll();

    void clear_session();
    void clear_route(int64_t p_route);
    void release_spawn_builds();
    void clear_spawn_parks();
    void release_spawn_route(int64_t p_route);
    void act_spawn_rows(spawn::Ops &r_ops);
    void clear_peer(int64_t p_peer_id);
    void dispose();
    godot::Dictionary counters() const;

    void register_spawn_constructor(
        const godot::StringName &p_id,
        const godot::Callable &p_fn
    );
    godot::Ref<NetwEntity> adopt_in_place(godot::Node *p_root);
    godot::TypedArray<godot::Dictionary> spawn_state_of(godot::Node *p_root);
    bool owns_spawned_route(int64_t p_route);
};

} // namespace netw
