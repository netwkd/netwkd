#include "netw/api/interest.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/scene.hpp"

#include "netw/api/participant.hpp"

#include "godot/callable.hpp"
#include "godot/class_db.hpp"
#include "godot/node.hpp"
#include "godot/object.hpp"
#include "godot/os.hpp"
#include "godot/packed_scene.hpp"
#include "godot/physics_server.hpp"
#include "godot/rendering_server.hpp"
#include "godot/resource.hpp"
#include "godot/scene_tree.hpp"
#include "godot/script.hpp"
#include "godot/spatial_node.hpp"
#include "godot/time.hpp"
#include "godot/utility.hpp"
#include "godot/viewport.hpp"
#include "godot/world.hpp"
#include "netw/api/display.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/entity_record.hpp"
#include "netw/api/join_request.hpp"
#include "netw/api/prediction.hpp"
#include "netw/api/replication_core.hpp"
#include "netw/repl/sync_compat.hpp"
#include "netw/repl/sync_pipeline.hpp"
#include "netw/colors.hpp"
#include "netw/comp_table.hpp"
#include "netw/game_call.hpp"
#include "netw/fault_book.hpp"
#include "netw/log.hpp"
#include "netw/predict/decl.hpp"
#include "netw/prediction_core.hpp"
#include "netw/profile.hpp"
#include "netw/repl/set_model.hpp"
#include "netw/schema_model.hpp"
#include "netw/script/model.hpp"
#include "netw/stage.hpp"
#include "netw/sync_authoring.hpp"
#include "netw/synchronizers.hpp"
#include "netw/wire/control_record.hpp"
#include "netw/wire/control_scheduler.hpp"
#include "netw/wire/frame.hpp"
#include "netw/wire/registry.hpp"
#include "netw/wire/stream_book.hpp"

using namespace godot;
using namespace netw;

namespace netw {

using repl::PropertyBinding;
using repl::PropertyBindingPtr;
using repl::PropertyColumn;
using repl::PropertySet;

namespace {

const GameCall MIGRATION_STEP
    = {"The NetwSchema.migrate or NetwPersistenceConfig.migrate step",
       "func(values: Dictionary) -> Dictionary",
       1};

constexpr double SHUTDOWN_NOTIFY_DELAY = 0.5;

enum class OpenSeat : uint8_t {
    SEAT,
    PARK,
    REFUSE,
};

wire::StreamTenure anchor_tenure(
    const NetwMultiplayer::AnchorRevision &p_revision
) {
    wire::StreamTenure tenure;
    tenure.anchor = p_revision.revision;
    tenure.anchor_author = p_revision.author;
    return tenure;
}

OpenSeat open_seat(
    NetwSyncModel *p_model,
    const Ref<NetwEntity> &p_entity,
    int64_t p_sender,
    const wire::StreamLane &p_lane,
    uint64_t p_tenure,
    const wire::StreamTenure &p_asked,
    const wire::StreamTenure &p_installed,
    wire::StreamTenure &r_tenure
) {
    r_tenure = wire::StreamTenure();
    r_tenure.anchor = p_installed.anchor;
    r_tenure.anchor_author = p_installed.anchor_author;
    if (p_asked.anchor > p_installed.anchor) {
        return OpenSeat::PARK;
    }
    if (p_asked.anchor < p_installed.anchor
        || p_asked.anchor_author != p_installed.anchor_author) {
        return OpenSeat::REFUSE;
    }
    const repl::SetRow *row = p_model != nullptr
        ? p_model->row(p_lane.route, int64_t(p_lane.ordinal))
        : nullptr;
    if (row == nullptr || p_entity.is_null()
        || !repl::record_follows_tenure(row->record)) {
        return OpenSeat::SEAT;
    }
    const uint64_t current = p_entity->get_control_tenure();
    if (p_tenure > current) {
        return OpenSeat::PARK;
    }
    if (p_tenure < current) {
        return OpenSeat::REFUSE;
    }
    const PropertyBindingPtr binding
        = p_model->binding_of(p_lane.route, int64_t(p_lane.ordinal));
    Node *node = binding != nullptr ? binding->node() : nullptr;
    if (node != nullptr
        && !p_model->admits_sender(
            p_lane.route,
            int64_t(p_lane.ordinal),
            p_sender,
            int64_t(node->get_multiplayer_authority()),
            p_entity->steering()
        )) {
        return OpenSeat::REFUSE;
    }
    r_tenure.bound = true;
    r_tenure.tenure = current;
    return OpenSeat::SEAT;
}

void answer_open(
    ReplicationSend *p_send,
    int p_peer,
    const wire::StreamLane &p_lane,
    uint64_t p_request,
    uint64_t p_epoch,
    uint32_t p_schema,
    const wire::StreamTenure &p_tenure
) {
    uint64_t token = 0;
    const wire::OpenVerdict verdict = p_send->reader_book().open(
        p_peer,
        p_lane,
        p_request,
        p_epoch,
        p_schema,
        token,
        p_tenure
    );
    const bool live = verdict == wire::OpenVerdict::MINTED
        || verdict == wire::OpenVerdict::REPEATED;
    wire::ControlRecord answer;
    answer.tag = live ? wire::ControlTag::READY : wire::ControlTag::RESET;
    answer.request = p_request;
    answer.token = token;
    p_send->control_scheduler().queue(p_peer, answer);
}

void seat_or_park(
    ReplicationSend *p_send,
    NetwSyncModel *p_model,
    const Ref<NetwEntity> &p_entity,
    const wire::ParkedOpen &p_open,
    const wire::StreamTenure &p_installed
) {
    wire::StreamTenure asked;
    asked.anchor = p_open.anchor;
    asked.anchor_author = p_open.anchor_author;
    wire::StreamTenure tenure;
    switch (open_seat(
        p_model,
        p_entity,
        int64_t(p_open.peer),
        p_open.lane,
        p_open.tenure,
        asked,
        p_installed,
        tenure
    )) {
        case OpenSeat::SEAT:
            answer_open(
                p_send,
                p_open.peer,
                p_open.lane,
                p_open.request,
                p_open.epoch,
                p_open.schema,
                tenure
            );
            return;
        case OpenSeat::PARK:
            p_send->reader_book().park(p_open);
            return;
        case OpenSeat::REFUSE:
            p_send->reader_book().refuse_open();
            return;
    }
}

} // namespace

ReplicationCore *NetwMultiplayer::get_replication_plane() const {
    return replication_owner;
}

void NetwMultiplayer::replication_flush_all_buffers() {
    if (ReplicationCore *plane = get_replication_plane()) {
        plane->flush_all_buffers();
    }
}

Node *NetwMultiplayer::replication_resolve_comp_node(
    const Ref<NetwEntity> &p_entity,
    int64_t p_comp,
    const String &p_path
) {
    ReplicationCore *plane = get_replication_plane();
    return plane != nullptr ? plane->resolve_comp_node(p_entity, p_comp, p_path)
                            : nullptr;
}

Ref<NetwEntity> NetwMultiplayer::replication_adopt_in_place(Node *p_root) {
    ReplicationCore *plane = get_replication_plane();
    return plane != nullptr ? plane->adopt_in_place(p_root) : Ref<NetwEntity>();
}

void NetwMultiplayer::replication_handle_table_frame(
    const PackedByteArray &p_payload,
    int64_t p_sender
) {
    if (ReplicationCore *plane = get_replication_plane()) {
        plane->handle_table_frame(p_payload, p_sender);
    }
}

void NetwMultiplayer::replication_settle_reply(
    int64_t p_txn,
    int64_t p_sender,
    int64_t p_route,
    int64_t p_comp,
    const String &p_path
) {
    if (ReplicationCore *plane = get_replication_plane()) {
        plane->settle_reply(p_txn, p_sender, p_route, p_comp, p_path);
    }
}

Error NetwMultiplayer::replication_dispatch(
    int64_t p_route,
    int64_t p_comp,
    int64_t p_channel,
    const PackedByteArray &p_payload,
    const String &p_path,
    int64_t p_sender,
    bool p_reliable,
    int64_t p_seq
) {
    ReplicationCore *plane = get_replication_plane();
    return plane != nullptr ? plane->dispatch(
                                  p_route,
                                  p_comp,
                                  p_channel,
                                  p_payload,
                                  p_path,
                                  p_sender,
                                  p_reliable,
                                  p_seq
                              )
                            : ERR_UNCONFIGURED;
}

void NetwMultiplayer::replication_clear_route(int64_t p_route) {
    if (ReplicationCore *plane = get_replication_plane()) {
        plane->clear_route(p_route);
    }
}

void NetwMultiplayer::replication_replay_tables(int64_t p_peer_id) {
    if (ReplicationCore *plane = get_replication_plane()) {
        plane->replay_tables(p_peer_id);
    }
}

Error NetwMultiplayer::receive_carrier(
    const PackedByteArray &p_framed,
    int64_t p_sender,
    bool p_reliable,
    int64_t p_seq,
    int64_t p_base_tick
) {
    NETW_STAGE(RECEIVE, "Receive carrier", colors::WIRE);
    NETW_ZONE_TEXT_F("peer %lld", static_cast<long long>(p_sender));
    ReplicationCore *plane = (get_replication_plane());
    if (plane == nullptr) {
        NETW_TRACE(
            sys::WIRE,
            "Dropped %d bytes from peer %d, because replication is not "
            "running.",
            int(p_framed.size()),
            int(p_sender)
        );
        return ERR_UNCONFIGURED;
    }
    return plane
        ->receive_carrier(p_framed, p_sender, p_reliable, p_seq, p_base_tick);
}

SyncPipeline *NetwMultiplayer::sync_pipeline() const {
    ReplicationCore *plane = get_replication_plane();
    return plane != nullptr ? plane->get_sync_pipeline() : nullptr;
}

void NetwMultiplayer::sync_register_derived(Node *p_node) {
    if (SyncPipeline *pipeline = sync_pipeline()) {
        pipeline->register_derived(p_node);
    }
}

void NetwMultiplayer::sync_unregister_derived(Node *p_node) {
    if (SyncPipeline *pipeline = sync_pipeline()) {
        pipeline->unregister_derived(p_node);
    }
}

void NetwMultiplayer::sync_pipeline_on_entity_live(
    int64_t p_route,
    const Ref<NetwEntity> &p_entity
) {
    if (SyncPipeline *pipeline = sync_pipeline()) {
        pipeline->on_entity_live(p_route, p_entity);
    }
}

void NetwMultiplayer::sync_pipeline_bind_declaration(int64_t p_binding) {
    SyncPipeline *pipeline = sync_pipeline();
    PropertyBinding *binding = binding_book.live(uint64_t(p_binding));
    if (pipeline != nullptr && binding != nullptr) {
        pipeline->bind_declaration(binding);
    }
}

void NetwMultiplayer::sync_note_node_removed(Node *p_node) {
    if (p_node != nullptr) {
        store().residents.note_removed(uint64_t(p_node->get_instance_id()));
    }
    if (SyncPipeline *pipeline = sync_pipeline()) {
        pipeline->note_node_removed(p_node);
    }
}

void NetwMultiplayer::sync_pipeline_recapture_entity(
    const Ref<NetwEntity> &p_entity
) {
    if (SyncPipeline *pipeline = sync_pipeline()) {
        pipeline->recapture_entity(p_entity);
    }
}

void NetwMultiplayer::sync_pipeline_report_missing_prediction_component(
    Node *p_node,
    int64_t p_config_hash
) {
    if (SyncPipeline *pipeline = sync_pipeline()) {
        pipeline->report_missing_prediction_component(p_node, p_config_hash);
    }
}

Variant NetwMultiplayer::sync_pipeline_encode_prop_val(
    const Ref<NetwEntity> &p_entity,
    Node *p_node,
    const StringName &p_property
) {
    SyncPipeline *pipeline = sync_pipeline();
    return pipeline != nullptr
        ? pipeline->encode_prop_val(p_entity, p_node, p_property)
        : Variant();
}

PackedByteArray NetwMultiplayer::sync_pipeline_encode_derived_descriptors(
    int64_t p_route
) {
    SyncPipeline *pipeline = sync_pipeline();
    return pipeline != nullptr ? pipeline->encode_derived_descriptors(p_route)
                               : PackedByteArray();
}

void NetwMultiplayer::sync_pipeline_note_derived_schema(
    int64_t p_route,
    const Dictionary &p_descriptors
) {
    if (SyncPipeline *pipeline = sync_pipeline()) {
        pipeline->note_derived_schema(p_route, p_descriptors);
    }
}

void NetwMultiplayer::sync_send_property(
    const RID &p_entity,
    Node *p_node,
    const StringName &p_property
) {
    if (p_node == nullptr) {
        return;
    }
    SyncPipeline *pipeline = sync_pipeline();
    const int64_t route = liveness_core->route_of(p_entity);
    if (route <= 0) {
        if (pipeline != nullptr) {
            pipeline->send_property(p_node, p_property);
        }
        return;
    }
    if (!gd::has_property(p_node, p_property)) {
        fault::raise(
            this,
            NETW_FAULT_UNKNOWN_NAME,
            {"Sending a property",
             String(p_node->get_name()),
             String(p_property)},
            gd::instance_id(p_node)
        );
        sink_verdict(ERR_INVALID_DATA, route);
        return;
    }
    if (pipeline == nullptr) {
        fault::raise(
            this,
            NETW_FAULT_NOT_RUNNING,
            {vformat("Sending property '%s'", String(p_property)),
             String(),
             "replication"},
            gd::instance_id(p_node)
        );
        sink_verdict(ERR_UNCONFIGURED, route);
        return;
    }
    pipeline->send_property(p_node, p_property);
}

void NetwMultiplayer::sync_send_signal(
    const RID &p_entity,
    Node *p_node,
    const StringName &p_signal,
    const Array &p_args
) {
    if (p_node == nullptr) {
        return;
    }
    SyncPipeline *pipeline = sync_pipeline();
    const int64_t route = liveness_core->route_of(p_entity);
    if (route <= 0) {
        if (pipeline != nullptr) {
            pipeline->send_signal(p_node, p_signal, p_args);
        }
        return;
    }
    if (!p_node->has_signal(p_signal)) {
        fault::raise(
            this,
            NETW_FAULT_UNKNOWN_NAME,
            {"Sending a signal", String(p_node->get_name()), String(p_signal)},
            gd::instance_id(p_node)
        );
        sink_verdict(ERR_INVALID_DATA, route);
        return;
    }
    if (pipeline == nullptr) {
        fault::raise(
            this,
            NETW_FAULT_NOT_RUNNING,
            {vformat("Sending signal '%s'", String(p_signal)),
             String(),
             "replication"},
            gd::instance_id(p_node)
        );
        sink_verdict(ERR_UNCONFIGURED, route);
        return;
    }
    pipeline->send_signal(p_node, p_signal, p_args);
}

NodePath NetwMultiplayer::property_path(
    Object *p_source,
    const StringName &p_property,
    Object *p_base
) {
    Node *base = Object::cast_to<Node>(p_base);
    if (base == nullptr) {
        return NodePath();
    }
    const NodePath relative = relative_path(base, p_source);
    if (relative.is_empty()) {
        return NodePath();
    }
    return NodePath(String(relative) + ":" + String(p_property));
}

Error NetwMultiplayer::sync_admit_frame(
    int64_t p_route,
    int64_t p_channel,
    const PackedByteArray &p_payload
) const {
    NETW_ZONE_LEAF("Row admit frame", colors::WIRE, profile::SUBSYSTEM_WIRE);
    const Error verdict = entity_frame_verdict(p_route);
    if (verdict != OK) {
        return verdict;
    }
    const bool carries_sync = p_channel == wire::builtin_channel("SYNC")
        || p_channel == wire::builtin_channel("SYNC_ROW")
        || p_channel == wire::builtin_channel("SYNC_ROW_DELTA")
        || p_channel == wire::builtin_channel("SYNC_ROW_WINDOW")
        || p_channel == wire::builtin_channel("SYNC_DELTA");
    if (!carries_sync || p_payload.is_empty()) {
        return ERR_INVALID_DATA;
    }
    return OK;
}

void NetwMultiplayer::table_announce_intake() {
    store::Arrays &arrays = store_world.arrays;
    const TypedArray<RID> touched = arrays.touched_tables();
    for (int index = 0; index < touched.size(); index++) {
        table_announce(touched[index]);
    }
    arrays.begin_intake();
}

void NetwMultiplayer::table_announce(const RID &p_table) {
    if (!views.table_handles.has(p_table)) {
        return;
    }
    Announcement row;
    row.kind = AnnouncementKind::TABLE_RECEIVED;
    row.table = p_table;
    row.to = store_world.arrays.tick_of(p_table);
    announce(row);
}

bool NetwMultiplayer::table_publishes() const {
    return is_host();
}

Error NetwMultiplayer::table_admit_frame(
    int64_t p_sender,
    int64_t p_channel,
    const PackedByteArray &p_payload
) {
    NETW_ZONE_NC("Table admit frame", colors::TABLE);
    if (p_channel != gate_channels.table) {
        return ERR_INVALID_DATA;
    }
    if (p_sender != SERVER_PEER) {
        store_world.arrays.count_bad_sender();
        return ERR_UNAUTHORIZED;
    }
    if (p_payload.is_empty()) {
        return ERR_INVALID_DATA;
    }
    return store_world.arrays.admit_header(p_payload);
}

Node *NetwMultiplayer::repl_comp_node(
    int64_t p_route,
    int64_t p_comp,
    const String &p_path,
    int64_t p_sender
) {
    const int64_t state = liveness_route_state(p_route);
    const bool lingering = state == int64_t(NetwLivenessCore::STATE_LINGERING);
    if (state == int64_t(NetwLivenessCore::STATE_DEAD) || lingering) {
        repl_drops.not_live++;
        if (lingering) {
            repl_drops.lingering_route++;
        } else {
            repl_drops.dead_route++;
        }
        NETW_TRACE(
            sys::TRANSPORT,
            "Dropped a frame for route %d, because the route is %s.",
            int(p_route),
            lingering ? "closing" : "gone"
        );
        return nullptr;
    }
    const Ref<NetwEntity> entity = wrapper_for_route(p_route);
    Node *root = entity.is_valid() ? entity->get_owner() : nullptr;
    if (root == nullptr) {
        repl_drops.no_node++;
        return nullptr;
    }
    NetwCompTable &table = entity->comp_table();
    Node *held = root;
    switch (table.classify(p_comp, p_path)) {
        case NetwCompTable::ADDRESS_HOSTILE:
            if (p_sender != 0) {
                fault::raise(
                    this,
                    NETW_FAULT_MALFORMED_DATAGRAM,
                    {String(), String(), String(), p_sender}
                );
            }
            repl_drops.traversal++;
            return nullptr;
        case NetwCompTable::ADDRESS_UNMAPPED:
            repl_drops.no_node++;
            return nullptr;
        case NetwCompTable::ADDRESS_RELATIVE:
            held = root->get_node_or_null(NodePath(p_path));
            if (held == nullptr) {
                repl_drops.comp_unresolved++;
                NETW_TRACE(
                    sys::TRANSPORT,
                    "Component '%s' on '%s' is not found yet.",
                    String(p_path).utf8().get_data(),
                    String(root->get_name()).utf8().get_data()
                );
                return nullptr;
            }
            if (held != root && !root->is_ancestor_of(held)) {
                if (p_sender != 0) {
                    fault::raise(
                        this,
                        NETW_FAULT_MALFORMED_DATAGRAM,
                        {String(), String(), String(), p_sender}
                    );
                }
                repl_drops.traversal++;
                return nullptr;
            }
            break;
        case NetwCompTable::ADDRESS_MAPPED:
            held = root->get_node_or_null(NodePath(table.path_for_id(p_comp)));
            break;
        default:
            break;
    }
    if (held == nullptr) {
        repl_drops.no_node++;
        return nullptr;
    }
    return held;
}

void NetwMultiplayer::repl_note_unknown_route() {
    repl_drops.unknown_route++;
}

void NetwMultiplayer::repl_note_gate_verdict(int64_t p_verdict) {
    if (p_verdict == int64_t(ERR_DOES_NOT_EXIST)) {
        repl_drops.unknown_route++;
    } else if (p_verdict == int64_t(ERR_SKIP)) {
        repl_drops.not_live++;
    } else if (p_verdict == int64_t(ERR_UNAVAILABLE)) {
        repl_drops.no_node++;
    }
}

Dictionary NetwMultiplayer::repl_drop_stats() const {
    Dictionary out;
    out[StringName("drops_unknown_route")] = repl_drops.unknown_route;
    out[StringName("drops_not_live")] = repl_drops.not_live;
    out[StringName("drops_lingering_route")] = repl_drops.lingering_route;
    out[StringName("drops_dead_route")] = repl_drops.dead_route;
    out[StringName("drops_no_node")] = repl_drops.no_node;
    out[StringName("drops_traversal")] = repl_drops.traversal;
    out[StringName("drops_comp_unresolved")] = repl_drops.comp_unresolved;
    return out;
}

void NetwMultiplayer::sync_pump_rows(
    NetwSyncModel *p_model,
    ReplicationSend *p_send,
    int64_t p_tick,
    const Callable &p_bind,
    const Callable &p_tap
) {
    NETW_STAGE(PULL, "Pull pass", colors::WIRE);
    if (p_model == nullptr) {
        return;
    }
#if defined(NETW_TESTS)
    const int64_t allocated = store::allocations_seen();
#endif
    if (store_model != p_model || store_model_revision != p_model->revision()) {
        store_model = p_model;
        store_model_revision = p_model->revision();
        store_world.unsource_all();
    }
    store_tap = p_tap;
    store_refresh();
    for (uint32_t at = 0; at < store_world.table_count(); ++at) {
        store_world.table(at).stamp_tick = p_tick;
        store_sources(p_model, p_send, at, p_bind, p_tick);
        store_world.pull(at, p_tick);
    }
    for (uint32_t at = 0; at < store_world.woken.size(); ++at) {
        sim_note_push(gd::object_of(ObjectID(store_world.woken[at])));
    }
    store_world.woken.clear();
    for (uint32_t at = 0; at < store_world.reprobed.size(); ++at) {
        sim_note_push(gd::object_of(ObjectID(store_world.reprobed[at])));
    }
    store_world.reprobed.clear();
#if defined(NETW_TESTS)
    idle_work.pump_allocations += store::allocations_seen() - allocated;
#endif
}

void NetwMultiplayer::peer_set_bandwidth(
    int64_t p_peer,
    int64_t p_kbps
) {
    if (p_kbps < 0) {
        fault::raise(
            this,
            NETW_FAULT_BAD_VALUE,
            {"{NetwSession.set_peer_bandwidth}",
             vformat("%d kbit/s", p_kbps),
             "a rate in kbit/s, or 0 for the session's rate",
             p_peer}
        );
        return;
    }
    ReplicationSend *send = row_send_of(get_replication_plane());
    if (send != nullptr) {
        send->set_peer_rate(p_peer, double(p_kbps) * KBPS_BYTES);
    }
}

void NetwMultiplayer::sync_note_backlog(ReplicationSend *p_send) {
    static const StringName webrtc("WebRTCMultiplayerPeer");
    static const StringName get_peer("get_peer");
    static const StringName buffered_amount("get_buffered_amount");
    static const StringName channels_key("channels");
    const Ref<MultiplayerPeer> transport = inner.is_valid()
        ? inner->get_multiplayer_peer()
        : Ref<MultiplayerPeer>();
    if (transport.is_null() || !transport->is_class(webrtc)) {
        return;
    }
    const int64_t now = session_elapsed_ms();
    for (uint32_t at = 0; at < store_roster.size(); ++at) {
        const Dictionary described
            = transport->call(get_peer, int64_t(store_roster[at]));
        const Array channels = described.get(channels_key, Array());
        int64_t buffered = 0;
        for (int64_t which = 0; which < channels.size(); ++which) {
            Object *channel = channels[which];
            if (channel != nullptr) {
                buffered += int64_t(channel->call(buffered_amount));
            }
        }
        p_send->note_backlog(store_roster[at], buffered, now);
    }
}

void NetwMultiplayer::sync_flush_rows(
    ReplicationSend *p_send,
    int64_t p_row_channel,
    int64_t p_window_channel,
    int64_t p_delta_channel
) {
    NETW_STAGE(SEND, "Send pass", colors::WIRE);
    if (p_send == nullptr) {
        return;
    }
    const int64_t rate = int64_t(clock_engine().send_rate());
    if (rate > 0) {
        p_send->set_tickrate(rate);
    }
    p_send->set_rate(double(session_settings.bandwidth_kbps) * KBPS_BYTES);
    p_send->publish_tables(table_publishes());
    sync_note_backlog(p_send);
#if defined(NETW_TESTS)
    const int64_t allocated = store::allocations_seen();
#endif
    const repl::SessionResult &result = p_send->run(
        datagram_budget() * 8,
        datagram_base_tick(),
        session_elapsed_ms()
    );
#if defined(NETW_TESTS)
    idle_work.send_allocations += store::allocations_seen() - allocated;
    idle_work.sends += 1;
    idle_work.sent_rows += int64_t(result.sends.size());
#endif
    event_record(
        EventPlane::GATHER,
        RID(),
        0,
        0,
        OK,
        int64_t(result.sends.size() + result.deferred),
        int64_t(result.sends.size())
    );
#if defined(NETW_TESTS)
    store::AllocationTally framing(idle_work.frame_allocations);
#endif
    const bool grain = attribution_is_armed();
    for (uint32_t at = 0; at < result.sends.size(); at++) {
        const repl::RowSend &send = result.sends[at];
        if (grain && p_send->store() != nullptr) {
            const store::Table &table = p_send->store()->table(send.table);
            sync_note_columns(table.schema, table.plan, send.mask);
        }
        event_record(
            EventPlane::SYNC_ENCODE,
            RID(),
            send.route,
            int64_t(send.peer),
            OK,
            send.bytes.size()
        );
        CarrierRow descriptor;
        const bool owns_baseline = p_send->describe(send, descriptor);
        send_row_to(
            int64_t(send.peer),
            send.route,
            send.reliable ? p_delta_channel
                          : (send.windowed ? p_window_channel : p_row_channel),
            send.bytes,
            send.reliable,
            int64_t(send.comp),
            owns_baseline ? &descriptor : nullptr
        );
        if (send.reliable) {
            sync_flush.retained++;
            continue;
        }
        if (send.windowed) {
            sync_flush.windows++;
            sync_flush.window_samples += int64_t(send.sample_count);
            continue;
        }
        sync_flush.rows++;
        if (send.whole) {
            sync_flush.whole_rows++;
        }
    }
    sync_flush.staged_out += int64_t(result.staged_out);
    sync_flush.ungathered += int64_t(result.ungathered);
    sync_flush.deferred += int64_t(result.deferred);
    if (!table_publishes()) {
        return;
    }
    for (uint32_t at = 0; at < result.tables.size(); ++at) {
        const repl::TableFrame &frame = result.tables[at];
        send_frame(
            int64_t(frame.peer),
            0,
            gate_channels.table,
            frame.bytes,
            frame.reliable,
            0,
            String(),
            nullptr,
            nullptr,
            true
        );
    }
}

void NetwMultiplayer::row_control_receive(
    const PackedByteArray &p_payload,
    int64_t p_sender
) {
    NETW_ZONE_NC("Row control receive", colors::WIRE);
    ReplicationCore *plane = get_replication_plane();
    SyncPipeline *pipeline
        = plane != nullptr ? plane->get_sync_pipeline() : nullptr;
    ReplicationSend *send
        = pipeline != nullptr ? pipeline->row_send() : nullptr;
    if (send == nullptr) {
        return;
    }
    const int64_t channel = wire::builtin_channel(StringName("ROW_CONTROL"));
    const int peer = int(p_sender);
    LocalVector<wire::ControlRecord> records;
    if (!send->control_scheduler().read(peer, p_payload, records)) {
        attribution_note_refusal(p_sender, channel, wire::Refusal::MALFORMED);
        return;
    }
    for (uint32_t at = 0; at < records.size(); ++at) {
        row_control_apply(send, records[at], peer);
    }
}

void NetwMultiplayer::row_control_apply(
    ReplicationSend *p_send,
    const wire::ControlRecord &p_record,
    int p_peer
) {
    ReplicationCore *plane = get_replication_plane();
    wire::StreamLane lane;
    lane.route = p_record.route;
    lane.ordinal = p_record.ordinal;
    lane.family = p_record.family;

    switch (p_record.tag) {
        case wire::ControlTag::OPEN: {
            if (!p_record.resolved) {
                wire::ControlRecord answer;
                answer.tag = wire::ControlTag::RESET;
                answer.request = p_record.request;
                p_send->control_scheduler().queue(p_peer, answer);
                return;
            }
            const bool unspawned
                = liveness_route_state(p_record.route) == ENTITY_STATE_UNKNOWN;
            if (!unspawned
                && uint64_t(liveness_route_epoch(p_record.route))
                    != p_record.epoch) {
                return;
            }
            wire::ParkedOpen asked;
            asked.peer = p_peer;
            asked.lane = lane;
            asked.request = p_record.request;
            asked.epoch = p_record.epoch;
            asked.schema = p_record.schema;
            asked.tenure = p_record.tenure;
            asked.anchor = p_record.anchor;
            asked.anchor_author = p_record.anchor_author;
            asked.parked_at_ms = session_elapsed_ms();
            if (unspawned) {
                p_send->reader_book().park(asked);
                return;
            }
            seat_or_park(
                p_send,
                plane->get_sync_model(),
                wrapper_for_route(p_record.route),
                asked,
                anchor_tenure(anchor_installed(p_record.route))
            );
            return;
        }
        case wire::ControlTag::READY: {
            p_send->stream_ready(p_peer, p_record.request, p_record.token);
            return;
        }
        case wire::ControlTag::ACCEPT: {
            for (uint32_t at = 0; at < p_record.receipts.size(); ++at) {
                p_send->stream_receipt(
                    p_peer,
                    p_record.receipts[at].token,
                    p_record.receipts[at].revision
                );
            }
            return;
        }
        case wire::ControlTag::RESET: {
            if (p_record.token == 0) {
                p_send->control_scheduler().forget_sent_basis(p_peer);
            }
            p_send->stream_reset(p_peer, p_record.request, p_record.token);
            return;
        }
        case wire::ControlTag::CLOSE: {
            p_send->reader_book().close(p_peer, p_record.token);
            return;
        }
    }
}

void NetwMultiplayer::row_control_flush(
    ReplicationSend *p_send,
    int64_t p_channel
) {
    if (p_send == nullptr) {
        return;
    }
    NETW_ZONE_NC("Row control flush", colors::WIRE);
    wire::ControlScheduler &scheduler = p_send->control_scheduler();
    const int64_t now = session_elapsed_ms();
    const LocalVector<wire::ParkedOpen> expired = p_send->expire_parks(now);
    for (uint32_t at = 0; at < expired.size(); ++at) {
        wire::ControlRecord answer;
        answer.tag = wire::ControlTag::RESET;
        answer.request = expired[at].request;
        scheduler.queue(expired[at].peer, answer);
    }
    const LocalVector<int> ready = scheduler.ready_peers(now);
    for (uint32_t at = 0; at < ready.size(); ++at) {
        PackedByteArray frame;
        PackedByteArray receipts;
        scheduler.flush(
            ready[at],
            now,
            p_send->control_budget_bytes(
                ready[at],
                wire::CONTROL_TICK_RESERVATION_BYTES
            ),
            &frame,
            &receipts
        );
        if (!frame.is_empty()) {
            send_to(
                int64_t(ready[at]),
                0,
                p_channel,
                frame,
                true,
                0,
                String()
            );
        }
        if (!receipts.is_empty()) {
            send_to(
                int64_t(ready[at]),
                0,
                p_channel,
                receipts,
                false,
                0,
                String(),
                wire::Delivery::RIDE
            );
        }
    }
}

ReplicationSend *NetwMultiplayer::row_send_of(ReplicationCore *p_plane) {
    SyncPipeline *pipeline
        = p_plane != nullptr ? p_plane->get_sync_pipeline() : nullptr;
    return pipeline != nullptr ? pipeline->row_send() : nullptr;
}

void NetwMultiplayer::row_streams_follow_tenure(
    int64_t p_route,
    uint64_t p_tenure
) {
    ReplicationCore *plane = get_replication_plane();
    ReplicationSend *send = row_send_of(plane);
    if (send == nullptr || p_route <= 0) {
        return;
    }
    send->park_tenures(p_route, p_tenure);
    row_streams_follow_readers(plane, send, p_route, p_tenure);
    row_streams_reseat_parks(plane, send, p_route);
}

void NetwMultiplayer::row_streams_follow_readers(
    ReplicationCore *p_plane,
    ReplicationSend *p_send,
    int64_t p_route,
    uint64_t p_tenure
) {
    wire::StreamReaderBook &book = p_send->reader_book();
    const LocalVector<wire::BoundLane> moved
        = book.lanes_bound_before(p_route, p_tenure);
    if (moved.is_empty()) {
        return;
    }
    const Ref<NetwEntity> entity = wrapper_for_route(p_route);
    const wire::StreamTenure installed
        = anchor_tenure(anchor_installed(p_route));
    for (uint32_t at = 0; at < moved.size(); ++at) {
        const wire::BoundLane &lane = moved[at];
        wire::StreamTenure asked;
        asked.anchor = lane.tenure.anchor;
        asked.anchor_author = lane.tenure.anchor_author;
        wire::StreamTenure seated;
        const OpenSeat seat = open_seat(
            p_plane->get_sync_model(),
            entity,
            int64_t(lane.peer),
            lane.lane,
            p_tenure,
            asked,
            installed,
            seated
        );
        book.follow_tenure(
            lane.peer,
            lane.lane,
            p_tenure,
            seat == OpenSeat::SEAT
        );
    }
}

void NetwMultiplayer::row_streams_follow_anchor(
    int64_t p_route,
    uint64_t p_anchor,
    uint64_t p_author
) {
    store_resource_route(p_route);
    ReplicationCore *plane = get_replication_plane();
    ReplicationSend *send = row_send_of(plane);
    if (send == nullptr || p_route <= 0) {
        return;
    }
    send->close_anchors(p_route, p_anchor, p_author);
    send->reader_book().close_anchors_before(p_route, p_anchor, p_author);
    row_streams_reseat_parks(plane, send, p_route);
}

void NetwMultiplayer::row_streams_reopen(int64_t p_route) {
    store_resource_route(p_route);
    ReplicationSend *send = row_send_of(get_replication_plane());
    if (send == nullptr || p_route <= 0) {
        return;
    }
    send->close_anchors(p_route, 0, 0);
}

void NetwMultiplayer::row_streams_reseat_parks(
    ReplicationCore *p_plane,
    ReplicationSend *p_send,
    int64_t p_route
) {
    const LocalVector<wire::ParkedOpen> parked
        = p_send->reader_book().take_parks(p_route);
    const Ref<NetwEntity> entity = wrapper_for_route(p_route);
    const wire::StreamTenure installed
        = anchor_tenure(anchor_installed(p_route));
    for (uint32_t at = 0; at < parked.size(); ++at) {
        seat_or_park(
            p_send,
            p_plane->get_sync_model(),
            entity,
            parked[at],
            installed
        );
    }
}

void NetwMultiplayer::sync_note_columns(
    const table::SchemaRecord &p_schema,
    const wire::WirePlan &p_plan,
    uint64_t p_mask
) {
    const wire::WirePlan &plan = p_plan;
    const int64_t schema = int64_t(p_schema.shape_hash);
    for (uint32_t index = 0; index < plan.column_count(); ++index) {
        if ((p_mask & (uint64_t(1) << index)) == 0) {
            continue;
        }
        const wire::ColumnPlan &slot = plan.column(index);
        attribution_note_column(
            schema,
            int64_t(index),
            int64_t(slot.stride) * int64_t(slot.width)
        );
    }
}

Dictionary NetwMultiplayer::peer_get_link_stats(int64_t p_peer) const {
    Dictionary out;
    const ClockEngine &clock = clock_engine();
    const double measured_ms = link_round_trip_ms(p_peer);
    out[StringName("rtt")]
        = measured_ms > 0.0 ? measured_ms / 1000.0 : clock.rtt_avg();
    out[StringName("jitter")] = measured_ms > 0.0
        ? link_jitter_ms(p_peer) / 1000.0
        : clock.rtt_jitter();
    out[StringName("reorders")] = seq_book.reorder_count(p_peer);
    out[StringName("duplicates")] = seq_book.duplicate_count(p_peer);
    out[StringName("loss")] = 0.0;
    out[StringName("mode")] = String("good");
    out[StringName("budget_bits")] = datagram_budget() * 8;
    out[StringName("budget_bytes")] = datagram_budget();
    out[StringName("sent_bytes")] = int64_t(0);
    out[StringName("debt_bytes")] = int64_t(0);
    out[StringName("deferred_rows")] = int64_t(0);

    ReplicationCore *plane = get_replication_plane();
    SyncPipeline *pipeline
        = plane != nullptr ? plane->get_sync_pipeline() : nullptr;
    ReplicationSend *send
        = pipeline != nullptr ? pipeline->row_send() : nullptr;
    if (send == nullptr) {
        return out;
    }
    out[StringName("loss")] = double(send->link_loss(p_peer));
    out[StringName("mode")] = String(
        send->link_mode(p_peer) == repl::LinkGovernor::BAD ? "bad" : "good"
    );
    out[StringName("budget_bits")]
        = send->link_budget_bits(p_peer, datagram_budget() * 8);
    out[StringName("budget_bytes")]
        = int64_t(send->fill_of(p_peer, datagram_budget() * 8));
    const repl::PeerAccount *account = send->account(p_peer);
    out[StringName("sent_bytes")]
        = account != nullptr ? account->sent_bytes : int64_t(0);
    out[StringName("debt_bytes")] = account != nullptr && account->balance < 0.0
        ? int64_t(-account->balance)
        : int64_t(0);
    out[StringName("deferred_rows")]
        = account != nullptr ? int64_t(account->deferred) : int64_t(0);
    return out;
}

Dictionary NetwMultiplayer::sync_flush_stats() const {
    Dictionary out;
    out[StringName("row_frames_out")] = sync_flush.rows;
    out[StringName("row_frames_full")] = sync_flush.whole_rows;
    out[StringName("retained_frames_out")] = sync_flush.retained;
    out[StringName("window_frames_out")] = sync_flush.windows;
    out[StringName("window_samples_out")] = sync_flush.window_samples;
    out[StringName("row_frames_stage_refused")] = sync_flush.staged_out;
    out[StringName("row_frames_ungathered")] = sync_flush.ungathered;
    out[StringName("row_frames_deferred")] = sync_flush.deferred;
    out[StringName("pump_skips_invalid_node")] = sync_flush.skips_invalid_node;
    out[StringName("pump_skips_no_entity")] = sync_flush.skips_no_entity;
    out[StringName("pump_skips_no_route")] = sync_flush.skips_no_route;
    return out;
}

Error NetwMultiplayer::display_lane(
    const RID &p_entity,
    const StringName &p_track,
    const Variant &p_value
) {
    const Error verdict = display_write_track(p_entity, p_track, p_value);
    event_record(
        EventPlane::DISPLAY_WRITE,
        p_entity,
        liveness_core->route_of(p_entity),
        0,
        verdict
    );
    return verdict;
}

void NetwMultiplayer::display_resolve_role(display::Runtime *p_runtime) {
    if (p_runtime != nullptr) {
#if defined(NETW_TESTS)
        idle_work.role_resolves += 1;
#endif
        {
            NETW_STAGE(DISPLAY, "Display settle body", colors::INTERP);
            sim_settle_body(p_runtime->entity());
        }
        display::resolve_role(p_runtime, display_hooks);
        display_book->wake(p_runtime);
    }
}

bool NetwMultiplayer::display_wants_runtime(Node *p_owner) const {
    return display::wants_runtime(p_owner, display_hooks);
}

void NetwMultiplayer::display_rebuild_runtime(display::Runtime *p_runtime) {
    if (p_runtime != nullptr) {
        NETW_ZONE_NC("Display seat bodies", colors::INTERP);
        display_seat_bodies(p_runtime);
    }
    display::rebuild_runtime(p_runtime, display_hooks);
    display_follow_tables(p_runtime);
    display_book->wake(p_runtime);
}

void NetwMultiplayer::display_seat_bodies(display::Runtime *p_runtime) {
    const sim::Row *row = sim_rows.row_of(p_runtime->entity_rid());
    LocalVector<ObjectID> bodies;
    sim::collect_bodies(
        p_runtime->owner(),
        row != nullptr ? row->declaration.bodies : Vector<NodePath>(),
        bodies
    );
    p_runtime->set_bodies(bodies);
    p_runtime->set_visual_offset(
        row != nullptr ? row->bodies.visual_transform : Variant()
    );
}

display::Channel *NetwMultiplayer::display_ensure_state(
    display::Runtime *p_runtime,
    Node *p_node,
    const StringName &p_source_prop,
    const StringName &p_target_prop,
    const Ref<NetwInterpolate> &p_spec,
    bool p_authoring_tick
) {
    if (p_runtime == nullptr) {
        return nullptr;
    }
    return display::ensure_state(
        p_runtime,
        p_node,
        p_source_prop,
        p_target_prop,
        p_spec,
        p_authoring_tick,
        display_hooks
    );
}

static Ref<NetwPrediction> prediction_of(const Ref<NetwEntity> &p_entity) {
    if (p_entity.is_null()) {
        return Ref<NetwPrediction>();
    }
    return p_entity->prediction_if_minted();
}

int NetwMultiplayer::display_default_role(
    const RID &p_entity,
    bool p_authors_streams
) {
    display::Runtime *p_runtime = display_book->runtime_of(p_entity);
    if (p_runtime == nullptr) {
        return netw::display::ROLE_DISABLED;
    }
    display::RoleFacts facts;
    Node *owner = p_runtime->owner();
    facts.owner_is_authority = owner != nullptr && owner->is_inside_tree()
        && owner->is_multiplayer_authority();
    const Ref<NetwEntity> entity = p_runtime->entity();
    if (entity.is_null()) {
        return display::resolve_role_facts(facts);
    }
    facts.owner_is_authority = facts.owner_is_authority
        && entity->get_controller() == int64_t(get_unique_id());
    if (entity->is_claim_running_ahead()) {
        return display::ROLE_DISABLED;
    }
    facts.authors_streams = p_authors_streams;
    facts.controlled_locally = entity->get_is_controlled_locally();
    const sim::Row *row = sim_rows.row_of(p_entity);
    if (row != nullptr) {
        const int derived = display::role_for_mode(
            row->mode,
            row->facts.predicted,
            facts.controlled_locally
        );
        if (derived != display::ROLE_AUTO) {
            return derived;
        }
    }
    facts.simulates_locally = authoring::declares_prediction(owner);
    return display::resolve_role_facts(facts);
}

double NetwMultiplayer::display_default_chase_clamp(const RID &p_entity) {
    display::Runtime *p_runtime = display_book->runtime_of(p_entity);
    if (p_runtime == nullptr) {
        return INFINITY;
    }
    const Ref<NetwEntity> entity = p_runtime->entity();
    if (entity.is_null()) {
        return INFINITY;
    }
    const predict::Decl *decl = predict_find_declaration(entity.ptr());
    if (decl == nullptr) {
        return NetwPrediction::DEFAULT_TELEPORT_THRESHOLD;
    }
    return MAX(decl->teleport_threshold, 0.0);
}

bool NetwMultiplayer::display_absorbs_recovery(const RID &p_entity) const {
    const display::Runtime *runtime = display_book->runtime_of(p_entity);
    return runtime != nullptr && runtime->get_chase_absorbs();
}

void NetwMultiplayer::display_on_recovered(
    const RID &p_entity,
    const Dictionary &p_deltas,
    bool p_teleported
) {
    display::Runtime *runtime = display_book->runtime_of(p_entity);
    if (runtime != nullptr && runtime->get_chase_absorbs()) {
        display_absorb_recovery(runtime, p_deltas, p_teleported);
    }
}

void NetwMultiplayer::display_default_chase_hook(
    const RID &p_entity,
    bool p_bind
) {
    display::Runtime *runtime = display_book->runtime_of(p_entity);
    if (runtime == nullptr) {
        return;
    }
    runtime->set_chase_absorbs(
        p_bind && prediction_of(runtime->entity()).is_valid()
    );
}

static bool runtime_tracks_key(
    display::Runtime *p_runtime,
    const StringName &p_key
) {
    for (const display::Channel *state : p_runtime->channels()) {
        if (state->get_source_prop() == p_key || state->get_name() == p_key) {
            return true;
        }
    }
    return false;
}

static bool sync_feeds_runtime(
    display::Runtime *p_runtime,
    MultiplayerSynchronizer *p_sync
) {
    const Ref<SceneReplicationConfig> config = p_sync->get_replication_config();
    if (config.is_null()) {
        return false;
    }
    const TypedArray<NodePath> held = config->get_properties();
    for (int at = 0; at < held.size(); at++) {
        const NodePath path = held[at];
        const int names = path.get_subname_count();
        if (names == 0) {
            continue;
        }
        if (runtime_tracks_key(p_runtime, path.get_subname(names - 1))) {
            return true;
        }
    }
    return false;
}

static bool set_feeds_runtime(
    display::Runtime *p_runtime,
    const PropertySet *p_set
) {
    for (const PropertyColumn &column : p_set->columns) {
        if (runtime_tracks_key(p_runtime, column.get_key())) {
            return true;
        }
    }
    return false;
}

void NetwMultiplayer::display_compute_sync_intervals(const RID &p_entity) {
    display::Runtime *p_runtime = display_book->runtime_of(p_entity);
    if (p_runtime == nullptr) {
        return;
    }
    const Ref<NetwEntity> entity = p_runtime->entity();
    if (entity.is_null()) {
        return;
    }
    p_runtime->set_authored_by_state(false);

    double widest = 0.0;
    const TypedArray<MultiplayerSynchronizer> held = entity->synchronizers();
    for (int at = 0; at < held.size(); at++) {
        MultiplayerSynchronizer *sync
            = Object::cast_to<MultiplayerSynchronizer>(held[at]);
        if (sync == nullptr || !sync->is_visibility_public()
            || !sync_feeds_runtime(p_runtime, sync)) {
            continue;
        }
        widest = MAX(
            widest,
            MAX(sync->get_replication_interval(), sync->get_delta_interval())
        );
    }

    const PropertyBindingPtr state = entity->get_state_binding();
    if (state != nullptr && set_feeds_runtime(p_runtime, state->get_set())) {
        p_runtime->set_authored_by_state(true);
    }
    if (!clock_engine().get_configured()) {
        return;
    }
    display::Playhead &playhead = p_runtime->display_playhead();
    const int paced = widest > 0.0
        ? MAX(1, int(Math::ceil(widest * clock_engine().get_tickrate())))
        : playhead.get_expected_interval_ticks();
    playhead.set_expected_interval_ticks(
        MAX(paced, clock_engine().get_send_interval())
    );
    playhead.set_send_interval_ticks(clock_engine().get_send_interval());
}

bool NetwMultiplayer::display_default_authors_streams(const RID &p_entity) {
    display::Runtime *p_runtime = display_book->runtime_of(p_entity);
    if (p_runtime == nullptr) {
        return false;
    }
    const Ref<NetwEntity> entity = p_runtime->entity();
    if (entity.is_null()) {
        return false;
    }
    bool found = false;
    const TypedArray<MultiplayerSynchronizer> held = entity->synchronizers();
    for (int at = 0; at < held.size(); at++) {
        MultiplayerSynchronizer *sync
            = Object::cast_to<MultiplayerSynchronizer>(held[at]);
        if (sync == nullptr || !sync->is_inside_tree()
            || sync->get_replication_config().is_null()
            || !sync->is_visibility_public()
            || !sync_feeds_runtime(p_runtime, sync)) {
            continue;
        }
        if (!sync->is_multiplayer_authority()) {
            return false;
        }
        found = true;
    }

    ReplicationCore *plane = get_replication_plane();
    if (plane == nullptr) {
        return found;
    }
    const LocalVector<PropertyBindingPtr> derived
        = plane->derived_group(p_runtime->get_route());
    for (const PropertyBindingPtr &binding : derived) {
        if (binding == nullptr) {
            continue;
        }
        Node *node = binding->node();
        if (node == nullptr || !node->is_inside_tree()) {
            continue;
        }
        const PropertySet *set = binding->get_set();
        if (set == nullptr
            || set->get_audience() != PropertySet::AUDIENCE_PUBLIC
            || !set_feeds_runtime(p_runtime, set)) {
            continue;
        }
        const bool authored = netw::repl::record_authors(
            set->get_record(),
            set->get_policy(),
            is_host(),
            get_unique_id(),
            node->get_multiplayer_authority(),
            entity->steering()
        );
        if (!authored) {
            return false;
        }
        found = true;
    }
    return found;
}

void NetwMultiplayer::display_pump_runtime(
    display::Runtime *p_runtime,
    const display::Timing &p_timing,
    display::PumpStats &p_stats
) {
    if (p_runtime == nullptr) {
        return;
    }
    display::pump_runtime(p_runtime, p_timing, p_stats, display_hooks);
}

double NetwMultiplayer::display_chase_smooth_time(
    display::Runtime *p_runtime,
    const display::Timing &p_timing
) const {
    if (p_runtime == nullptr) {
        return 0.0;
    }
    return display::chase_smooth_time(p_runtime, p_timing);
}

Error NetwMultiplayer::display_pump_entity(
    const RID &p_entity,
    const display::Timing &p_timing
) {
    display::Runtime *runtime = display_book->runtime_of(p_entity);
    NETW_BUG_COND_V(runtime == nullptr, ERR_DOES_NOT_EXIST);
    display_pump_runtime(runtime, p_timing, display_book->get_stats());
    return OK;
}

void NetwMultiplayer::display_absorb_recovery(
    display::Runtime *p_runtime,
    const Dictionary &p_deltas,
    bool p_teleported
) {
    if (p_runtime == nullptr) {
        return;
    }
    display::absorb_recovery(p_runtime, p_deltas, p_teleported, display_hooks);
    display_book->wake(p_runtime);
}

void NetwMultiplayer::display_absorb_moved(
    display::Runtime *p_runtime,
    const LocalVector<StringName> &p_keys,
    const LocalVector<Variant> &p_deltas
) {
    if (p_runtime == nullptr) {
        return;
    }
    display::absorb_moved(p_runtime, p_keys, p_deltas, display_hooks);
    display_book->wake(p_runtime);
}

bool NetwMultiplayer::counts_verdict(Error p_verdict) {
    return GateVerdictBook::counts(p_verdict);
}

int64_t NetwMultiplayer::stats_get_verdict_count(Error p_verdict) const {
    return verdict_book.total(p_verdict);
}

Error NetwMultiplayer::sink_verdict(
    Error p_verdict,
    int64_t p_route,
    int64_t p_peer
) {
    switch (GateVerdictBook::report_of(p_verdict)) {
        case GateVerdictBook::QUIET:
            break;
        case GateVerdictBook::WARN:
            if (p_peer != 0) {
                fault::raise(
                    this,
                    NETW_FAULT_MALFORMED_DATAGRAM,
                    {String(), String(), String(), p_peer}
                );
            } else {
                fault::raise(
                    this,
                    NETW_FAULT_LINK_FAILED,
                    {vformat(
                         "the packet for route %d (error %d)",
                         int(p_route),
                         int(p_verdict)
                     ),
                     String(),
                     String("a peer that refused a packet")}
                );
            }
            break;
        case GateVerdictBook::DEFECT:
            NETW_BUG_REPORT();
            break;
    }
    if (p_verdict != OK) {
        count_verdict(p_verdict, p_route);
    }
    return p_verdict;
}

Error NetwMultiplayer::finish_stage_verdict(
    int64_t p_stage,
    Error p_verdict,
    int64_t p_route,
    int64_t p_peer
) {
    event_record(p_stage, RID(), p_route, p_peer, p_verdict);
    return sink_verdict(p_verdict, p_route, p_peer);
}

void NetwMultiplayer::clear_verdicts() {
    verdict_book.clear();
}

RID NetwMultiplayer::schema_create(const StringName &p_name) {
    if (p_name == StringName()) {
        return RID();
    }
    const RID existing = schema_core.find(p_name);
    if (existing.is_valid()) {
        schema_core.declare(existing, p_name);
        return existing;
    }
    const RID minted = schemas.rid_create();
    schema_core.declare(minted, p_name);
    return minted;
}

int NetwMultiplayer::schema_add_column(
    const RID &p_schema,
    const StringName &p_key,
    ColumnType p_type,
    int p_stride
) {
    return schema_core.add_column(p_schema, p_key, p_type, p_stride);
}

void NetwMultiplayer::schema_set_column_quantizer(
    const RID &p_schema,
    int p_column,
    const Ref<NetwQuantize> &p_quantizer
) {
    schema_core.set_column_quantizer(p_schema, p_column, p_quantizer);
}

Error NetwMultiplayer::schema_seal(const RID &p_schema) {
    return schema_core.seal(p_schema);
}

RID NetwMultiplayer::schema_find(const StringName &p_name) const {
    return schema_core.find(p_name);
}

RID NetwMultiplayer::schema_find_or_adopt(const StringName &p_name) {
    const RID found = schema_find(p_name);
    if (found.is_valid() || schema_model::find(p_name).is_null()) {
        return found;
    }
    adopt_schema_declarations();
    return schema_find(p_name);
}

RID NetwMultiplayer::table_find_or_adopt(const StringName &p_name) {
    const RID found = table_find(p_name);
    if (found.is_valid() || schema_model::find(p_name).is_null()) {
        return found;
    }
    adopt_schema_declarations();
    adopt_table_declarations();
    return table_find(p_name);
}

StringName NetwMultiplayer::schema_get_name(const RID &p_schema) const {
    return schema_core.name_of(p_schema);
}

void NetwMultiplayer::adopt_schema_declarations() {
    const TypedArray<NetwSchema> declared = schema_model::declarations();
    for (int at = 0; at < declared.size(); ++at) {
        const Ref<NetwSchema> declaration = declared[at];
        if (declaration.is_null()) {
            continue;
        }
        const RID schema = schema_create(declaration->get_schema_name());
        if (!schema.is_valid()) {
            continue;
        }
        for (const NetwSchema::Column &column : declaration->columns_of()) {
            const int index = schema_add_column(
                schema,
                column.key,
                column.type,
                column.stride
            );
            if (index >= 0) {
                schema_set_column_quantizer(schema, index, column.quantizer);
            }
        }
        schema_core.set_storage_version(
            schema,
            declaration->get_storage_version()
        );
        const Dictionary steps = declaration->get_migrations();
        const Array froms = steps.keys();
        for (int at_step = 0; at_step < froms.size(); ++at_step) {
            const Callable step = steps[froms[at_step]];
            game_call_declared(step, MIGRATION_STEP);
            schema_core.add_migration(schema, int(froms[at_step]), step);
        }
        if (schema_seal(schema) != OK) {
            fault::raise(
                this,
                NETW_FAULT_DECLARED_TWICE,
                {vformat(
                     "Schema '%s'",
                     String(declaration->get_schema_name())
                 ),
                 "this session"}
            );
        } else if (!declaration->adopters.has(gd::instance_id(this))) {
            declaration->adopters.push_back(gd::instance_id(this));
        }
    }
}

void NetwMultiplayer::adopt_table_declarations() {
    const TypedArray<NetwSchema> declared = schema_model::declarations();
    for (int at = 0; at < declared.size(); ++at) {
        const Ref<NetwSchema> declaration = declared[at];
        if (declaration.is_null() || !declaration->is_replicated()) {
            continue;
        }
        const RID schema = schema_core.find(declaration->get_schema_name());
        if (!schema.is_valid()) {
            continue;
        }
        const RID table = table_create(schema);
        if (!table.is_valid()) {
            fault::raise(
                this,
                NETW_FAULT_SETTING_REFUSED,
                {vformat(
                     "{NetwSchema.replicated} on schema '%s'",
                     String(declaration->get_schema_name())
                 ),
                 "a column whose size varies",
                 "a {NetwChannel} for it, or a schema without replicated"}
            );
            continue;
        }
        table_set_param(
            table,
            TABLE_PARAM_RELIABLE,
            declaration->is_reliable()
        );
    }
}

RID NetwMultiplayer::table_create(const RID &p_schema) {
    const SchemaRecord *record = schema_core.record_of(p_schema);
    if (record == nullptr || !record->sealed) {
        return RID();
    }
    const RID *bound = table_by_name.getptr(record->name);
    if (bound && bound->is_valid()) {
        return *bound;
    }
    const RID minted = tables.rid_create();
    if (store_world.arrays.declare(minted, record) != OK) {
        return RID();
    }
    table_by_name[record->name] = minted;
    table_schema[int64_t(minted.get_id())] = p_schema;
    return minted;
}

Ref<NetwTable> NetwMultiplayer::get_table_handle(const RID &p_table) {
    if (const Ref<NetwTable> *held = views.table_handles.getptr(p_table)) {
        return *held;
    }
    const Ref<NetwTable> minted = NetwTable::over(p_table, this);
    if (minted.is_valid()) {
        views.table_handles[p_table] = minted;
    }
    return minted;
}

RID NetwMultiplayer::table_get_schema(const RID &p_table) const {
    const RID *bound = table_schema.getptr(int64_t(p_table.get_id()));
    return bound ? *bound : RID();
}

Error NetwMultiplayer::table_set_param(
    const RID &p_table,
    TableParam p_param,
    const Variant &p_value
) {
    if (!store_world.arrays.is_valid(p_table)) {
        return ERR_DOES_NOT_EXIST;
    }
    if (p_param != TABLE_PARAM_RELIABLE) {
        return ERR_INVALID_PARAMETER;
    }
    store_world.arrays.set_reliable(p_table, bool(p_value));
    return OK;
}

Variant NetwMultiplayer::table_get_param(
    const RID &p_table,
    TableParam p_param
) const {
    if (p_param == TABLE_PARAM_RELIABLE) {
        return store_world.arrays.is_reliable(p_table);
    }
    return Variant();
}

RID NetwMultiplayer::table_find(const StringName &p_name) const {
    const RID *bound = table_by_name.getptr(p_name);
    return bound ? *bound : RID();
}

int NetwMultiplayer::table_get_wire_hash(const RID &p_table) const {
    return store_world.arrays.schema_hash(p_table);
}

Error NetwMultiplayer::table_write_routes(
    const RID &p_table,
    const PackedInt64Array &p_routes
) {
    return store_world.arrays.write_routes(p_table, p_routes);
}

Error NetwMultiplayer::table_write_column(
    const RID &p_table,
    int p_column,
    const Variant &p_data
) {
    return store_world.arrays.write_column(p_table, p_column, p_data);
}

Error NetwMultiplayer::table_commit(const RID &p_table) {
    const Error verdict
        = store_world.arrays.commit(p_table, clock_engine().get_tick());
    if (event_records(EventPlane::TABLE_COMMIT)) {
        event_record(
            EventPlane::TABLE_COMMIT,
            RID(),
            0,
            0,
            verdict,
            table_read_routes(p_table).size()
        );
    }
    return verdict;
}

PackedInt64Array NetwMultiplayer::table_read_routes(const RID &p_table) const {
    return store_world.arrays.read_routes(p_table);
}

Variant NetwMultiplayer::table_read_column(
    const RID &p_table,
    int p_column
) const {
    return store_world.arrays.read_column(p_table, p_column);
}

PackedInt64Array NetwMultiplayer::table_read_births(const RID &p_table) const {
    return store_world.arrays.read_births(p_table);
}

PackedInt64Array NetwMultiplayer::table_read_deaths(const RID &p_table) const {
    return store_world.arrays.read_deaths(p_table);
}

int NetwMultiplayer::table_get_row(const RID &p_table, int64_t p_route) const {
    return store_world.arrays.row_of(p_table, p_route);
}

PackedInt32Array NetwMultiplayer::table_get_rows(
    const RID &p_table,
    const PackedInt64Array &p_routes
) const {
    return store_world.arrays.rows_of(p_table, p_routes);
}

int64_t NetwMultiplayer::table_get_tick(const RID &p_table) const {
    return store_world.arrays.tick_of(p_table);
}

void NetwMultiplayer::lagcomp_effect_arm(
    const StringName &p_key,
    const Callable &p_revert,
    int p_timeout_ticks
) {
    const int ttl
        = p_timeout_ticks > 0 ? p_timeout_ticks : EFFECT_TIMEOUT_TICKS;
    effects.arm(p_key, p_revert, clock_engine().get_tick() + ttl);
}

bool NetwMultiplayer::lagcomp_effect_watch(
    const StringName &p_key,
    const Callable &p_confirmed,
    const Callable &p_denied
) {
    const bool watching = effects.watch(p_key, p_confirmed, p_denied);
    if (!watching) {
        fault::raise(
            this,
            NETW_FAULT_REWIND_MISUSE,
            {String(p_key),
             String(),
             String(),
             0,
             NETW_REWIND_EFFECT_NOT_ARMED}
        );
    }
    return watching;
}

StringName NetwMultiplayer::lagcomp_action_key(
    const RID &p_entity,
    int64_t p_view_tick,
    int64_t p_slot
) const {
    const Ref<NetwEntity> wrapper = entity_get_view(p_entity);
    const StringName named
        = wrapper.is_valid() ? wrapper->get_name() : StringName();
    if (named.is_empty()) {
        Array act_args;
        act_args.push_back(p_view_tick);
        act_args.push_back(p_slot);
        return StringName(String("act__{0}__{1}").format(act_args));
    }
    Array act_args;
    act_args.push_back(named);
    act_args.push_back(p_view_tick);
    act_args.push_back(p_slot);
    return StringName(String("act__{0}__{1}__{2}").format(act_args));
}

Error NetwMultiplayer::write_display_param(
    const Ref<NetwEntity> &p_wrapper,
    int p_param,
    const Variant &p_value
) {
    NetwEntityRecord *const record = record_of(p_wrapper.ptr());
    if (record == nullptr) {
        return ERR_DOES_NOT_EXIST;
    }
    display::Decl *fallback = &record->display_declaration();
    const RID rid = p_wrapper->get_rid_handle();
    const Error verdict
        = display_book->write_param(rid, *fallback, p_param, p_value);
    const display::Decl *stored = display_book->decl_ptr(rid);
    if (stored != nullptr) {
        *fallback = *stored;
    }
    return verdict;
}

Error NetwMultiplayer::display_declare(
    const RID &p_entity,
    int64_t p_comp,
    const StringName &p_track,
    const Ref<NetwInterpolate> &p_spec
) {
    const Ref<NetwEntity> wrapper = entity_get_view(p_entity);
    if (wrapper.is_null()) {
        return ERR_DOES_NOT_EXIST;
    }
    if (p_track.is_empty() || p_spec.is_null()) {
        return ERR_INVALID_DATA;
    }
    Node *node = entity_component_node(p_entity, p_comp);
    if (node == nullptr) {
        return ERR_UNAVAILABLE;
    }
    netw::script::model::configure_node_property(node, p_track)
        ->interpolate(gd::array_of(p_spec));
    display_book->mark_dirty(p_entity, netw::display::DIRT_RUNTIME);

    DisplayTrackRow row;
    row.comp = p_comp;
    row.spec = p_spec;
    display_declarations[p_entity.get_id()][p_track] = row;
    return OK;
}

void NetwMultiplayer::display_undeclare(const RID &p_entity) {
    const int64_t route = display_book->route_of(p_entity);
    if (route > 0) {
        display_release_route(route);
    }
    display_declarations.erase(p_entity.get_id());
    display_target_items.erase(p_entity.get_id());
    display_callbacks.erase(p_entity.get_id());
}

Error NetwMultiplayer::display_record_track(
    const RID &p_entity,
    const StringName &p_track,
    const Variant &p_value,
    int64_t p_tick
) {
    const HashMap<StringName, DisplayTrackRow> *tracks
        = display_declarations.getptr(p_entity.get_id());
    if (tracks == nullptr) {
        return ERR_DOES_NOT_EXIST;
    }
    const DisplayTrackRow *row = tracks->getptr(p_track);
    if (row == nullptr) {
        return ERR_DOES_NOT_EXIST;
    }
    Node *node = entity_component_node(p_entity, row->comp);
    if (node == nullptr) {
        return ERR_UNAVAILABLE;
    }
    display_record(node, p_track, p_value, p_tick, row->spec, false);
    return OK;
}

Error NetwMultiplayer::display_write_track(
    const RID &p_entity,
    const StringName &p_track,
    const Variant &p_value
) {
    const Callable *callback = display_callbacks.getptr(p_entity.get_id());
    if (callback != nullptr && callback->is_valid()) {
        callback->call(p_entity, p_track, p_value);
        return OK;
    }
    const RID *item = display_target_items.getptr(p_entity.get_id());
    if (item == nullptr || !item->is_valid()) {
        return ERR_DOES_NOT_EXIST;
    }
    switch (p_value.get_type()) {
        case Variant::TRANSFORM2D:
            RenderingServer::get_singleton()->canvas_item_set_transform(
                *item,
                p_value
            );
            return OK;
        case Variant::TRANSFORM3D:
            RenderingServer::get_singleton()->instance_set_transform(
                *item,
                p_value
            );
            return OK;
        default:
            return ERR_INVALID_DATA;
    }
}

Error NetwMultiplayer::display_set_param(
    const RID &p_entity,
    DisplayParam p_param,
    const Variant &p_value
) {
    const Ref<NetwEntity> wrapper = entity_get_view(p_entity);
    if (wrapper.is_null()) {
        return ERR_DOES_NOT_EXIST;
    }
    if (p_param == DISPLAY_PARAM_VISUAL_ROOT
        && p_value.get_type() != Variant::NODE_PATH
        && p_value.get_type() != Variant::STRING) {
        return display_set_target_node(
            p_entity,
            entity_component_node(p_entity, int64_t(p_value))
        );
    }
    return write_display_param(wrapper, p_param, p_value);
}

Error NetwMultiplayer::display_set_target_node(
    const RID &p_entity,
    Node *p_node
) {
    const Ref<NetwEntity> wrapper = entity_get_view(p_entity);
    if (wrapper.is_null()) {
        return ERR_DOES_NOT_EXIST;
    }
    Node *owner = wrapper->get_owner();
    if (p_node == nullptr) {
        return write_display_param(
            wrapper,
            DISPLAY_PARAM_VISUAL_ROOT,
            NodePath()
        );
    }
    if (owner == nullptr
        || (p_node != owner && !owner->is_ancestor_of(p_node))) {
        return ERR_INVALID_PARAMETER;
    }
    const Error verdict = write_display_param(
        wrapper,
        DISPLAY_PARAM_VISUAL_ROOT,
        owner->get_path_to(p_node)
    );
    display_target_items.erase(p_entity.get_id());
    display_callbacks.erase(p_entity.get_id());
    return verdict;
}

void NetwMultiplayer::display_set_target_item(
    const RID &p_entity,
    const RID &p_item
) {
    if (entity_get_view(p_entity).is_null()) {
        return;
    }
    if (p_item.is_valid()) {
        display_target_items.insert(p_entity.get_id(), p_item);
    } else {
        display_target_items.erase(p_entity.get_id());
    }
    display_callbacks.erase(p_entity.get_id());
    display_book->mark_dirty(p_entity, netw::display::DIRT_RUNTIME);
}

void NetwMultiplayer::display_set_callback(
    const RID &p_entity,
    const Callable &p_callback
) {
    if (entity_get_view(p_entity).is_null()) {
        return;
    }
    if (p_callback.is_valid()) {
        display_callbacks.insert(p_entity.get_id(), p_callback);
    } else {
        display_callbacks.erase(p_entity.get_id());
    }
    display_target_items.erase(p_entity.get_id());
    display_book->mark_dirty(p_entity, netw::display::DIRT_RUNTIME);
}

void NetwMultiplayer::display_clear_declarations() {
    display_declarations.clear();
    display_target_items.clear();
    display_callbacks.clear();
}

Variant NetwMultiplayer::display_get_param(
    const RID &p_entity,
    DisplayParam p_param
) {
    const display::Decl *decl = display_book->decl_ptr(p_entity);
    return decl != nullptr ? decl->get_param(p_param) : Variant();
}

void NetwMultiplayer::display_reset(const RID &p_entity) {
    display::Runtime *runtime = display_book->runtime_of(p_entity);
    if (runtime == nullptr) {
        return;
    }
    runtime->reset(
        clock_engine().get_display_offset(),
        clock_engine().recommended_display_offset()
    );
    display_book->wake(runtime);
}

void NetwMultiplayer::display_snap(
    const RID &p_entity,
    const StringName &p_track,
    const Variant &p_value
) {
    display::Runtime *runtime = display_book->runtime_of(p_entity);
    if (runtime == nullptr) {
        return;
    }
    runtime->snap_named(p_track, p_value);
    display_book->wake(runtime);
}

Variant NetwMultiplayer::display_get_value(
    const RID &p_entity,
    const StringName &p_track
) {
    display::Runtime *runtime = display_book->runtime_of(p_entity);
    if (runtime == nullptr) {
        return Variant();
    }
    const display::Channel *channel = runtime->channel_named(p_track);
    return channel != nullptr ? channel->get_last_written() : Variant();
}

int64_t NetwMultiplayer::display_get_tick(const RID &p_entity) {
    display::Runtime *runtime = display_book->runtime_of(p_entity);
    return runtime != nullptr ? runtime->authoring_tick() : -1;
}

Variant NetwMultiplayer::display_get_track_stat(
    const RID &p_entity,
    const StringName &p_track,
    const StringName &p_stat
) {
    display::Runtime *runtime = display_book->runtime_of(p_entity);
    return runtime != nullptr ? runtime->track_stat(p_track, p_stat)
                              : Variant();
}

bool NetwMultiplayer::sync_policy_admits(
    int p_policy,
    int64_t p_sender,
    const RID &p_entity,
    int64_t p_comp
) {
    const Ref<NetwEntity> wrapper = entity_get_view(p_entity);
    Node *node = entity_component_node(p_entity, p_comp);
    if (wrapper.is_null() || node == nullptr) {
        return false;
    }
    return entity::Control::policy_admits(
        p_policy,
        p_sender,
        node->get_multiplayer_authority(),
        wrapper->steering()
    );
}

uint64_t NetwMultiplayer::property_set_id_of(const RID &p_set) const {
    const uint64_t *held = minted_property_sets.getptr(p_set.get_id());
    return held != nullptr ? *held : repl::SetBook::NONE;
}

PropertySet *NetwMultiplayer::property_set_of(const RID &p_set) const {
    return set_book.get(property_set_id_of(p_set));
}

PropertySet *NetwMultiplayer::mutable_property_set(const RID &p_set) {
    PropertySet *record = property_set_of(p_set);
    NETW_BUG_COND_V(record == nullptr, nullptr);
    NETW_BUG_COND_V(record->get_sealed(), nullptr);
    return record;
}

RID NetwMultiplayer::property_set_create(
    const RID &p_schema,
    RecordKind p_record
) {
    if (p_record != RECORD_KIND_STATE && p_record != RECORD_KIND_INPUT
        && p_record != RECORD_KIND_BROADCAST) {
        return RID();
    }
    const SchemaRecord *declaration = schema_core.record_of(p_schema);
    if (declaration == nullptr || schema_core.has_stride(p_schema)) {
        return RID();
    }
    const RID minted = property_sets.rid_create();
    const uint64_t held = set_book.create();
    PropertySet *record = set_book.get(held);
    property_set_builder::for_record(*record, p_record);
    record->schema = *declaration;
    record->rid = minted;
    minted_property_sets.insert(minted.get_id(), held);
    return minted;
}

int NetwMultiplayer::property_set_add_column(const RID &p_set, int p_column) {
    PropertySet *record = mutable_property_set(p_set);
    if (record == nullptr) {
        return -1;
    }
    const SchemaColumn *shape
        = SchemaCore::column_at(&record->schema, p_column);
    if (shape == nullptr || record->member(p_column) != nullptr) {
        return -1;
    }
    PropertyColumn member;
    member.shape = *shape;
    member.schema_column = p_column;
    record->columns.push_back(member);
    return int(record->columns.size()) - 1;
}

void NetwMultiplayer::property_set_set_column_param(
    const RID &p_set,
    int p_field,
    int p_param,
    const Variant &p_value
) {
    PropertySet *record = mutable_property_set(p_set);
    if (record == nullptr) {
        return;
    }
    if (p_field < 0 || p_field >= int(record->columns.size())) {
        return;
    }
    PropertyColumn &target = record->columns[uint32_t(p_field)];
    switch (p_param) {
        case COLUMN_PARAM_UNCHECKED:
            target.unchecked = bool(p_value);
            return;
        case COLUMN_PARAM_CORRECTION_THRESHOLD:
            target.correction_threshold = double(p_value);
            return;
        case COLUMN_PARAM_TELEPORT_THRESHOLD:
            target.teleport_threshold = double(p_value);
            return;
        case COLUMN_PARAM_EXTRAPOLATE:
            target.rate_property = StringName(p_value);
            return;
        case COLUMN_PARAM_TELEPORT_ONLY:
            target.explicit_teleport_only = bool(p_value);
            return;
        case COLUMN_PARAM_LANE:
            target.lane = int64_t(p_value);
            target.watch = target.lane == PropertySet::RETAINED;
            record->reproject_lanes();
            return;
        case COLUMN_PARAM_CORRECTION_WEIGHT:
            target.correction_weight = double(p_value);
            return;
        default:
            fault::raise(
                this,
                NETW_FAULT_BAD_VALUE,
                {"A column setting",
                 vformat("setting %d", int(p_param)),
                 "a column setting that exists"}
            );
    }
}

void NetwMultiplayer::property_set_set_param(
    const RID &p_set,
    int p_param,
    const Variant &p_value
) {
    PropertySet *record = mutable_property_set(p_set);
    if (record == nullptr) {
        return;
    }
    switch (p_param) {
        case SET_PARAM_WINDOW:
            record->window = int64_t(p_value);
            return;
        case SET_PARAM_AUDIENCE:
            record->audience = int64_t(p_value);
            return;
        case SET_PARAM_POLICY:
            record->policy
                = static_cast<NetwMemberConfig::Policy>(int(p_value));
            return;
        case SET_PARAM_CADENCE:
            record->cadence = int64_t(p_value);
            return;
        case SET_PARAM_STAMP:
            record->stamp = int64_t(p_value);
            return;
        case SET_PARAM_PROFILE:
            record->profile = int64_t(p_value);
            return;
        case SET_PARAM_CHANNEL:
            record->channel = int64_t(p_value);
            return;
        case SET_PARAM_RELIABLE:
            record->reliable = bool(p_value);
            return;
        case SET_PARAM_HEARTBEAT:
            record->heartbeat = int64_t(p_value);
            return;
        default:
            fault::raise(
                this,
                NETW_FAULT_BAD_VALUE,
                {"A property set setting",
                 vformat("setting %d", int(p_param)),
                 "a property set setting that exists"}
            );
    }
}

void NetwMultiplayer::property_set_clear() {
    property_set_by_script.clear();
    entity_property_sets.clear();
    minted_property_sets.clear();
    set_book.clear();
    property_sets.clear();
}

RID NetwMultiplayer::script_schema(const Ref<Script> &p_script, Node *p_node) {
    StringName name("@anonymous_script");
    if (p_script.is_valid()) {
        name = StringName(
            p_script->get_path().is_empty() ? "@script:"
                    + String::num_int64(int64_t(p_script->get_instance_id()))
                                            : p_script->get_path()
        );
    }
    const RID schema = schema_create(name);
    const Dictionary configs
        = netw::script::model::get_property_configs(p_script);
    const Array properties = configs.keys();
    for (int at = 0; at < properties.size(); ++at) {
        const StringName property = properties[at];
        const Ref<NetwPropertyConfig> config = configs[property];
        if (config.is_null()) {
            continue;
        }
        const int column = schema_add_column(
            schema,
            property,
            static_cast<ColumnType>(
                PropertySet::column_type_for(p_script, p_node, property)
            ),
            1
        );
        const Array quantizers = config->get_quantizers();
        if (column >= 0 && !quantizers.is_empty()) {
            schema_set_column_quantizer(schema, column, quantizers[0]);
        }
    }
    if (schema_seal(schema) != OK) {
        fault::raise(
            this,
            NETW_FAULT_DECLARED_TWICE,
            {"The list of replicated properties", String(name)},
            gd::instance_id(p_node)
        );
    }
    return schema;
}

RID NetwMultiplayer::adopted_property_set(
    const Ref<Script> &p_script,
    RecordKind p_record_kind
) const {
    const uint64_t key = p_script.is_valid()
        ? uint64_t(p_script->get_instance_id())
        : uint64_t(0);
    const HashMap<int, RID> *cached = property_set_by_script.getptr(key);
    if (cached == nullptr) {
        return RID();
    }
    const RID *found = cached->getptr(p_record_kind);
    return found != nullptr ? *found : RID();
}

RID NetwMultiplayer::adopt_property_set(
    const Ref<Script> &p_script,
    RecordKind p_record_kind,
    const PropertySet &p_source,
    Node *p_node
) {
    const RID adopted = adopted_property_set(p_script, p_record_kind);
    if (adopted.is_valid()) {
        return adopted;
    }
    const RID schema = script_schema(p_script, p_node);
    const RID set = property_set_create(schema, p_record_kind);
    if (!set.is_valid()) {
        return set;
    }
    property_set_set_param(set, SET_PARAM_WINDOW, p_source.get_window());
    property_set_set_param(set, SET_PARAM_AUDIENCE, p_source.get_audience());
    property_set_set_param(set, SET_PARAM_POLICY, p_source.get_policy());
    property_set_set_param(set, SET_PARAM_CADENCE, p_source.get_cadence());
    property_set_set_param(set, SET_PARAM_STAMP, p_source.get_stamp());
    property_set_set_param(set, SET_PARAM_PROFILE, p_source.get_profile());
    property_set_set_param(set, SET_PARAM_CHANNEL, p_source.get_channel());
    property_set_set_param(set, SET_PARAM_RELIABLE, p_source.get_reliable());
    property_set_set_param(set, SET_PARAM_HEARTBEAT, p_source.get_heartbeat());
    for (const PropertyColumn &member : p_source.columns) {
        const int column = schema_core.find_column(schema, member.get_key());
        const int at = property_set_add_column(set, column);
        if (at < 0) {
            continue;
        }
        property_set_set_column_param(
            set,
            at,
            COLUMN_PARAM_UNCHECKED,
            member.unchecked
        );
        property_set_set_column_param(
            set,
            at,
            COLUMN_PARAM_CORRECTION_THRESHOLD,
            member.correction_threshold
        );
        property_set_set_column_param(
            set,
            at,
            COLUMN_PARAM_TELEPORT_THRESHOLD,
            member.teleport_threshold
        );
        property_set_set_column_param(
            set,
            at,
            COLUMN_PARAM_EXTRAPOLATE,
            member.rate_property
        );
        property_set_set_column_param(
            set,
            at,
            COLUMN_PARAM_TELEPORT_ONLY,
            member.explicit_teleport_only
        );
        property_set_set_column_param(set, at, COLUMN_PARAM_LANE, member.lane);
        property_set_set_column_param(
            set,
            at,
            COLUMN_PARAM_CORRECTION_WEIGHT,
            member.correction_weight
        );
    }
    property_set_seal(set);
    const uint64_t key = p_script.is_valid()
        ? uint64_t(p_script->get_instance_id())
        : uint64_t(0);
    property_set_by_script[key][p_record_kind] = set;
    return set;
}

Error NetwMultiplayer::property_set_seal(const RID &p_set) {
    PropertySet *record = property_set_of(p_set);
    if (record == nullptr) {
        return ERR_DOES_NOT_EXIST;
    }
    record->seal();
    return OK;
}

void NetwMultiplayer::effect_adopt(const StringName &p_key) {
    effects.adopt(p_key);
}

void NetwMultiplayer::effect_discard(const StringName &p_key) {
    effects.discard(p_key);
}

void NetwMultiplayer::effect_drain() {
    if (is_inside_step() || config_applying || action_handler_depth > 0) {
        return;
    }
    const Ref<NetwMultiplayer> keep(this);
    effects.drain();
}

bool NetwMultiplayer::lagcomp_effect_pending(const StringName &p_key) const {
    return effects.pending(p_key);
}

int64_t NetwMultiplayer::lagcomp_effect_count() const {
    return effects.count();
}

void NetwMultiplayer::event_arm(BitField<EventGroup> p_groups) {
    plane.arm(int64_t(p_groups));
}

PackedInt64Array NetwMultiplayer::event_drain() {
    return plane.drain();
}

void NetwMultiplayer::event_record(
    int64_t p_kind,
    const RID &p_entity,
    int64_t p_route,
    int64_t p_peer,
    int64_t p_verdict,
    int64_t p_a,
    int64_t p_b
) {
    if (!plane.records(p_kind)) {
        return;
    }
    const RID entity = p_entity.is_valid() || p_route == 0
        ? p_entity
        : liveness_core->rid_from_route(int(p_route));
    EventPlane::Row row;
    row.kind = p_kind;
    row.tick = clock_engine().get_tick();
    row.entity = int64_t(entity.get_id());
    row.route = p_route;
    row.peer = p_peer;
    row.verdict = p_verdict;
    row.a = p_a;
    row.b = p_b;
    plane.keep(row);
}

void NetwMultiplayer::lagcomp_effect_sweep(int64_t p_tick) {
    effects.sweep(p_tick);
}

SyncCompat *NetwMultiplayer::sync_adapter() const {
    ReplicationCore *plane = get_replication_plane();
    return plane != nullptr ? plane->get_sync_compat() : nullptr;
}

void NetwMultiplayer::sync_compat_entity_live(
    int64_t p_route,
    const Ref<NetwEntity> &p_entity
) {
    NETW_ZONE_NC("Compat entity live", colors::WIRE);
    if (SyncCompat *adapter = sync_adapter()) {
        adapter->on_entity_live(p_route, p_entity);
    }
}

void NetwMultiplayer::sync_compat_visibility_changed(
    int64_t p_peer,
    Node *p_root
) {
    if (SyncCompat *adapter = sync_adapter()) {
        adapter->on_sync_visibility_changed(p_peer, p_root);
    }
}

void NetwMultiplayer::sync_compat_config_changed() {
    if (SyncCompat *adapter = sync_adapter()) {
        adapter->on_config_changed();
    }
}

void NetwMultiplayer::sync_compat_node_removed(Node *p_node) {
    if (SyncCompat *adapter = sync_adapter()) {
        adapter->on_node_removed(p_node);
    }
}

} // namespace netw
