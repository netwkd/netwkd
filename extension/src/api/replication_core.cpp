#include "netw/api/replication_core.hpp"

#include "godot/class_db.hpp"
#include "godot/multiplayer.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/call_args.hpp"
#include "netw/entity/control.hpp"
#include "netw/fault_book.hpp"
#include "netw/liveness_core.hpp"
#include "netw/log.hpp"
#include "netw/session/event_plane.hpp"
#include "netw/session/frames.hpp"
#include "netw/spawn/record.hpp"
#include "netw/store/array.hpp"
#include "netw/store/store.hpp"
#include "netw/wire/frame.hpp"

using namespace godot;

namespace netw {

using repl::PropertyBindingPtr;
using repl::PropertySet;

namespace {

const int64_t USER_CHANNEL_FIRST = 100;
const int64_t USER_CHANNEL_LAST = 254;

} // namespace

ReplicationCore::ReplicationCore() {
    const wire::WireRegistry registry = wire::WireRegistry::create_default();
    const wire::ChannelDecl *requests
        = registry.find_channel(wire::CHANNEL_CONTROL_REQUEST);
    control_requests_ordered = requests != nullptr
        && requests->reliability == wire::Reliability::RELIABLE;
}

void ReplicationCore::install(
    NetwMultiplayer *p_session,
    Clock *p_clock,
    store::Store *p_store,
    NetwLivenessCore *p_liveness,
    NetwChannelBook *p_channels,
    repl::BindingBook *p_bindings,
    repl::SetBook *p_sets,
    NetwPredictionEngine *p_prediction
) {
    if (p_session == nullptr) {
        return;
    }
    session = p_session;
    clock_engine = p_clock;
    store_world = p_store;
    liveness_core = p_liveness;
    channels = p_channels;

    sync_pipeline.seat(
        session,
        p_clock,
        p_store,
        p_liveness,
        p_bindings,
        p_sets,
        p_prediction
    );
    sync_pipeline.set_sync_model(&sync_model);
    sync_pipeline.set_channels(
        wire::CHANNEL_SYNC_ROW,
        wire::CHANNEL_SYNC_ROW_WINDOW,
        wire::CHANNEL_SYNC_ROW_DELTA,
        wire::CHANNEL_ROW_CONTROL,
        wire::CHANNEL_PROPERTY_SYNC,
        wire::CHANNEL_SIGNAL
    );

    spawn_pipeline.seat(session, p_clock, p_liveness);
    spawn_pipeline.set_channels(
        wire::CHANNEL_SPAWN,
        wire::CHANNEL_DESPAWN,
        wire::CHANNEL_HIDE,
        wire::CHANNEL_REPARENT
    );

    spawner_compat.seat(session);

    sync_compat.seat(session, p_clock, p_liveness);
    sync_compat.set_sync_model(&sync_model);
    sync_compat.set_spawn_book(spawn_pipeline.get_spawn_book());
    sync_compat.set_channels(wire::CHANNEL_SYNC, wire::CHANNEL_SYNC_DELTA);

    spawn_pipeline.set_sync_adapter(&sync_compat);
}

#if defined(DEBUG_ENABLED)
void ReplicationCore::seat_profiler(
    wire::NetworkProfiler *p_profiler,
    uint32_t *p_outgoing
) {
    network_profiler = p_profiler;
    sync_pipeline.seat_profiler(p_profiler, p_outgoing);
    sync_compat.seat_profiler(p_profiler, p_outgoing);
}
#endif

#if defined(NETW_TESTS)
void ReplicationCore::seat_idle(session::IdleWork *p_idle) {
    sync_pipeline.seat_idle(p_idle);
}
#endif

void ReplicationCore::unseat() {
    session = nullptr;
    sync_pipeline.unseat();
    spawn_pipeline.unseat();
    spawner_compat.seat(nullptr);
    sync_compat.unseat();
}

bool ReplicationCore::get_is_applying_remote_frame() const {
    return session != nullptr ? session->spawn_applying_remote_frame() : false;
}

PropertyBindingPtr ReplicationCore::derived_binding(
    Node *p_node,
    int64_t p_record
) {
    return sync_pipeline.derived_binding(p_node, p_record);
}

LocalVector<PropertyBindingPtr> ReplicationCore::derived_group(
    int64_t p_route
) {
    return sync_pipeline.derived_group(p_route);
}

void ReplicationCore::note_peer_ack(
    int64_t p_peer_id,
    int64_t p_acked_seq,
    uint32_t p_history
) {
    sync_pipeline.note_peer_ack(p_peer_id, p_acked_seq, p_history);
}

void ReplicationCore::send_to(
    int64_t p_peer_id,
    int64_t p_route,
    int64_t p_channel,
    const PackedByteArray &p_payload,
    bool p_reliable,
    int64_t p_comp,
    const String &p_path
) {
    if (session != nullptr) {
        session->send_to(
            p_peer_id,
            p_route,
            p_channel,
            p_payload,
            p_reliable,
            p_comp,
            p_path
        );
    }
}

void ReplicationCore::broadcast_control(
    const Ref<NetwEntity> &p_entity,
    const session::ControlApply &p_decision,
    int64_t p_issuer
) {
    if (session == nullptr || session->session_get_inner().is_null()
        || session->session_get_inner()->get_multiplayer_peer().is_null()) {
        return;
    }
    session::ControlApply shared = p_decision;
    shared.op = 0;
    shared.outcome = 0;
    const PackedByteArray payload = session::frame_write(shared);
    const PackedInt32Array recipients = session->rpc_recipients(
        p_entity.is_valid() ? p_entity->get_rid_handle() : RID()
    );
    for (int at = 0; at < recipients.size(); ++at) {
        if (p_decision.op != 0 && recipients[at] == p_issuer) {
            reply_control(p_entity, p_decision, p_issuer);
            continue;
        }
        send_to(
            recipients[at],
            p_entity->get_route(),
            wire::CHANNEL_CONTROL_APPLY,
            payload,
            true,
            0,
            String()
        );
    }
}

void ReplicationCore::reply_control(
    const Ref<NetwEntity> &p_entity,
    const session::ControlApply &p_decision,
    int64_t p_issuer
) {
    if (session == nullptr || session->session_get_inner().is_null()
        || session->session_get_inner()->get_multiplayer_peer().is_null()) {
        return;
    }
    send_to(
        p_issuer,
        p_entity->get_route(),
        wire::CHANNEL_CONTROL_APPLY,
        session::frame_write(p_decision),
        true,
        0,
        String()
    );
}

void ReplicationCore::watch_control(const Ref<NetwEntity> &p_entity) {
    const ObjectID held = gd::instance_id(p_entity.ptr());
    if (control_waiting.find(held) < 0) {
        control_waiting.push_back(held);
    }
}

void ReplicationCore::watch_claim_image(const Ref<NetwEntity> &p_entity) {
    const ObjectID held = gd::instance_id(p_entity.ptr());
    if (claim_imaging.find(held) < 0) {
        claim_imaging.push_back(held);
    }
}

bool ReplicationCore::lanes_stream_to(
    const Ref<NetwEntity> &p_entity,
    int64_t p_peer_id
) {
    ReplicationSend *send = sync_pipeline.row_send();
    if (session == nullptr || send == nullptr) {
        return true;
    }
    const int64_t live = liveness_core->route_of(p_entity->get_rid_handle());
    const LocalVector<PropertyBindingPtr> group
        = derived_group(live > 0 ? live : p_entity->get_route());
    for (const PropertyBindingPtr &binding : group) {
        if (binding == nullptr || binding->get_set() == nullptr
            || binding->get_set()->get_record()
                != PropertySet::RECORD_BROADCAST) {
            continue;
        }
        const repl::SetRow *row = sync_model.row_for(
            binding->get_route(),
            int64_t(NetwSyncModel::KIND_DERIVED),
            binding->get_order_key(),
            PropertySet::RECORD_BROADCAST
        );
        if (row == nullptr) {
            continue;
        }
        bool streams = false;
        for (uint8_t family = 0; family <= wire::STREAM_FAMILY_CEILING;
             ++family) {
            wire::StreamLane lane;
            lane.route = binding->get_route();
            lane.ordinal = uint8_t(row->ordinal);
            lane.family = wire::StreamFamily(family);
            streams = streams || send->token_of(p_peer_id, lane) != 0;
        }
        if (!streams) {
            return false;
        }
    }
    return true;
}

void ReplicationCore::send_claim_images(int64_t p_tick) {
    if (session == nullptr || claim_imaging.is_empty()) {
        return;
    }
    LocalVector<ObjectID> held;
    held.reserve(claim_imaging.size());
    for (uint32_t at = 0; at < claim_imaging.size(); ++at) {
        held.push_back(claim_imaging[at]);
    }
    claim_imaging.clear();
    for (uint32_t at = 0; at < held.size(); ++at) {
        NetwEntity *entity
            = Object::cast_to<NetwEntity>(gd::object_of(held[at]));
        if (entity == nullptr
            || !session->control_sends_claim_image(
                entity->get_rid_handle(),
                p_tick
            )) {
            continue;
        }
        const Ref<NetwEntity> view(entity);
        const bool ahead = entity->is_claim_running_ahead();
        const PackedInt32Array holders
            = session->rpc_recipients(entity->get_rid_handle());
        PackedInt32Array owed;
        for (int which = 0; which < holders.size(); ++which) {
            if (ahead || !lanes_stream_to(view, holders[which])) {
                owed.push_back(holders[which]);
            }
        }
        if (owed.is_empty()) {
            continue;
        }
        watch_claim_image(view);
        const PackedByteArray payload
            = session->control_claim_image(entity->get_rid_handle(), p_tick);
        if (payload.is_empty()) {
            continue;
        }
        ReplicationSend *send = sync_pipeline.row_send();
        for (int which = 0; which < owed.size(); ++which) {
            const repl::PeerAccount *account
                = send != nullptr ? send->account(owed[which]) : nullptr;
            if (account != nullptr
                && account->balance < double(payload.size())) {
                continue;
            }
            send_to(
                owed[which],
                entity->get_route(),
                wire::CHANNEL_CLAIM_IMAGE,
                payload,
                false,
                0,
                String()
            );
        }
    }
}

void ReplicationCore::expire_control(int64_t p_tick) {
    if (session == nullptr || control_waiting.is_empty()) {
        return;
    }
    const int64_t deadline = clock_engine->get_tickrate();
    LocalVector<ObjectID> held;
    held.reserve(control_waiting.size());
    for (uint32_t at = 0; at < control_waiting.size(); ++at) {
        held.push_back(control_waiting[at]);
    }
    control_waiting.clear();
    for (uint32_t at = 0; at < held.size(); ++at) {
        const Ref<NetwEntity> entity(
            Object::cast_to<NetwEntity>(gd::object_of(held[at]))
        );
        if (entity.is_valid()
            && session->control_expire(
                entity->get_rid_handle(),
                p_tick,
                deadline
            )) {
            watch_control(entity);
        }
    }
}

bool ReplicationCore::is_live_for(
    int64_t p_peer_id,
    const Ref<NetwEntity> &p_entity
) {
    if (session == nullptr) {
        return true;
    }
    return session->send_admits(
        p_peer_id,
        p_entity.is_valid() ? p_entity->get_rid_handle() : RID()
    );
}

bool ReplicationCore::policy_admits(
    int64_t p_policy,
    int64_t p_sender,
    Node *p_node,
    const Ref<NetwEntity> &p_entity
) {
    Ref<NetwEntity> resolved = p_entity;
    if (resolved.is_null()) {
        resolved = NetwEntity::of(p_node);
    }
    return entity::Control::policy_admits(
        p_policy,
        p_sender,
        p_node != nullptr ? p_node->get_multiplayer_authority() : 0,
        resolved.is_valid() ? resolved->steering() : 0
    );
}

PackedInt32Array ReplicationCore::live_peers(const Ref<NetwEntity> &p_entity) {
    if (session == nullptr) {
        return PackedInt32Array();
    }
    return session->rpc_recipients(
        p_entity.is_valid() ? p_entity->get_rid_handle() : RID()
    );
}

void ReplicationCore::request_control(
    const Ref<NetwEntity> &p_entity,
    const session::ControlRequest &p_request
) {
    if (session == nullptr) {
        return;
    }
    send_to(
        SERVER_PEER,
        p_entity->get_route(),
        wire::CHANNEL_CONTROL_REQUEST,
        session::frame_write(p_request),
        control_requests_ordered,
        0,
        String()
    );
}

void ReplicationCore::flush_all_buffers() {
    if (session == nullptr) {
        return;
    }
    session->carrier_flush(session->session_elapsed_ms(), upkeep_rides());
}

bool ReplicationCore::upkeep_rides() const {
    return clock_engine->get_configured() && !clock_engine->is_held();
}

void ReplicationCore::flush_echoes() {
    if (session != nullptr) {
        session->flush_standalone_acks(
            session->session_elapsed_ms(),
            upkeep_rides()
        );
    }
}

void ReplicationCore::note_staged(int64_t p_peer_id, int64_t p_seq) {
    if (session != nullptr) {
        session->note_sent(p_peer_id, p_seq);
    }
}

Error ReplicationCore::receive_carrier(
    const PackedByteArray &p_framed,
    int64_t p_sender,
    bool p_reliable,
    int64_t p_seq,
    int64_t p_base_tick
) {
    if (p_framed.is_empty()) {
        return OK;
    }
    sync_pipeline.open_datagram(p_base_tick, p_reliable ? -1 : p_seq);
    wire::ReadStream reader(p_framed);
    Error result = OK;
    int64_t frames = 0;
    while (reader.bits_remaining() > 0) {
        const int64_t before = reader.bit_length();
        wire::Frame frame;
        if (!wire::frame_unpack_next(reader, frame)) {
            return finish_receive_pass(frames, ERR_INVALID_DATA, p_sender);
        }
        frames += 1;
        const int64_t frame_bytes = (reader.bit_length() - before) / 8;
#if defined(DEBUG_ENABLED)
        const bool loopback
            = session != nullptr && p_sender == session->get_unique_id();
        const uint32_t provisional = session == nullptr
            ? wire::NetworkProfiler::NONE
            : handler_names_arrival(frame.channel)
            ? network_profiler->aggregate(frame.channel)
            : session->profiler_frame_subject(
                  frame.route,
                  frame.comp,
                  frame.channel,
                  frame.path
              );
        wire::ProfileArrivalScope observation(
            session != nullptr ? network_profiler : nullptr,
            !loopback,
            p_sender,
            frame_bytes,
            provisional
        );
#endif
        const Error verdict = dispatch(
            frame.route,
            int64_t(frame.comp),
            int64_t(frame.channel),
            frame.payload,
            frame.path,
            p_sender,
            p_reliable,
            p_seq,
            frame_bytes
        );
        if (result == OK && verdict != OK) {
            result = verdict;
        }
    }
    return finish_receive_pass(frames, result, p_sender);
}

Error ReplicationCore::finish_receive_pass(
    int64_t p_frames,
    Error p_verdict,
    int64_t p_sender
) {
    sync_pipeline.close_datagram();
    if (session != nullptr) {
        session->event_record(
            EventPlane::APPLY,
            RID(),
            0,
            p_sender,
            p_verdict,
            p_frames
        );
    }
    return p_verdict;
}

Error ReplicationCore::dispatch(
    int64_t p_route,
    int64_t p_comp,
    int64_t p_channel,
    const PackedByteArray &p_payload,
    const String &p_path,
    int64_t p_sender,
    bool p_reliable,
    int64_t p_seq,
    int64_t p_frame_bytes
) {
    if (session == nullptr) {
        return ERR_UNAVAILABLE;
    }
    if (p_sender != session->get_unique_id()) {
        session->wire().observe_in(
            p_sender,
            p_channel,
            p_route,
            p_comp,
            p_frame_bytes >= 0 ? p_frame_bytes : p_payload.size()
        );
    }
    if (p_route != 0) {
        const Error admission
            = admit_entity_frame(p_sender, p_route, p_channel, p_payload);
        if (admission != OK) {
            session->repl_note_gate_verdict(admission);
            session->wire().attribution.note_refusal(
                p_sender,
                p_channel,
                wire::Refusal::GATE
            );
            return admission;
        }
    }
    const int64_t previous = session->rpc_get_relay_sender();
    session->set_relay_sender(p_sender);
    dispatch_frame(
        p_route,
        p_comp,
        p_channel,
        p_payload,
        p_path,
        p_sender,
        p_reliable,
        p_seq
    );
    session->set_relay_sender(previous);
    return OK;
}

void ReplicationCore::settle_reply(
    int64_t p_txn,
    int64_t p_sender,
    int64_t p_route,
    int64_t p_comp,
    const String &p_path
) {
    if (session == nullptr) {
        return;
    }
    const Ref<NetwEntity> entity = session->wrapper_for_route(p_route);
    Node *node = entity.is_valid() ? resolve_comp_node(entity, p_comp, p_path)
                                   : nullptr;
    session->rpc_handle_reply(p_txn, p_sender, node);
}

void ReplicationCore::dispatch_reply(
    const PackedByteArray &p_payload,
    int64_t p_sender
) {
    if (session == nullptr) {
        return;
    }
    wire::ReadStream reader(p_payload);
    uint64_t staged = 0;
    bool failed = false;
    if (!reader.varuint(staged, 5) || !reader.bool1(failed)) {
        return;
    }
    const int64_t txn = int64_t(staged);
    if (failed) {
        uint64_t code = 0;
        String detail;
        if (!reader.varuint(code, 2) || !wire::string_field(reader, detail)
            || !reader.align_verify() || reader.bits_remaining() != 0) {
            return;
        }
        NETW_NETWORK_PROFILE(network_profiler->name_arrival(
            network_profiler->take_reply(p_sender, txn, false)
        ));
        session->rpc_handle_reply_failed(txn, p_sender, Error(code), detail);
        return;
    }
    LocalVector<call_args::Slot> slots;
    if (!call_args::read(reader, Array(), Array(), slots)
        || !reader.align_verify() || reader.bits_remaining() != 0) {
        return;
    }
    NETW_NETWORK_PROFILE(network_profiler->name_arrival(
        network_profiler->take_reply(p_sender, txn, false)
    ));
    if (slots.is_empty() || !slots[0].addresses_node) {
        session->rpc_handle_reply(
            txn,
            p_sender,
            slots.is_empty() ? Variant() : slots[0].value
        );
        return;
    }
    const call_args::NodeRef &node = slots[0].node;
    if (node.route > 0
        && session->liveness_route_state(node.route)
            == NetwMultiplayer::ENTITY_STATE_UNKNOWN) {
        session->liveness_when_live(
            node.route,
            callable_mp(session, &NetwMultiplayer::replication_settle_reply)
                .bind(txn, p_sender, node.route, node.comp, node.path),
            0,
            Callable()
        );
        return;
    }
    settle_reply(txn, p_sender, node.route, node.comp, node.path);
}

void ReplicationCore::receive_protocol(
    NetwMultiplayer *p_session,
    wire::Channel p_channel,
    const PackedByteArray &p_payload,
    int64_t p_sender
) {
    switch (p_channel) {
        case wire::CHANNEL_TABLE:
            return p_session->replication_handle_table_frame(
                p_payload,
                p_sender
            );
        case wire::CHANNEL_SPAWN:
            return p_session->spawn_handle_spawn_frame(p_payload, p_sender);
        case wire::CHANNEL_DESPAWN:
            return p_session->spawn_handle_despawn_frame(p_payload, p_sender);
        case wire::CHANNEL_HIDE:
            return p_session->spawn_handle_hide_frame(p_payload, p_sender);
        case wire::CHANNEL_REPARENT:
            return p_session->spawn_handle_reparent_frame(p_payload, p_sender);
        case wire::CHANNEL_CLOCK_HANDSHAKE:
            return p_session->clock_receive_handshake(p_payload, p_sender);
        case wire::CHANNEL_CLOCK_HANDSHAKE_REPLY:
            return p_session->clock_receive_handshake_reply(
                p_payload,
                p_sender
            );
        case wire::CHANNEL_CLOCK_PING:
            return p_session->clock_receive_ping(p_payload, p_sender);
        case wire::CHANNEL_CLOCK_PONG:
            return p_session->clock_receive_pong(p_payload, p_sender);
        case wire::CHANNEL_INTEREST_AWARENESS:
            return p_session->interest_receive_awareness(p_payload, p_sender);
        case wire::CHANNEL_LAGCOMP_DENY:
            return p_session->handle_deny(p_payload, p_sender);
        case wire::CHANNEL_LAGCOMP_CONFIRM:
            return p_session->handle_confirm(p_payload, p_sender);
        case wire::CHANNEL_ROW_CONTROL:
            return p_session->row_control_receive(p_payload, p_sender);
        case wire::CHANNEL_SESSION_SCENE_RELEASED:
            return p_session->scene_receive_released_frame(
                p_payload,
                int(p_sender)
            );
        case wire::CHANNEL_SESSION_SPAWN_FAILED:
            return p_session->spawn_receive_failed_frame(
                p_payload,
                int(p_sender)
            );
        case wire::CHANNEL_SESSION_SCENE_VIEWERS:
            return p_session->scene_receive_viewers_frame(
                p_payload,
                int(p_sender)
            );
        case wire::CHANNEL_SESSION_JOIN:
            return p_session->session_receive_join(p_payload, p_sender);
        case wire::CHANNEL_SESSION_ACCEPT:
            return p_session->session_receive_accept(p_payload, p_sender);
        case wire::CHANNEL_SESSION_ROSTER:
            return p_session->session_receive_roster(p_payload, p_sender);
        case wire::CHANNEL_SESSION_RESYNC:
            return p_session->session_receive_resync(p_payload, p_sender);
        case wire::CHANNEL_ROUTE_LEASE:
            return p_session->liveness_receive_lease(p_payload, p_sender);
        case wire::CHANNEL_LIFECYCLE_DECISION:
            return p_session->lifecycle_receive_decision(p_payload, p_sender);
        case wire::CHANNEL_SESSION_PAUSE:
        case wire::CHANNEL_SESSION_UNPAUSE:
        case wire::CHANNEL_SESSION_KICKED:
        case wire::CHANNEL_SESSION_SHUTDOWN:
        case wire::CHANNEL_SESSION_KICK_REQUEST:
        case wire::CHANNEL_SESSION_LEAVE:
        case wire::CHANNEL_SESSION_PLAYER_LEFT:
            return p_session
                ->session_receive_control(p_payload, p_sender, p_channel);
        case wire::CHANNEL_ACTION:
        case wire::CHANNEL_CALL:
        case wire::CHANNEL_REPLY:
        case wire::CHANNEL_SIGNAL:
        case wire::CHANNEL_PROPERTY_SYNC:
        case wire::CHANNEL_SYNC:
        case wire::CHANNEL_SYNC_DELTA:
        case wire::CHANNEL_CONTROL_REQUEST:
        case wire::CHANNEL_CONTROL_APPLY:
        case wire::CHANNEL_PREDICT_COMMAND:
        case wire::CHANNEL_PREDICT_ACK:
        case wire::CHANNEL_PREDICT_RELAY:
        case wire::CHANNEL_PREDICT_RELAY_REQUEST:
        case wire::CHANNEL_SYNC_ROW:
        case wire::CHANNEL_SYNC_ROW_DELTA:
        case wire::CHANNEL_SYNC_ROW_WINDOW:
        case wire::CHANNEL_CLAIM_IMAGE:
            return;
    }
}

void ReplicationCore::dispatch_frame(
    int64_t p_route,
    int64_t p_comp,
    int64_t p_channel,
    const PackedByteArray &p_payload,
    const String &p_path,
    int64_t p_sender,
    bool p_reliable,
    int64_t p_seq
) {
    if (session == nullptr || channels == nullptr) {
        return;
    }

    if (p_route == 0) {
        if (p_channel >= USER_CHANNEL_FIRST && p_channel <= USER_CHANNEL_LAST) {
            const Callable handler = channels->handler_of(p_channel);
            if (handler.is_valid()) {
                handler.call(RID(), p_payload, p_sender);
            }
            return;
        }
        receive_protocol(
            session,
            wire::Channel(p_channel),
            p_payload,
            p_sender
        );
        return;
    }

    if (p_seq >= 0 && !channel_addresses_a_set(p_channel)
        && !sync_pipeline
                .accept_unreliable(p_sender, p_route, p_channel, p_seq)) {
        session->wire().attribution.note_refusal(
            p_sender,
            p_channel,
            wire::Refusal::STALE
        );
        return;
    }

    if (p_channel == wire::CHANNEL_REPLY) {
        dispatch_reply(p_payload, p_sender);
        return;
    }

    if (session->liveness_route_state(p_route)
        == NetwMultiplayer::ENTITY_STATE_UNKNOWN) {
        if (defers_unknown_route(p_channel, p_reliable)) {
            session->rpc_defer_call(
                p_sender,
                p_route,
                callable_mp(session, &NetwMultiplayer::replication_dispatch)
                    .bind(
                        p_route,
                        p_comp,
                        p_channel,
                        p_payload,
                        p_path,
                        p_sender,
                        p_reliable,
                        int64_t(-1)
                    )
            );
            return;
        }
        session->repl_note_unknown_route();
        session->wire().attribution.note_refusal(
            p_sender,
            p_channel,
            wire::Refusal::UNROUTED
        );
        return;
    }

    if (channel_addresses_a_set(p_channel)) {
        const Ref<NetwEntity> set_owner = session->wrapper_for_route(p_route);
        if (set_owner.is_null()
            || session->repl_comp_node(p_route, 0, String()) == nullptr) {
            session->wire().attribution.note_refusal(
                p_sender,
                p_channel,
                wire::Refusal::UNBOUND
            );
            return;
        }
        receive_routed(
            session,
            wire::Channel(p_channel),
            set_owner,
            nullptr,
            p_comp,
            p_payload,
            p_sender
        );
        return;
    }

    Node *comp_node
        = session->repl_comp_node(p_route, p_comp, p_path, p_sender);
    if (comp_node == nullptr) {
        if (p_channel == wire::CHANNEL_CALL) {
            session->rpc_refuse_unrouted_call(p_sender, p_route, p_payload);
        }
        return;
    }
    const Ref<NetwEntity> entity = session->wrapper_for_route(p_route);

    if (p_channel >= USER_CHANNEL_FIRST && p_channel <= USER_CHANNEL_LAST) {
        const Callable handler = channels->handler_of(p_channel);
        if (handler.is_valid()) {
            handler.call(
                entity.is_valid() ? entity->get_rid_handle() : RID(),
                p_payload,
                p_sender
            );
        }
        return;
    }
    receive_routed(
        session,
        wire::Channel(p_channel),
        entity,
        comp_node,
        p_comp,
        p_payload,
        p_sender
    );
}

void ReplicationCore::receive_routed(
    NetwMultiplayer *p_session,
    wire::Channel p_channel,
    const Ref<NetwEntity> &p_entity,
    Node *p_node,
    int64_t p_comp,
    const PackedByteArray &p_payload,
    int64_t p_sender
) {
    const RID handle = p_entity.is_valid() ? p_entity->get_rid_handle() : RID();
    switch (p_channel) {
        case wire::CHANNEL_SYNC_ROW:
            return sync_pipeline.handle_derived_row(
                handle,
                p_comp,
                p_payload,
                p_sender,
                p_channel
            );
        case wire::CHANNEL_SYNC_ROW_DELTA:
            return sync_pipeline.handle_retained_row(
                handle,
                p_comp,
                p_payload,
                p_sender,
                p_channel
            );
        case wire::CHANNEL_SYNC_ROW_WINDOW:
            return sync_pipeline.handle_window_row(
                handle,
                p_comp,
                p_payload,
                p_sender,
                p_channel
            );
        case wire::CHANNEL_CALL:
            return p_session->rpc_handle_call(
                p_entity.is_valid() ? p_entity->get_rid_handle() : RID(),
                p_node,
                p_payload,
                p_sender
            );
        case wire::CHANNEL_CONTROL_REQUEST: {
            session::ControlRequest request;
            if (p_entity.is_valid()
                && session::frame_read(p_payload, request)) {
                p_session->control_receive_request(
                    p_entity->get_rid_handle(),
                    p_sender,
                    request
                );
            }
            return;
        }
        case wire::CHANNEL_CONTROL_APPLY: {
            session::ControlApply applied;
            if (p_entity.is_valid() && p_sender == SERVER_PEER
                && session::frame_read(p_payload, applied)) {
                p_session->control_receive_apply(
                    p_entity->get_rid_handle(),
                    applied
                );
            }
            return;
        }
        case wire::CHANNEL_CLAIM_IMAGE: {
            session::ClaimImage image;
            if (p_entity.is_valid() && session::frame_read(p_payload, image)) {
                p_session->control_receive_claim_image(
                    p_entity->get_rid_handle(),
                    p_sender,
                    image
                );
            }
            return;
        }
        case wire::CHANNEL_PROPERTY_SYNC:
            return sync_pipeline.handle_property_sync(
                handle,
                p_node,
                p_payload,
                p_sender,
                p_comp
            );
        case wire::CHANNEL_SIGNAL:
            return sync_pipeline
                .handle_signal(handle, p_node, p_payload, p_sender);
        case wire::CHANNEL_SYNC:
            return sync_compat.handle_sync(handle, p_payload, p_sender);
        case wire::CHANNEL_SYNC_DELTA:
            return sync_compat.handle_sync_delta(handle, p_payload, p_sender);
        case wire::CHANNEL_PREDICT_COMMAND:
            if (p_session->lagcomp_is_wired()) {
                p_session->predict_receive_command_carrier(
                    handle,
                    p_payload,
                    p_sender
                );
            }
            return;
        case wire::CHANNEL_PREDICT_ACK:
            if (p_session->lagcomp_is_wired()) {
                p_session
                    ->predict_receive_ack_carrier(handle, p_payload, p_sender);
            }
            return;
        case wire::CHANNEL_PREDICT_RELAY:
            if (p_session->lagcomp_is_wired()) {
                p_session->predict_receive_relay_carrier(
                    handle,
                    p_payload,
                    p_sender
                );
            }
            return;
        case wire::CHANNEL_PREDICT_RELAY_REQUEST:
            if (p_session->lagcomp_is_wired()) {
                p_session->predict_receive_relay_request_carrier(
                    handle,
                    p_payload,
                    p_sender
                );
            }
            return;
        case wire::CHANNEL_ACTION:
            if (p_session->lagcomp_is_wired()) {
                p_session->action_receive_carrier(handle, p_payload, p_sender);
            }
            return;
        case wire::CHANNEL_REPLY:
        case wire::CHANNEL_INTEREST_AWARENESS:
        case wire::CHANNEL_CLOCK_HANDSHAKE:
        case wire::CHANNEL_CLOCK_HANDSHAKE_REPLY:
        case wire::CHANNEL_CLOCK_PING:
        case wire::CHANNEL_CLOCK_PONG:
        case wire::CHANNEL_LAGCOMP_DENY:
        case wire::CHANNEL_LAGCOMP_CONFIRM:
        case wire::CHANNEL_SPAWN:
        case wire::CHANNEL_DESPAWN:
        case wire::CHANNEL_REPARENT:
        case wire::CHANNEL_TABLE:
        case wire::CHANNEL_HIDE:
        case wire::CHANNEL_ROW_CONTROL:
        case wire::CHANNEL_SESSION_JOIN:
        case wire::CHANNEL_SESSION_ACCEPT:
        case wire::CHANNEL_SESSION_ROSTER:
        case wire::CHANNEL_SESSION_PAUSE:
        case wire::CHANNEL_SESSION_UNPAUSE:
        case wire::CHANNEL_SESSION_KICKED:
        case wire::CHANNEL_SESSION_SHUTDOWN:
        case wire::CHANNEL_SESSION_SCENE_RELEASED:
        case wire::CHANNEL_SESSION_KICK_REQUEST:
        case wire::CHANNEL_SESSION_SCENE_VIEWERS:
        case wire::CHANNEL_ROUTE_LEASE:
        case wire::CHANNEL_LIFECYCLE_DECISION:
        case wire::CHANNEL_SESSION_RESYNC:
        case wire::CHANNEL_SESSION_LEAVE:
        case wire::CHANNEL_SESSION_PLAYER_LEFT:
        case wire::CHANNEL_SESSION_SPAWN_FAILED:
            return;
    }
}

bool ReplicationCore::channel_addresses_a_set(int64_t p_channel) const {
    return p_channel == wire::CHANNEL_SYNC_ROW
        || p_channel == wire::CHANNEL_SYNC_ROW_DELTA
        || p_channel == wire::CHANNEL_SYNC_ROW_WINDOW;
}

#if defined(DEBUG_ENABLED)
bool ReplicationCore::handler_names_arrival(int64_t p_channel) const {
    return p_channel == wire::CHANNEL_CALL || p_channel == wire::CHANNEL_REPLY
        || p_channel == wire::CHANNEL_SIGNAL
        || p_channel == wire::CHANNEL_PROPERTY_SYNC
        || p_channel == wire::CHANNEL_SYNC
        || p_channel == wire::CHANNEL_SYNC_DELTA
        || p_channel == wire::CHANNEL_ACTION;
}
#endif

bool ReplicationCore::defers_unknown_route(
    int64_t p_channel,
    bool p_reliable
) const {
    if (p_channel == wire::CHANNEL_CALL
        || p_channel == wire::CHANNEL_CONTROL_APPLY) {
        return p_reliable;
    }
    return channels != nullptr ? channels->defers(p_channel) : false;
}

Error ReplicationCore::admit_entity_frame(
    int64_t p_sender,
    int64_t p_route,
    int64_t p_channel,
    const PackedByteArray &p_payload
) {
    if (session == nullptr) {
        return ERR_UNAVAILABLE;
    }
    int64_t gate = -1;
    Error verdict = OK;
    if (p_channel == wire::CHANNEL_SYNC || p_channel == wire::CHANNEL_SYNC_ROW
        || p_channel == wire::CHANNEL_SYNC_ROW_DELTA
        || p_channel == wire::CHANNEL_SYNC_ROW_WINDOW
        || p_channel == wire::CHANNEL_SYNC_DELTA) {
        gate = EventPlane::GATE_SYNC;
        verdict = session->sync_admit_frame(p_route, p_channel, p_payload);
    } else if (
        p_channel == wire::CHANNEL_PREDICT_COMMAND
        || p_channel == wire::CHANNEL_PREDICT_ACK
        || p_channel == wire::CHANNEL_PREDICT_RELAY
        || p_channel == wire::CHANNEL_PREDICT_RELAY_REQUEST
    ) {
        gate = EventPlane::GATE_PREDICT;
        verdict = session->predict_admit_frame(
            p_sender,
            p_route,
            p_channel,
            p_payload
        );
    }
    if (gate < 0) {
        return OK;
    }
    return session->finish_stage_verdict(gate, verdict, p_route, p_sender);
}

Node *ReplicationCore::resolve_comp_node(
    const Ref<NetwEntity> &p_entity,
    int64_t p_comp,
    const String &p_path
) {
    if (p_entity.is_null()) {
        return nullptr;
    }
    return p_entity->comp_table()
        .resolve_node(p_entity->get_owner(), p_comp, p_path);
}

void ReplicationCore::on_clock_tick(int64_t p_tick) {
    expire_control(p_tick);
    send_table_removals(p_tick);
    sync_pipeline.pump(p_tick);
    send_claim_images(p_tick);
    sync_compat.pump();
    if (session != nullptr) {
#if defined(NETW_TESTS)
        session->spawn_place_held();
#endif
        session->spawn_retry_adopt();
    }
    {
        NETW_ZONE_NC("Table pump", colors::TABLE);
        pump_tables(p_tick);
    }
    if (session != nullptr && session->lagcomp_is_configured()
        && clock_engine->is_closing_tick()) {
        return;
    }
    if (!clock_engine->is_send_tick()) {
        return;
    }
    flush_all_buffers();
    flush_echoes();
    clock_engine->mark_sent();
}

void ReplicationCore::pump_tables(int64_t p_tick) {
    (void)p_tick;
    if (session == nullptr) {
        return;
    }
    session->table_announce_intake();

    if (!session->table_publishes() || session->session_get_inner().is_null()
        || session->session_get_inner()->get_multiplayer_peer().is_null()) {
        return;
    }
    store::Arrays &arrays = *store_world->arrays;
    const TypedArray<RID> dirty = arrays.dirty_tables();
    for (int at = 0; at < dirty.size(); ++at) {
        arrays.clear_dirty(dirty[at]);
        session->table_announce(dirty[at]);
    }
}

void ReplicationCore::send_table_removals(int64_t p_tick) {
    if (session == nullptr || !session->table_publishes()
        || session->session_get_inner().is_null()
        || session->session_get_inner()->get_multiplayer_peer().is_null()) {
        return;
    }
    store::Arrays &arrays = *store_world->arrays;
    const int64_t budget = session->datagram_budget();
    const PackedInt64Array retired = arrays.take_lifecycle_removals();
    if (!retired.is_empty()) {
        broadcast_table_frames(
            store::Arrays::encode_lifecycle(retired, p_tick, budget),
            true
        );
    }
    const TypedArray<RID> dirty = arrays.dirty_tables();
    if (dirty.is_empty()) {
        return;
    }
    const PackedInt32Array peers
        = gd::api_peer_ids(session->session_get_inner());
    arrays.forget_peers_except(peers);
    for (int at = 0; at < dirty.size(); ++at) {
        const RID id = dirty[at];
        const PackedInt64Array gone = arrays.take_pending_removals(id);
        if (!gone.is_empty()) {
            broadcast_table_frames(
                arrays.encode_removal(id, gone, p_tick, budget),
                true
            );
        }
    }
}

void ReplicationCore::broadcast_table_frames(
    const TypedArray<PackedByteArray> &p_frames,
    bool p_reliable
) {
    if (p_frames.is_empty() || session == nullptr) {
        return;
    }
    const PackedInt32Array peers
        = gd::api_peer_ids(session->session_get_inner());
    for (int peer = 0; peer < peers.size(); ++peer) {
        for (int at = 0; at < p_frames.size(); ++at) {
            send_to(
                peers[peer],
                0,
                wire::CHANNEL_TABLE,
                p_frames[at],
                p_reliable,
                0,
                String()
            );
        }
    }
}

void ReplicationCore::handle_table_frame(
    const PackedByteArray &p_payload,
    int64_t p_sender
) {
    if (session == nullptr) {
        return;
    }
    const Error verdict
        = session->table_admit_frame(p_sender, wire::CHANNEL_TABLE, p_payload);
    if (session
            ->finish_stage_verdict(EventPlane::GATE_TABLE, verdict, 0, p_sender)
        != OK) {
        return;
    }
    store::Arrays &arrays = *store_world->arrays;
    const store::ArrayLanding landed = arrays.apply_frame(p_payload);
    if (!landed.bound.is_empty()) {
        session->liveness_bind_routes_data(landed.bound);
    }
    if (!landed.retired.is_empty()) {
        session->liveness_tombstone_routes_data(landed.retired);
    }
}

void ReplicationCore::replay_tables(int64_t p_peer_id) {
    if (session == nullptr || !session->table_publishes()
        || session->session_get_inner().is_null()
        || session->session_get_inner()->get_multiplayer_peer().is_null()) {
        return;
    }
    store::Arrays &arrays = *store_world->arrays;
    const int64_t budget = session->datagram_budget();
    const TypedArray<RID> published = arrays.published_tables();
    for (int at = 0; at < published.size(); ++at) {
        const TypedArray<PackedByteArray> frames
            = arrays.encode_snapshot(published[at], int(p_peer_id), budget);
        for (int frame = 0; frame < frames.size(); ++frame) {
            send_to(
                p_peer_id,
                0,
                wire::CHANNEL_TABLE,
                frames[frame],
                true,
                0,
                String()
            );
        }
    }
}

void ReplicationCore::on_frame_end() {
    if (!clock_engine->is_send_tick()) {
        return;
    }
    flush_all_buffers();
    if (clock_engine->closed_a_tick()) {
        flush_echoes();
        clock_engine->mark_sent();
    }
}

void ReplicationCore::on_poll() {
    if (session == nullptr) {
        return;
    }
    if (clock_engine->get_configured()) {
        return;
    }
    if (session->session_is_online() && sync_pipeline.owes_rows()) {
        fault::raise(session, NETW_FAULT_ROWS_WITHOUT_CLOCK, {});
    }
    sync_compat.pump();
    session->spawn_retry_adopt();
    flush_all_buffers();
    flush_echoes();
}

void ReplicationCore::clear_session() {
    if (session != nullptr) {
        session->carrier_clear();
    }
    sync_model.clear();
    sync_pipeline.clear_session();
    spawn_pipeline.begin_clear_session();
    clear_spawn_parks();
    release_spawn_builds();
    spawn_pipeline.finish_clear_session();
    sync_compat.clear_session();
}

void ReplicationCore::clear_spawn_parks() {
    const LocalVector<int64_t> parked = spawn_pipeline.begin_clear_parks();
    for (const int64_t route : parked) {
        release_spawn_route(route);
    }
    spawn_pipeline.finish_clear_parks();
}

void ReplicationCore::release_spawn_route(int64_t p_route) {
    spawn::Ops ops;
    spawn_pipeline.release_build(p_route, ops);
    act_spawn_rows(ops);
}

void ReplicationCore::clear_route(int64_t p_route) {
    sync_model.clear_route(p_route);
    sync_pipeline.clear_route(p_route);
    spawn::Ops ops;
    spawn_pipeline.clear_route(p_route, ops);
    act_spawn_rows(ops);
    sync_compat.clear_route(p_route);
}

void ReplicationCore::release_spawn_builds() {
    const LocalVector<int64_t> routes = spawn_pipeline.build_routes();
    for (const int64_t route : routes) {
        release_spawn_route(route);
    }
}

void ReplicationCore::act_spawn_rows(spawn::Ops &r_ops) {
    if (session != nullptr) {
        session->spawn_act(r_ops);
        return;
    }
    for (; r_ops.acted < r_ops.rows.size(); ++r_ops.acted) {
        NetwMultiplayer::free_returned(
            Variant(gd::instance_from_id(r_ops.rows[r_ops.acted].node)),
            Variant()
        );
    }
}

void ReplicationCore::clear_peer(int64_t p_peer_id) {
    sync_pipeline.clear_peer(p_peer_id);
    sync_compat.clear_peer(p_peer_id);
}

void ReplicationCore::dispose() {
    sync_pipeline.dispose();
    sync_compat.dispose();
}

Dictionary ReplicationCore::counters() const {
    const Dictionary drops
        = session != nullptr ? session->repl_drop_stats() : Dictionary();
    Dictionary out;
    out[StringName("drops_unknown_route")]
        = int64_t(drops.get(StringName("drops_unknown_route"), 0));
    out[StringName("drops_not_live")]
        = int64_t(drops.get(StringName("drops_not_live"), 0));
    out[StringName("drops_lingering_route")]
        = int64_t(drops.get(StringName("drops_lingering_route"), 0));
    out[StringName("drops_dead_route")]
        = int64_t(drops.get(StringName("drops_dead_route"), 0));
    out[StringName("drops_no_node")]
        = int64_t(drops.get(StringName("drops_no_node"), 0));
    out[StringName("drops_traversal")]
        = int64_t(drops.get(StringName("drops_traversal"), 0));
    out[StringName("drops_comp_unresolved")]
        = int64_t(drops.get(StringName("drops_comp_unresolved"), 0));
    out.merge(sync_pipeline.counters());
    out.merge(spawn_pipeline.counters());
    out.merge(spawner_compat.counters());
    out.merge(sync_compat.counters());
    return out;
}

void ReplicationCore::register_spawn_constructor(
    const StringName &p_id,
    const Callable &p_fn
) {
    if (session != nullptr) {
        session->spawn_register_constructor(p_id, p_fn);
    }
}

Ref<NetwEntity> ReplicationCore::adopt_in_place(Node *p_root) {
    if (session == nullptr) {
        return Ref<NetwEntity>();
    }
    const Ref<NetwEntity> adopted = session->spawn_adopt_in_place(p_root);
    if (adopted.is_null()) {
        return Ref<NetwEntity>();
    }
    return session->entity_get_view(adopted->get_rid_handle());
}

TypedArray<Dictionary> ReplicationCore::spawn_state_of(Node *p_root) {
    const Ref<NetwEntity> wrapper = NetwEntity::of(p_root);
    if (session != nullptr && wrapper.is_valid()
        && liveness_core->entity_is_valid(wrapper->get_rid_handle())) {
        return session->spawn_get_state(wrapper->get_rid_handle());
    }
    return spawn_pipeline.collect_spawn_state(p_root);
}

bool ReplicationCore::owns_spawned_route(int64_t p_route) {
    return spawn_pipeline.owns_spawned_route(p_route);
}

} // namespace netw
