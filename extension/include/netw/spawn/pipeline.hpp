#pragma once

#include <cstdint>
#include <optional>

#include "godot/multiplayer_spawner.hpp"
#include "godot/node.hpp"
#include "godot/object.hpp"
#include "godot/packed_scene.hpp"
#include "godot/templates.hpp"
#include "godot/variant.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/participant.hpp"
#include "netw/call_args.hpp"
#include "netw/lifecycle/rule.hpp"
#include "netw/log.hpp"
#include "netw/repl/sync_compat.hpp"
#include "netw/spawn/book.hpp"
#include "netw/spawn/op.hpp"
#include "netw/spawn/park.hpp"
#include "netw/spawn/record.hpp"

namespace netw {
class Clock;
class NetwLivenessCore;
class NetwMultiplayer;
} // namespace netw

namespace netw::spawn {

struct SpawnFrame {
    struct StateEntry {
        bool by_path = false;
        godot::String path;
        godot::PackedByteArray token;
        godot::PackedByteArray values;
    };

    struct NativeEntry {
        godot::String path;
        godot::PackedByteArray value;
    };

    int64_t route = 0;
    int64_t epoch = 0;
    godot::Dictionary header;
    int64_t recipe = 0;
    bool parent_is_spawn_target = false;
    Anchor parent_anchor;
    bool moved_from_origin = false;
    Anchor origin_anchor;
    Anchor spawner_anchor;
    int64_t scene_index = 0;
    godot::PackedByteArray custom;
    godot::String fn_id;
    Anchor host_anchor;
    godot::String method;
    bool fn_waits = false;
    godot::PackedByteArray args;
    godot::LocalVector<StateEntry> state;
    godot::LocalVector<NativeEntry> native;
    godot::PackedByteArray consumed;
    godot::PackedByteArray derived;
};

bool decode_spawn_frame(
    const NetwMultiplayer *p_session,
    const godot::PackedByteArray &p_payload,
    SpawnFrame &r_frame
);

struct Built {
    enum Outcome : uint8_t {
        BUILT,
        DROPPED,
        WAITS,
    };

    SpawnFrame frame;
    godot::Node *node = nullptr;
    godot::Node *parent = nullptr;
    godot::Node *host = nullptr;
    godot::Node *origin = nullptr;
    godot::MultiplayerSpawner *spawner = nullptr;
    godot::Array args;
    godot::Variant data;
    bool strict = false;
    bool names_a_freed_node = false;
    bool adopted = false;
    Park::Wait wait = Park::WAIT_NONE;
    int64_t wait_route = 0;
    godot::String scene;
    bool from_build = false;
};

struct Ops {
    godot::LocalVector<NodeOp> rows;
    uint32_t acted = 0;
    godot::PackedByteArray payload;
    std::optional<Built> building;

    Built &build() {
        if (!building) {
            building.emplace();
        }
        return *building;
    }

    Ops() = default;
    Ops(const Ops &) = delete;
    Ops &operator=(const Ops &) = delete;

#ifdef NETW_TESTS
    ~Ops() {
        NETW_BUG_COND(acted < rows.size());
    }
#endif
};

struct Build {
    godot::String scene;
    godot::Variant pending;
    godot::ObjectID ready;
    godot::ObjectID host;
    godot::StringName method;
    int64_t serial = 0;
    int64_t started_at = 0;
};

class Pipeline {
private:
    NetwMultiplayer *session = nullptr;
    Clock *clock_engine = nullptr;
    NetwLivenessCore *liveness_core = nullptr;

    Book spawn_book;
    Park park;
    SyncCompat *sync_compat = nullptr;
    bool retrying_scene_parks = false;
    bool applying_action_gate = false;
    bool revealing_action_gates = false;

    int64_t channel_spawn = 0;
    int64_t channel_despawn = 0;
    int64_t channel_hide = 0;
    int64_t channel_reparent = 0;

    godot::HashMap<int64_t, int64_t> first_recipients;
    godot::HashMap<int64_t, Build> builds;
    int64_t build_serial = 0;
    godot::HashSet<uint64_t> refused_placed;
    godot::HashSet<int64_t> placed_before_host;
    struct Unadded {
        godot::ObjectID node;
        int64_t armed_at_ticks_run = 0;
    };
    godot::HashMap<int64_t, Unadded> unadded;

    int64_t drops_spawn_bad_sender = 0;
    int64_t drops_spawn_duplicate = 0;
    int64_t drops_spawn_unresolved = 0;
    int64_t drops_spawn_truncated = 0;
    int64_t drops_spawn_stale_life = 0;
    int64_t drops_reparent_stale = 0;
    int64_t drops_despawn_unknown = 0;
    int64_t drops_hide_unknown = 0;
    int64_t spawn_deferrals = 0;
    int64_t spawn_parked_cancelled = 0;
    int64_t spawn_park_expired = 0;
    int64_t spawn_park_refused = 0;
    int64_t spawn_nested_published = 0;
    int64_t lifecycle_ops_admitted = 0;
    int64_t lifecycle_ops_refused = 0;

#ifdef NETW_TESTS
    godot::TypedArray<godot::Dictionary> armed_spawn_state;
    bool spawn_state_is_armed = false;
    int spawn_state_asked = 0;

    struct HeldPlacement {
        Built built;
        int64_t release_tick = 0;
    };
    godot::HashMap<int64_t, int64_t> placement_delays;
    godot::LocalVector<HeldPlacement> held_placements;

    bool hold_placement(const Built &p_built);
#endif

    friend class netw::NetwMultiplayer;

    bool is_server_authority() const;
    bool has_peer() const;
    godot::PackedInt32Array session_peers() const;

    godot::Error stage_verdict(
        int64_t p_stage,
        godot::Error p_verdict,
        int64_t p_route
    );
    void sink_verdict(godot::Error p_verdict, int64_t p_route);

    void arm(const Record &p_record);
    void await_entry(int64_t p_route, godot::Node *p_node);
    void entered(int64_t p_route);
    void drop_armed(int64_t p_route);
    void note_requested(int64_t p_route, int64_t p_sender);
    void count_nested_published();
    bool take_placed_before_host(int64_t p_route);
    bool refuse_placed_once(uint64_t p_node);
    godot::Error declare_stage(
        const godot::RID &p_handle,
        const godot::Dictionary &p_facts
    );

    bool flush_armed_spawn(int64_t p_route, Ops &r_ops);
    void send_authored_spawn(int64_t p_route);
    bool armed_player_left(int64_t p_route);
    bool is_booked(int64_t p_route) const;
    void note_placed_before_host(int64_t p_route);
    godot::String adopt_place_of(const godot::PackedByteArray &p_payload);
    void settle_move(int64_t p_route, int64_t p_author = 0);
    void settle_death(int64_t p_route);
    void despawn_now(int64_t p_route, godot::Node *p_within);
    void settle_absence(int64_t p_route);
    bool holds_received_route(int64_t p_route) const;
    bool holds_spawned_route(int64_t p_route) const;
    void despawn_tracked_route(
        int64_t p_route,
        godot::Node *p_within = nullptr
    );
    void send_reparent(
        Record *p_record,
        godot::Node *p_node,
        int64_t p_author = 0
    );

    godot::PackedByteArray encode_fn_args(
        const godot::Array &p_schema,
        const godot::Array &p_args
    );
    static godot::LocalVector<call_args::Slot> read_fn_args(
        const godot::PackedByteArray &p_bytes,
        const godot::Array &p_schema
    );

    void fail_pending_spawns(const godot::String &p_detail);
    bool frame_read(bool p_healthy, int64_t p_route, int64_t p_sender);
    bool admits_frame(
        int64_t p_sender,
        int64_t p_channel,
        const godot::PackedByteArray &p_payload
    );
    godot::Error admission(
        int64_t p_sender,
        int64_t p_channel,
        const godot::PackedByteArray &p_payload
    );
    void decide_spawn(
        const godot::PackedByteArray &p_payload,
        int64_t p_sender,
        Ops &r_ops
    );
    Built::Outcome decide_build(
        const godot::PackedByteArray &p_payload,
        int64_t p_sender,
        Built &r_built
    );
    void settle_build(Built::Outcome p_outcome, int64_t p_sender, Ops &r_ops);
    void park_built(
        const godot::PackedByteArray &p_payload,
        int64_t p_sender,
        const Built &p_built
    );
    void admit_controller_spawn(
        const godot::PackedByteArray &p_payload,
        int64_t p_sender,
        Ops &r_ops
    );
    void refuse_controller_spawn(
        int64_t p_route,
        int64_t p_sender,
        godot::Error p_code,
        bool p_holds_route,
        const godot::String &p_reason = godot::String()
    );
    void apply_parked(
        int64_t p_route,
        const godot::PackedByteArray &p_payload,
        int64_t p_sender,
        Ops &r_ops
    );

    int64_t park_timeout_ticks() const;
    void warn_slow_build(
        const godot::StringName &p_method,
        int64_t p_started_at,
        uint64_t p_host
    ) const;
    void park_spawn(
        const godot::PackedByteArray &p_payload,
        int64_t p_dep_route,
        int64_t p_route,
        int64_t p_sender
    );
    bool cancel_parked(int64_t p_route, Ops &r_ops);
    void drop_scene_park_retry();

    Built::Outcome wait_for_scene(
        const godot::String &p_path,
        Built &r_built,
        Ops &r_ops
    );
    Built::Outcome drop_missing_scene(
        int64_t p_route,
        const godot::String &p_path,
        Ops &r_ops
    );
    int64_t build_waits(
        int64_t p_route,
        const godot::Variant &p_waiting,
        godot::Node *p_host,
        const godot::StringName &p_method,
        Ops &r_ops
    );
    Built::Outcome build_settled(int64_t p_serial, Built &r_built) const;
    godot::Node *take_ready(int64_t p_route);
    void drop_build(int64_t p_route, Ops &r_ops);

    int64_t gate_display_tick() const;
    void apply_action_gate(int64_t p_route, const godot::RID &p_entity);
    void on_action_reveal_tick(double p_delta, int64_t p_tick);
    void settle_action_reveal();
    void note_action_gate();
    void clear_action_gates();

public:
    double park_timeout_seconds = 5.0;

    Pipeline() = default;
    Pipeline(const Pipeline &) = delete;
    Pipeline &operator=(const Pipeline &) = delete;

    bool retries_scene_parks() const {
        return retrying_scene_parks;
    }
    bool applies_action_gate() const {
        return applying_action_gate;
    }
    bool reveals_action_gates() const {
        return revealing_action_gates;
    }
    bool awaits_unadded() const {
        return !unadded.is_empty();
    }
    void report_unadded(int64_t p_ticks_run);
    void set_park_timeout_seconds(double p_seconds) {
        park_timeout_seconds = p_seconds;
    }

    void seat(
        NetwMultiplayer *p_session,
        Clock *p_clock,
        NetwLivenessCore *p_liveness
    );
    void unseat();
    void set_sync_adapter(SyncCompat *p_sync);
    void set_channels(
        int64_t p_spawn,
        int64_t p_despawn,
        int64_t p_hide,
        int64_t p_reparent
    );

    Book *get_spawn_book() {
        return &spawn_book;
    }
    bool books_node(godot::Node *p_node) const;
    Park &get_park() {
        return park;
    }

    void park_spawn_for_scene(
        const godot::PackedByteArray &p_payload,
        int64_t p_route,
        int64_t p_sender
    );
    void park_spawn_for_adopt(
        const godot::PackedByteArray &p_payload,
        int64_t p_route,
        int64_t p_sender
    );
    int64_t park_now() const;
    void retry_parked(int64_t p_route, Ops &r_ops);
    void expire_parked(int64_t p_route, Ops &r_ops);
    void expire_spawner_route(int64_t p_route, int64_t p_now, Ops &r_ops);
    void retry_spawner_route(int64_t p_route, Ops &r_ops);
    void settle_spawner_retry();
    bool retry_adopt(int64_t p_route, int64_t p_now, Ops &r_ops);
    void settle_adopt_retry(int64_t p_route);
    void retry_build(int64_t p_route, Ops &r_ops);
    bool build_returned(
        const godot::Variant &p_value,
        int64_t p_route,
        int64_t p_serial,
        Ops &r_ops
    );
    void build_failed(
        const godot::String &p_detail,
        int64_t p_route,
        int64_t p_serial,
        Ops &r_ops
    );

    static bool roots_published_entity(godot::Node *p_node);
    const Record *spawned_record(int64_t p_route) const {
        return spawn_book.spawned_of(p_route);
    }

    bool owns_spawned_route(int64_t p_route) const;
    godot::TypedArray<godot::Dictionary> collect_spawn_state(
        godot::Node *p_root
    );

#ifdef NETW_TESTS
    void arm_spawn_state(const godot::Array &p_rows);
    void disarm_spawn_state();
    void delay_placement(int64_t p_route, int64_t p_ticks);
    bool take_held_placement(Ops &r_ops);
#endif

    godot::PackedByteArray encode_spawn_frame(
        int64_t p_route,
        godot::Node *p_node
    );
    godot::PackedByteArray encode_record_frame(
        Record *p_record,
        int64_t p_route,
        godot::Node *p_node
    );

    void receive_spawn(
        const godot::PackedByteArray &p_payload,
        int64_t p_sender,
        Ops &r_ops
    );
    void built(const NodeOp &p_op, Built::Outcome p_outcome, Ops &r_ops);
    void enroll_received(const Built &p_built);
    void kept_reparent(int64_t p_route, Ops &r_ops);
    void handle_despawn_frame(
        const godot::PackedByteArray &p_payload,
        int64_t p_sender,
        Ops &r_ops
    );
    void handle_hide_frame(
        const godot::PackedByteArray &p_payload,
        int64_t p_sender,
        Ops &r_ops
    );
    void missed(const NodeOp &p_op);
    void handle_reparent_frame(
        const godot::PackedByteArray &p_payload,
        int64_t p_sender,
        Ops &r_ops
    );
    godot::Error admit_controller_frame(
        lifecycle::Kind p_kind,
        int64_t p_sender,
        const godot::PackedByteArray &p_payload
    );
    void count_lifecycle_op(bool p_admitted);

    godot::LocalVector<godot::ObjectID> end_session_nodes();
    void begin_clear_session();
    void finish_clear_session();
    godot::LocalVector<int64_t> begin_clear_parks();
    void finish_clear_parks();
    godot::LocalVector<int64_t> build_routes() const;
    void release_build(int64_t p_route, Ops &r_ops);
    godot::PackedInt64Array promote_received();
    godot::PackedInt64Array begin_demotion();
    void discard_armed_spawn(int64_t p_route, Ops &r_ops);
    void demote_spawned();
    void release_received(int64_t p_route, Ops &r_ops);
    void clear_route(int64_t p_route, Ops &r_ops);

    godot::Dictionary counters() const;
};

} // namespace netw::spawn
