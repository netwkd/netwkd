#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/scene.hpp"

#include "netw/api/participant.hpp"

#include "godot/class_db.hpp"
#include "godot/engine_debugger.hpp"
#include "godot/node.hpp"
#include "godot/object.hpp"
#include "godot/os.hpp"
#include "godot/packed_scene.hpp"
#include "godot/physics_server.hpp"
#include "godot/rendering_server.hpp"
#include "godot/resource.hpp"
#include "godot/spatial_node.hpp"
#include "godot/time.hpp"
#include "godot/utility.hpp"
#include "godot/viewport.hpp"
#include "godot/world.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/join_request.hpp"
#include "netw/api/prediction.hpp"
#include "netw/api/replication_core.hpp"
#include "netw/api/schema_model.hpp"
#include "netw/colors.hpp"
#include "netw/comp_table.hpp"
#include "netw/fault_book.hpp"
#include "netw/game_call.hpp"
#include "netw/log.hpp"
#include "netw/prediction_core.hpp"
#include "netw/profile.hpp"
#include "netw/repl/sync_pipeline.hpp"
#include "netw/script/model.hpp"
#include "netw/stage.hpp"
#include "netw/synchronizers.hpp"
#include "netw/wire/frame.hpp"
#include "netw/wire/registry.hpp"
#include "netw/wire/spec.hpp"

using namespace godot;
using namespace netw;

namespace netw {

namespace {

const char *SIG_PEER_AUTHENTICATION_FAILED = "peer_authentication_failed";
const char *SIG_PEER_PACKET = "peer_packet";

constexpr int64_t CALL_TARGET_EVERYONE = 0;
constexpr int64_t CALL_TARGET_SERVER = 1;
constexpr int64_t CALL_TARGET_PEER = 2;
constexpr int64_t CALL_AWAITS_REPLY = 4;

PackedByteArray call_frame(
    int64_t p_target_type,
    int64_t p_txn,
    bool p_awaits_reply,
    int64_t p_target_peer,
    const Variant &p_method,
    const LocalVector<call_args::Slot> &p_slots,
    const Array &p_quantizers,
    const Array &p_types,
    NetwMultiplayer *p_session
) {
    wire::WriteStream stream;
    uint64_t flags = uint64_t(p_target_type)
        | uint64_t(p_awaits_reply ? CALL_AWAITS_REPLY : 0);
    if (!stream.bits(flags, 8)) {
        return PackedByteArray();
    }
    if (p_awaits_reply) {
        uint64_t txn = uint64_t(MAX(p_txn, int64_t(0)));
        if (!stream.varuint(txn, 5)) {
            return PackedByteArray();
        }
    }
    if (p_target_type == CALL_TARGET_PEER) {
        uint64_t target = uint64_t(p_target_peer) & 0xFFFFFFFFULL;
        if (!stream.bits(target, 32)) {
            return PackedByteArray();
        }
    }
    if (!script::model::write_call_body(
            stream,
            p_method,
            p_slots,
            p_quantizers,
            p_types,
            p_session
        )
        || !stream.align_verify()) {
        return PackedByteArray();
    }
    return stream.to_bytes();
}

} // namespace

Error NetwMultiplayer::relay_subscribe(
    const RID &p_entity,
    int64_t p_peer,
    bool p_subscribed
) {
    if (!is_host()) {
        return ERR_UNAUTHORIZED;
    }
    const Ref<NetwEntity> pinned = slot_pinned_entity(p_entity);
    if (pinned.is_null()) {
        return ERR_DOES_NOT_EXIST;
    }
    const RID entity = pinned->get_rid_handle();
    if (!liveness_core.entity_is_valid(entity)) {
        return ERR_DOES_NOT_EXIST;
    }
    const int64_t slot = entity.get_id();
    if (!p_subscribed) {
        relay_book.set_subscribed(slot, p_peer, false);
        return OK;
    }
    if (interest_peer_bit(p_peer) < 0) {
        return ERR_UNAUTHORIZED;
    }
    relay_book.set_subscribed(slot, p_peer, true);
    relay_book.audience_changed(slot);
    return OK;
}

uint32_t NetwMultiplayer::relay_generation(int64_t p_entity_slot) const {
    return relay_book.generation(p_entity_slot);
}

PackedInt64Array NetwMultiplayer::relay_peers(int64_t p_entity_slot) const {
    return relay_book.peers(p_entity_slot);
}

int NetwMultiplayer::relay_peer_count(int64_t p_entity_slot) const {
    return relay_book.peer_count(p_entity_slot);
}

void NetwMultiplayer::relay_release(int64_t p_entity_slot) {
    relay_book.release(p_entity_slot);
}

int NetwMultiplayer::relay_request_of(const PackedByteArray &p_payload) const {
    return predict::RelayBook::request_of(p_payload);
}

void NetwMultiplayer::channel_dispatch(
    const RID &p_entity,
    const PackedByteArray &p_payload,
    int64_t p_sender,
    int64_t p_channel
) {
    const ChannelHandler *row = channel_handlers.getptr(p_channel);
    if (row == nullptr || !row->handler.is_valid()) {
        return;
    }
    Array args;
    if (row->sender_first) {
        args.push_back(p_sender);
        args.push_back(p_payload);
        game_call(row->handler, args, game_calls::NETW_CHANNEL_HANDLER);
        return;
    }
    args.push_back(p_entity);
    args.push_back(p_payload);
    args.push_back(p_sender);
    game_call(row->handler, args, game_calls::CHANNEL_HANDLER);
}

void NetwMultiplayer::rpc_channel_register(
    int64_t p_channel,
    const Callable &p_handler,
    bool p_defer_when_unknown
) {
    rpc_channel_install(p_channel, p_handler, p_defer_when_unknown, false);
}

void NetwMultiplayer::rpc_channel_install(
    int64_t p_channel,
    const Callable &p_handler,
    bool p_defer_when_unknown,
    bool p_sender_first
) {
    if (p_channel < CHANNEL_USER_FIRST || p_channel > CHANNEL_USER_LAST) {
        return;
    }
    game_call_declared(
        p_handler,
        p_sender_first ? game_calls::NETW_CHANNEL_HANDLER
                       : game_calls::CHANNEL_HANDLER
    );
    if (!p_handler.is_valid()) {
        return;
    }
    channel_handlers.insert(p_channel, {p_handler, p_sender_first});
    get_channel_book()->register_channel(
        p_channel,
        callable_mp(this, &NetwMultiplayer::channel_dispatch).bind(p_channel),
        p_defer_when_unknown
    );
}

NetwCarrierFrame NetwMultiplayer::receive_header(
    int64_t p_peer,
    const PackedByteArray &p_packet
) {
    capture_note(wire::CaptureDirection::IN, p_peer, p_packet);
    const NetwCarrierFrame header = NetwCarrierFrame::read(p_packet);
    if (header.kind == NetwCarrierFrame::FOREIGN) {
        emit_signal(SIG_PEER_PACKET, p_peer, p_packet);
        return header;
    }
    if (header.kind == NetwCarrierFrame::MALFORMED) {
        if (header.unreadable) {
            fault::raise(
                this,
                NETW_FAULT_MALFORMED_DATAGRAM,
                {String(), String(), String(), p_peer}
            );
        }
        event_record(
            EventPlane::DATAGRAM_MALFORMED,
            RID(),
            0,
            p_peer,
            ERR_INVALID_DATA,
            p_packet.size()
        );
        return header;
    }
    world.wire.count_received(p_packet.size() - header.payload_offset);
    world.wire.attribution.note_datagram(
        true,
        p_peer,
        int64_t(p_packet.size())
    );
#if defined(DEBUG_ENABLED)
    const bool bare = header.payload_offset == p_packet.size();
    if (bare) {
        network_profiler.observe(
            network_profiler.ack_subject(),
            network_profiler.epoch(),
            p_peer,
            true,
            header.payload_offset
        );
    }
    profiler_datagram(true, p_peer, bare ? 0 : header.payload_offset);
#endif
    event_record(
        EventPlane::DATAGRAM_RECEIVED,
        RID(),
        0,
        p_peer,
        OK,
        p_packet.size() - header.payload_offset,
        header.kind == NetwCarrierFrame::RELIABLE ? int64_t(-1) : header.seq
    );
    return header;
}

Callable NetwMultiplayer::relay_for(const StringName &p_signal, int p_arity) {
    switch (p_arity) {
        case 0:
            return callable_mp(this, &NetwMultiplayer::relay_bare)
                .bind(p_signal);
        case 1:
            return callable_mp(this, &NetwMultiplayer::relay_one)
                .bind(p_signal);
        case 2:
            return callable_mp(this, &NetwMultiplayer::relay_two)
                .bind(p_signal);
    }
    fault::raise(
        this,
        NETW_FAULT_BAD_VALUE,
        {vformat("Signal '%s'", String(p_signal)),
         vformat("%d arguments", p_arity),
         "a signal with up to 2 arguments"}
    );
    return Callable();
}

void NetwMultiplayer::relay_from(
    Object *p_source,
    const StringName &p_signal,
    int p_arity
) {
    relay_named_from(p_source, p_signal, p_signal, p_arity);
}

void NetwMultiplayer::relay_named_from(
    Object *p_source,
    const StringName &p_source_signal,
    const StringName &p_own_signal,
    int p_arity
) {
    const Callable relay = relay_for(p_own_signal, p_arity);
    if (relay.is_null()) {
        return;
    }
    p_source->connect(p_source_signal, relay);
}

void NetwMultiplayer::stop_relay_named_from(
    Object *p_source,
    const StringName &p_source_signal,
    const StringName &p_own_signal,
    int p_arity
) {
    const Callable relay = relay_for(p_own_signal, p_arity);
    if (relay.is_null()) {
        return;
    }
    p_source->disconnect(p_source_signal, relay);
}

void NetwMultiplayer::stop_relay_from(
    Object *p_source,
    const StringName &p_signal,
    int p_arity
) {
    const Callable relay = relay_for(p_signal, p_arity);
    if (relay.is_null()) {
        return;
    }
    p_source->disconnect(p_signal, relay);
}

void NetwMultiplayer::relay_bare(const StringName &p_signal) {
    if (installing_transfer) {
        return;
    }
    emit_signal(p_signal);
}

void NetwMultiplayer::relay_one(
    const Variant &p_first,
    const StringName &p_signal
) {
    emit_signal(p_signal, p_first);
}

void NetwMultiplayer::relay_two(
    const Variant &p_first,
    const Variant &p_second,
    const StringName &p_signal
) {
    emit_signal(p_signal, p_first, p_second);
}

void NetwMultiplayer::relay_auth_failed(int64_t p_peer) {
    if (installing_transfer) {
        auth_forget_link(p_peer);
        return;
    }
    event_record(EventPlane::PEER_AUTH_FAILED, RID(), 0, p_peer);
    const LinkHandshake *link = link_handshakes.getptr(p_peer);
    const bool reported = link != nullptr && link->refused;
    auth_forget_link(p_peer);
    emit_signal(SIG_PEER_AUTHENTICATION_FAILED, p_peer);
    if (!probe_forget(p_peer) && !reported) {
        NETW_DEBUG(
            netw::sys::SESSION,
            "Peer %d left before it finished authenticating.",
            int(p_peer)
        );
    }
}

bool NetwMultiplayer::rpc_routes_through_session(
    Object *p_object,
    const StringName &p_method,
    const Array &p_args,
    int64_t p_peer
) {
    Node *node = Object::cast_to<Node>(p_object);
    if (node == nullptr) {
        return false;
    }
    const Ref<NetwEntity> entity = NetwEntity::of(node);
    if (entity.is_null() || liveness_route_of(entity.ptr()) <= 0) {
        return false;
    }
    rpc_call(Callable(node, p_method), p_args, p_peer);
    return true;
}

#if defined(NETW_MODULE)
Error NetwMultiplayer::rpcp(
    Object *p_object,
    int p_peer_id,
    const StringName &p_method,
    const Variant **p_args,
    int p_argcount
) {
    Array args;
    for (int at = 0; at < p_argcount; ++at) {
        args.push_back(*p_args[at]);
    }
    if (rpc_routes_through_session(p_object, p_method, args, p_peer_id)) {
        return OK;
    }
    return inner->rpcp(p_object, p_peer_id, p_method, p_args, p_argcount);
}
#else
Error NetwMultiplayer::_rpc(
    int32_t p_peer_id,
    Object *p_object,
    const StringName &p_method,
    const Array &p_args
) {
    if (rpc_routes_through_session(p_object, p_method, p_args, p_peer_id)) {
        return OK;
    }
    return inner->rpc(p_peer_id, p_object, p_method, p_args);
}
#endif

bool NetwMultiplayer::send_admits(int64_t p_peer_id, const RID &p_entity) {
    if (entity_get_state(p_entity) != ENTITY_STATE_LIVE) {
        return false;
    }
    if (p_peer_id == SERVER_PEER) {
        return true;
    }
    if (!interest_entity_has_filter(p_entity)) {
        return true;
    }
    return interest_wire_admits(p_peer_id, p_entity);
}

PackedInt32Array NetwMultiplayer::send_recipients(
    const RID &p_entity,
    const PackedInt32Array &p_peers
) {
    PackedInt32Array out;
    for (int at = 0; at < p_peers.size(); at++) {
        if (send_admits(int64_t(p_peers[at]), p_entity)) {
            out.push_back(p_peers[at]);
        }
    }
    return out;
}

NetwPredictTiming NetwMultiplayer::frame_timing() const {
    const Clock &clock = clock_engine();
    const double ticktime = clock.ticktime();
    return NetwPredictTiming::of(
        clock.get_tick() - 1,
        ticktime,
        ticktime,
        simulated_frames,
        declared_quantum(),
        clock.is_simulating()
    );
}

namespace {

Dictionary peer_channel_rows(const HashMap<uint64_t, int64_t> &p_table) {
    Dictionary out;
    for (const KeyValue<uint64_t, int64_t> &row : p_table) {
        const int64_t peer = int64_t(int32_t(row.key >> 8));
        const int64_t channel = int64_t(row.key & 0xff);
        Dictionary channels = out.get(peer, Dictionary());
        channels[channel] = row.value;
        out[peer] = channels;
    }
    return out;
}

Dictionary route_rows(const HashMap<int64_t, int64_t> &p_table) {
    Dictionary out;
    for (const KeyValue<int64_t, int64_t> &row : p_table) {
        out[row.key] = row.value;
    }
    return out;
}

Dictionary attribution_rows(const wire::AttributionBook &p_book) {
    Dictionary out;
    out[StringName("bytes_out")] = peer_channel_rows(p_book.bytes_out_rows());
    out[StringName("bytes_in")] = peer_channel_rows(p_book.bytes_in_rows());
    out[StringName("route_bytes_out")] = route_rows(p_book.route_out_rows());
    out[StringName("route_bytes_in")] = route_rows(p_book.route_in_rows());
    out[StringName("attributed_out")] = p_book.get_attributed_out();
    out[StringName("attributed_in")] = p_book.get_attributed_in();
    out[StringName("framing_out")] = p_book.get_framing_out();
    out[StringName("frames_out")] = p_book.get_frames_out();
    out[StringName("frames_in")] = p_book.get_frames_in();
    out[StringName("staged_dropped_out")] = p_book.get_staged_dropped_out();
    out[StringName("frames_in_by_channel")]
        = peer_channel_rows(p_book.frames_in_rows());
    out[StringName("refused_in")] = p_book.get_refused_in();
    Dictionary refused;
    for (const KeyValue<uint64_t, int64_t> &row : p_book.refusal_rows()) {
        const int64_t peer = int64_t(int32_t(row.key >> 16));
        const int64_t channel = int64_t((row.key >> 8) & 0xff);
        const wire::Refusal verdict = wire::Refusal(uint8_t(row.key & 0xff));
        Dictionary channels = refused.get(peer, Dictionary());
        Dictionary verdicts = channels.get(channel, Dictionary());
        verdicts[String(wire::refusal_name(verdict))] = row.value;
        channels[channel] = verdicts;
        refused[peer] = channels;
    }
    out[StringName("refusals")] = refused;
    out[StringName("armed")] = p_book.is_armed();
    Dictionary columns;
    for (const KeyValue<uint64_t, int64_t> &row : p_book.column_rows()) {
        Dictionary cell;
        cell[StringName("schema")] = int64_t(int32_t(row.key >> 16));
        cell[StringName("column")] = int64_t(row.key & 0xffff);
        cell[StringName("bits")] = row.value;
        columns[int64_t(row.key)] = cell;
    }
    out[StringName("columns")] = columns;
    return out;
}

} // namespace

Dictionary NetwMultiplayer::attribution_snapshot() const {
    const wire::AttributionBook &attribution = world.wire.attribution;
    Dictionary out = attribution_rows(attribution);
    const int64_t attributed_out = attribution.get_attributed_out();
    const int64_t framing_out = attribution.get_framing_out();
    const int64_t dropped_out = attribution.get_staged_dropped_out();
    const int64_t attributed_in = attribution.get_attributed_in();

    Dictionary residual;
    const int64_t sent_bytes = world.wire.sent_bytes;
    const int64_t received_bytes = world.wire.received_bytes;
    residual[StringName("datagram_bytes_out")] = sent_bytes;
    residual[StringName("attributed_out")] = attributed_out;
    residual[StringName("framing_out")] = framing_out;
    residual[StringName("staged_dropped_out")] = dropped_out;
    residual[StringName("residual_out")]
        = sent_bytes - attributed_out - framing_out;
    residual[StringName("datagram_bytes_in")] = received_bytes;
    residual[StringName("attributed_in")] = attributed_in;
    residual[StringName("residual_in")] = received_bytes - attributed_in;

    const int64_t wire_out = attribution.get_wire_out();
    const int64_t wire_in = attribution.get_wire_in();
    residual[StringName("wire_bytes_out")] = wire_out;
    residual[StringName("wire_bytes_in")] = wire_in;
    residual[StringName("datagrams_out")] = attribution.get_datagrams_out();
    residual[StringName("datagrams_in")] = attribution.get_datagrams_in();
    residual[StringName("carrier_overhead_out")]
        = wire_out - attributed_out - framing_out;
    residual[StringName("carrier_overhead_in")] = wire_in - attributed_in;
    out[StringName("residual")] = residual;
    return out;
}

Dictionary NetwMultiplayer::capture_header() {
    Dictionary header = wire::spec_document();
    header[StringName("peer")] = int64_t(get_unique_id());
    header[StringName("schemas")] = wire::spec_schemas(schema_core);
    return header;
}

void NetwMultiplayer::capture_note(
    wire::CaptureDirection p_dir,
    int64_t p_peer,
    const PackedByteArray &p_datagram
) {
    if (!capture_decided) {
        capture_decided = true;
        if (capture_in_memory) {
            capture.open_memory(capture_header());
        } else {
            const String path = wire::CaptureWriter::claim_armed_path();
            if (!path.is_empty()) {
                capture.open(path, capture_header());
            }
        }
    }
    if (!capture.is_open()) {
        return;
    }
    capture.note(p_dir, p_peer, clock_engine().get_tick(), p_datagram);
}

void NetwMultiplayer::capture_close() {
    capture.close();
}

int64_t NetwMultiplayer::receive_tick() const {
    return clock_engine().get_configured() ? clock_engine().get_tick()
                                           : frame_counter;
}

int64_t NetwMultiplayer::datagram_budget() const {
    constexpr int64_t HEADROOM = 150;
    constexpr int64_t FLOOR = 128;
    if (inner.is_null()) {
        return FLOOR;
    }
    const int64_t ceiling
        = int64_t(inner->get_max_sync_packet_size()) - HEADROOM;
    return ceiling > FLOOR ? ceiling : FLOOR;
}

PackedByteArray NetwMultiplayer::frame_pack(
    int64_t p_route,
    int64_t p_comp,
    int64_t p_channel,
    const PackedByteArray &p_payload,
    const String &p_path
) {
    return wire::frame_pack(
        p_route,
        uint8_t(p_comp),
        uint8_t(p_channel),
        p_payload,
        p_path
    );
}

void NetwMultiplayer::rpc_note_dropped_unroutable() {
    ++rpc_dropped_unroutable;
}

void NetwMultiplayer::rpc_note_dropped_not_live() {
    ++rpc_dropped_not_live;
}

NetwMultiplayer::CallTarget NetwMultiplayer::call_target(
    Node *p_node,
    const StringName &p_member
) {
    CallTarget out;
    if (p_node == nullptr) {
        out.code = ERR_INVALID_PARAMETER;
        out.detail = String("A Netw RPC needs a method of a Node.");
        fault::raise(
            this,
            NETW_FAULT_BAD_CALLABLE,
            {String("A Netw RPC"), String(), String("a method of a Node")}
        );
        return out;
    }
    const Ref<NetwEntity> wrapper = NetwEntity::of(p_node);
    if (wrapper.is_null()) {
        out.code = ERR_UNCONFIGURED;
        out.detail = vformat(
            "'%s' is not part of a NetwEntity, so the call to '%s' was "
            "dropped. Call Netw.configure_entity on its root.",
            String(p_node->get_name()),
            String(p_member)
        );
        fault::raise(
            this,
            NETW_FAULT_NOT_ENTITY,
            {String("The call to '") + String(p_member) + String("'"),
             String(p_node->get_name())},
            gd::instance_id(p_node)
        );
        return out;
    }
    out.entity = wrapper->get_rid_handle();
    out.record = wrapper->get_record();
    out.owner = wrapper->get_owner();
    out.route = liveness_route_of(wrapper.ptr());
    if (out.route <= 0) {
        out.code = ERR_UNAVAILABLE;
        out.detail = vformat(
            "'%s' is not spawned on the network yet, so the call to '%s' was "
            "dropped. Call it from _ready or later.",
            String(p_node->get_name()),
            String(p_member)
        );
        fault::raise(
            this,
            NETW_FAULT_NOT_RUNNING,
            {String("The call to '") + String(p_member) + String("'"),
             String(),
             String("'") + String(p_node->get_name()) + String("'")},
            gd::instance_id(p_node)
        );
    }
    return out;
}

NetwMultiplayer::CallTarget NetwMultiplayer::rpc_target(
    Node *p_node,
    const StringName &p_method
) {
    const CallTarget target = call_target(p_node, p_method);
    if (target.code == ERR_UNAVAILABLE) {
        ++rpc_dropped_not_live;
    } else if (target.code != OK) {
        ++rpc_dropped_unroutable;
    }
    return target;
}

Error NetwMultiplayer::rpc_declared(
    Node *p_node,
    const StringName &p_method,
    String &r_detail
) {
    const Ref<Script> script = p_node->get_script();
    if (netw::script::model::get_rpc_options(script, p_method).is_valid()
        || (script.is_valid()
            && Dictionary(script->get_rpc_config()).has(p_method))) {
        return OK;
    }
    r_detail = vformat(
        "Method '%s' on '%s' is not an RPC. Declare it with "
        "Netw.configure_rpc or @rpc.",
        String(p_method),
        String(p_node->get_name())
    );
    fault::raise(
        this,
        NETW_FAULT_UNKNOWN_NAME,
        {String("The call"),
         String(p_node->get_name()),
         String(p_method)},
        gd::instance_id(p_node)
    );
    return ERR_METHOD_NOT_FOUND;
}

int64_t NetwMultiplayer::rpc_controller_peer(
    const NetwEntityRecord &p_record
) const {
    const int64_t controller
        = entity::Control::public_peer(p_record.get_control()->resolve());
    return controller > 0 ? controller : int64_t(SERVER_PEER);
}

int64_t NetwMultiplayer::rpc_sends_dropped_unroutable() const {
    return rpc_dropped_unroutable;
}

int64_t NetwMultiplayer::rpc_sends_dropped_not_live() const {
    return rpc_dropped_not_live;
}

void NetwMultiplayer::rpc_resolve_parked(
    int64_t p_id,
    const Callable &p_callback,
    uint64_t p_generation
) {
    if (p_generation != transfer_generation) {
        return;
    }
    if (rpc_park.resolve(p_id) && p_callback.is_valid()) {
        p_callback.call();
    }
}

void NetwMultiplayer::rpc_defer_call(
    int64_t p_sender,
    int64_t p_route,
    const Callable &p_callback
) {
    const int64_t timeout = clock_engine().get_configured()
        ? int64_t(clock_engine().get_tickrate())
        : 30;
    const int64_t id
        = rpc_park.park(p_sender, p_route, receive_tick(), timeout);
    if (id < 0) {
        return;
    }
    liveness_when_live(
        p_route,
        callable_mp(this, &NetwMultiplayer::rpc_resolve_parked)
            .bind(id, p_callback, transfer_generation),
        0,
        Callable()
    );
}

void NetwMultiplayer::rpc_sweep_deferred_calls() {
    rpc_park.sweep(receive_tick(), liveness_core);
    LocalVector<int64_t> stranded;
    for (const KeyValue<int64_t, RpcDeferred> &held : rpc_deferred) {
        const bool wrapper_freed = !held.value.wrapper.is_null()
            && gd::object_of(held.value.wrapper) == nullptr;
        if (gd::object_of(held.value.node) == nullptr || wrapper_freed) {
            stranded.push_back(held.key);
        }
    }
    for (const int64_t park : stranded) {
        const RpcDeferred call = rpc_deferred[park];
        rpc_deferred.erase(park);
        rpc_release_gate(call.node, call.gate);
    }
}

void NetwMultiplayer::rpc_sweep_transactions(int64_t p_current) {
    if (rpc_disposed) {
        return;
    }
    const PackedInt64Array expired = rpc_txns.expire(p_current);
    for (int at = 0; at < expired.size(); ++at) {
        const int64_t txn = expired[at];
        NETW_NETWORK_PROFILE(network_profiler.forget_transaction(txn));
        const RpcSettle settle = rpc_settle_of(txn);
        rpc_settles.erase(txn);
        if (settle.single.is_valid()) {
            settle.single->reject(ERR_TIMEOUT, String());
            continue;
        }
        if (settle.group.is_valid()) {
            settle.group->reject(ERR_TIMEOUT, String());
        }
    }
}

void NetwMultiplayer::rpc_handle_disconnect(int64_t p_peer) {
    NETW_NETWORK_PROFILE(network_profiler.forget_peer(p_peer));
    if (rpc_disposed) {
        return;
    }
    const PackedInt64Array waiting = rpc_txns.waiting_on(p_peer);
    for (int at = 0; at < waiting.size(); ++at) {
        const int64_t txn = waiting[at];
        const RpcSettle settle = rpc_settle_of(txn);
        if (settle.single.is_valid()) {
            settle.single->reject(ERR_UNAVAILABLE, String("Peer disconnected"));
            rpc_settle_close(txn);
            continue;
        }
        if (settle.group.is_valid()) {
            settle.group->remove_peer(p_peer);
            if (settle.group->get_is_settled()) {
                rpc_settle_close(txn);
            }
        }
    }
}

void NetwMultiplayer::rpc_drop_deferred_calls() {
    for (const KeyValue<int64_t, RpcDeferred> &held : rpc_deferred) {
        Object *node = gd::object_of(held.value.node);
        if (node == nullptr) {
            continue;
        }
        const Callable opener = rpc_gate_opener(node, held.value.gate);
        if (node->is_connected(held.value.gate, opener)) {
            node->disconnect(held.value.gate, opener);
        }
    }
    rpc_deferred.clear();
}

void NetwMultiplayer::rpc_clear_session() {
    rpc_park.clear();
    rpc_calls_deferred = 0;
    rpc_drop_deferred_calls();
    if (rpc_disposed) {
        return;
    }
    const PackedInt64Array abandoned = rpc_txns.drain();
    const HashMap<int64_t, RpcSettle> settles(rpc_settles);
    rpc_settles.clear();
    for (int at = 0; at < abandoned.size(); ++at) {
        const RpcSettle *found = settles.getptr(abandoned[at]);
        if (found == nullptr) {
            continue;
        }
        if (found->single.is_valid()) {
            found->single->reject(ERR_UNAVAILABLE, String("Session ended"));
            continue;
        }
        if (found->group.is_valid()) {
            found->group->reject(ERR_UNAVAILABLE, String("Session ended"));
        }
    }
}

void NetwMultiplayer::rpc_dispose() {
    rpc_disposed = true;
    rpc_settles.clear();
}

Dictionary NetwMultiplayer::rpc_counters() const {
    Dictionary out;
    out[StringName("drops_backlog_limit")] = rpc_park.refused();
    out[StringName("sends_dropped_unroutable")]
        = rpc_sends_dropped_unroutable();
    out[StringName("sends_dropped_not_live")] = rpc_sends_dropped_not_live();
    return out;
}

Dictionary NetwMultiplayer::relay_stats_snapshot() {
    ReplicationCore *plane = get_replication_plane();
    const Dictionary repl = plane == nullptr ? Dictionary() : plane->counters();
    const Dictionary rpc = rpc_counters();
    static const char *RELAYED[] = {
        "drops_unknown_route",   "drops_not_live",
        "drops_no_node",         "drops_traversal",
        "drops_comp_unresolved", "sync_drops_stale",
        "derived_sets_active",   "derived_frames_in",
        "drops_derived_no_set",  "drops_derived_bad_sender",
        "drops_derived_schema",  "row_frames_out",
        "row_frames_full",       "row_frames_stage_refused",
        "row_frames_ungathered", "row_frames_deferred",
        "retained_frames_out",   "window_frames_out",
        "window_samples_out",    "sync_sets_active",
        "sync_frames_out",       "sync_frames_in",
        "delta_frames_out",      "delta_frames_in",
        "drops_sync_no_set",     "drops_sync_bad_sender",
        "drops_sync_poisoned",   "drops_sync_unknown_flag",
        "spawn_book_armed",      "spawn_book_spawned",
        "spawn_book_recv",
    };
    Dictionary out;
    for (const char *key : RELAYED) {
        out[StringName(key)] = repl.get(StringName(key), int64_t(0));
    }
    out[StringName("drops_sync_bad_sender")]
        = int64_t(repl.get(StringName("drops_sync_bad_sender"), 0))
        + int64_t(repl.get(StringName("drops_write_refused"), 0));
    out[StringName("drops_backlog_limit")]
        = rpc[StringName("drops_backlog_limit")];
    out[StringName("sends_dropped_unroutable")]
        = int64_t(repl.get(StringName("sends_dropped_unroutable"), 0))
        + int64_t(rpc[StringName("sends_dropped_unroutable")]);
    out[StringName("sends_dropped_not_live")]
        = int64_t(repl.get(StringName("sends_dropped_not_live"), 0))
        + int64_t(rpc[StringName("sends_dropped_not_live")]);
    const wire::Wire &counts = world.wire;
    const wire::AttributionBook &attribution = counts.attribution;
    out[StringName("sent_packets")] = counts.sent_packets;
    out[StringName("sent_bytes")] = counts.sent_bytes;
    out[StringName("received_packets")] = counts.received_packets;
    out[StringName("received_bytes")] = counts.received_bytes;
    out[StringName("state_acks_out")] = counts.state_acks_out;
    out[StringName("state_acks_in")] = counts.state_acks_in;
    out[StringName("standalone_acks_out")] = counts.standalone_acks_out;
    out[StringName("attributed_bytes_out")] = attribution.get_attributed_out();
    out[StringName("attributed_bytes_in")] = attribution.get_attributed_in();
    out[StringName("attributed_frames_out")] = attribution.get_frames_out();
    out[StringName("attributed_frames_in")] = attribution.get_frames_in();
    out[StringName("attribution_framing_out")] = attribution.get_framing_out();
    out[StringName("attribution_dropped_out")]
        = attribution.get_staged_dropped_out();
    return out;
}

Dictionary NetwMultiplayer::lagcomp_metrics() const {
    Dictionary out = predict_metrics();
    out[StringName("pending_actions")] = pending_action_count();
    out[StringName("gate_fallbacks")] = action_gate_fallbacks();
    out[StringName("effects_armed")] = lagcomp_effect_count();
    return out;
}

double NetwMultiplayer::peer_get_traffic(
    int64_t p_peer,
    Traffic p_stat
) const {
    return world.wire.traffic.read(p_peer, int(p_stat));
}

int64_t NetwMultiplayer::packet_overhead() const {
    const connect::Transport *transport = connect_core.live_transport();
    return transport != nullptr ? transport->packet_overhead()
                                : connect::Transport::UDP_PACKET_OVERHEAD;
}

void NetwMultiplayer::stats_fill_predict(Dictionary &out) const {
    const Dictionary predict = lagcomp_metrics();
    out[StringName("predict_entities")] = predict[StringName("entities")];
    out[StringName("predict_timelines")] = predict[StringName("timelines")];
    out[StringName("predict_corrections")] = predict[StringName("corrections")];
    out[StringName("predict_installs")] = predict[StringName("installs")];
    out[StringName("predict_max_replay_depth")]
        = predict[StringName("max_replay_depth")];
    out[StringName("predict_consumed")] = predict[StringName("consumed")];
    out[StringName("predict_missing")] = predict[StringName("missing")];
    out[StringName("predict_pending_actions")]
        = predict.has(StringName("predict_pending_actions"))
        ? predict[StringName("predict_pending_actions")]
        : predict[StringName("pending_actions")];
    out[StringName("predict_effects_armed")]
        = predict[StringName("effects_armed")];
    out[StringName("predict_gate_fallbacks")]
        = predict[StringName("gate_fallbacks")];

    const Dictionary joint = predict[StringName("joint")];
    out[StringName("joint_passes")] = joint[StringName("joint_passes")];
    out[StringName("joint_members")] = joint[StringName("joint_members")];
    out[StringName("joint_cells_relayed")] = joint[StringName("cells_relayed")];
    out[StringName("joint_cells_substituted")]
        = joint[StringName("cells_substituted")];
    out[StringName("joint_heal_snaps")] = joint[StringName("heal_snaps")];
    out[StringName("joint_linger_held")] = joint[StringName("linger_held")];
}

void NetwMultiplayer::stats_fill_display(Dictionary &r_out) const {
    const display::PumpStats &display = display_book.get_stats();
    r_out[StringName("display_runtimes")] = display.runtimes;
    r_out[StringName("display_starving")] = display.starving;
    r_out[StringName("display_sleeping")] = display.sleeping;
    r_out[StringName("display_projecting")] = display.projecting;
    r_out[StringName("display_snaps")] = display.snaps;
    r_out[StringName("display_max_display_lag")]
        = int64_t(display.max_display_lag);
    r_out[StringName("display_max_forecast_age")]
        = int64_t(display.max_forecast_age);
}

int64_t NetwMultiplayer::stats_get(Stat p_stat) {
    static const char *STAT_KEYS[] = {
#define NETW_SESSION_STAT_KEY(m_name, m_key) m_key,
        NETW_SESSION_STAT_TABLE(NETW_SESSION_STAT_KEY)
#undef NETW_SESSION_STAT_KEY
    };
    if (p_stat < 0 || p_stat >= int64_t(STAT_COUNT)) {
        return 0;
    }
    const StringName key(STAT_KEYS[p_stat]);
    const wire::Wire &counts = world.wire;
    const wire::AttributionBook &attribution = counts.attribution;
    Dictionary family;
    switch (p_stat) {
        case STAT_SENT_PACKETS:
            return counts.sent_packets;
        case STAT_SENT_BYTES:
            return counts.sent_bytes;
        case STAT_RECEIVED_PACKETS:
            return counts.received_packets;
        case STAT_RECEIVED_BYTES:
            return counts.received_bytes;
        case STAT_STATE_ACKS_OUT:
            return counts.state_acks_out;
        case STAT_STATE_ACKS_IN:
            return counts.state_acks_in;
        case STAT_STANDALONE_ACKS_OUT:
            return counts.standalone_acks_out;
        case STAT_ATTRIBUTED_BYTES_OUT:
            return attribution.get_attributed_out();
        case STAT_ATTRIBUTED_BYTES_IN:
            return attribution.get_attributed_in();
        case STAT_ATTRIBUTED_FRAMES_OUT:
            return attribution.get_frames_out();
        case STAT_ATTRIBUTED_FRAMES_IN:
            return attribution.get_frames_in();
        case STAT_ATTRIBUTION_FRAMING_OUT:
            return attribution.get_framing_out();
        case STAT_ATTRIBUTION_DROPPED_OUT:
            return attribution.get_staged_dropped_out();
        case STAT_DROPS_BACKLOG_LIMIT:
            return rpc_park.refused();
        case STAT_PENDING_LIVE:
        case STAT_INTEREST_RELAY_BACKLOG:
            return liveness_pending_live_count();
        case STAT_INTEREST_LAYERS:
            return interest_layers().size();
        case STAT_INTEREST_ENTITIES_FILTERED:
            return interest_engine.membership_keys().size();
        case STAT_INTEREST_VISIBLE_EDGES:
            return interest_engine.stats_edges();
        case STAT_INTEREST_DIRTY_ENTITIES:
            return interest_engine.dirty_count();
        case STAT_INTEREST_TRANSITIONS_TOTAL:
            return interest_engine.transitions_total();
        case STAT_INTEREST_VANISHED_DIRTY_SKIPS:
            return interest_engine.stats_vanished_dirty_skips();
        case STAT_VERDICT_DOES_NOT_EXIST:
            return stats_get_verdict_count(ERR_DOES_NOT_EXIST);
        case STAT_VERDICT_SKIP:
            return stats_get_verdict_count(ERR_SKIP);
        case STAT_VERDICT_UNAVAILABLE:
            return stats_get_verdict_count(ERR_UNAVAILABLE);
        case STAT_VERDICT_UNAUTHORIZED:
            return stats_get_verdict_count(ERR_UNAUTHORIZED);
        case STAT_VERDICT_INVALID_DATA:
            return stats_get_verdict_count(ERR_INVALID_DATA);
        case STAT_VERDICT_BUSY:
            return stats_get_verdict_count(ERR_BUSY);
        case STAT_DROPS_TABLE_UNKNOWN:
        case STAT_DROPS_TABLE_SCHEMA:
        case STAT_DROPS_TABLE_UNKNOWN_FLAG:
        case STAT_DROPS_TABLE_TRUNCATED:
        case STAT_DROPS_TABLE_BAD_SENDER:
        case STAT_TABLE_DROPS_STALE:
        case STAT_TABLE_DROPS_TOMBSTONE:
        case STAT_TABLE_DROPS_STALE_ROW:
            family = array_tables.counters();
            break;
        case STAT_PREDICT_ENTITIES:
        case STAT_PREDICT_TIMELINES:
        case STAT_PREDICT_CORRECTIONS:
        case STAT_PREDICT_INSTALLS:
        case STAT_PREDICT_MAX_REPLAY_DEPTH:
        case STAT_PREDICT_CONSUMED:
        case STAT_PREDICT_MISSING:
        case STAT_PREDICT_PENDING_ACTIONS:
        case STAT_PREDICT_EFFECTS_ARMED:
        case STAT_PREDICT_GATE_FALLBACKS:
        case STAT_JOINT_PASSES:
        case STAT_JOINT_MEMBERS:
        case STAT_JOINT_CELLS_RELAYED:
        case STAT_JOINT_CELLS_SUBSTITUTED:
        case STAT_JOINT_HEAL_SNAPS:
        case STAT_JOINT_LINGER_HELD:
            stats_fill_predict(family);
            break;
        case STAT_DISPLAY_RUNTIMES:
        case STAT_DISPLAY_STARVING:
        case STAT_DISPLAY_SLEEPING:
        case STAT_DISPLAY_PROJECTING:
        case STAT_DISPLAY_SNAPS:
        case STAT_DISPLAY_MAX_DISPLAY_LAG:
        case STAT_DISPLAY_MAX_FORECAST_AGE:
            stats_fill_display(family);
            break;
        default:
            family = relay_stats_snapshot();
            break;
    }
    return int64_t(family.get(key, 0));
}

void NetwMultiplayer::rpc_settle_open(
    int64_t p_txn,
    const RpcSettle &p_settle
) {
    rpc_settles[p_txn] = p_settle;
}

NetwMultiplayer::RpcSettle NetwMultiplayer::rpc_settle_of(int64_t p_txn) const {
    const RpcSettle *found = rpc_settles.getptr(p_txn);
    return found ? *found : RpcSettle();
}

void NetwMultiplayer::rpc_settle_close(int64_t p_txn) {
    rpc_settles.erase(p_txn);
    if (!rpc_disposed) {
        rpc_txns.close(p_txn);
    }
}

Variant NetwMultiplayer::rpc_method_token(
    const NetwEntityRecord &p_record,
    Node *p_node,
    const StringName &p_method
) const {
    const Ref<Script> script = p_node->get_script();
    if (script.is_valid() && !p_record.get_comp_table().get_poisoned()) {
        const int64_t known
            = netw::script::model::get_method_id(script, p_method);
        if (known > 0) {
            return known;
        }
    }
    return p_method;
}

LocalVector<call_args::Slot> NetwMultiplayer::rpc_encoded_args(
    const Array &p_args
) {
    LocalVector<call_args::Slot> out;
    out.reserve(uint32_t(p_args.size()));
    for (int at = 0; at < p_args.size(); ++at) {
        out.push_back(rpc_encoded_arg(p_args[at]));
    }
    return out;
}

call_args::Slot NetwMultiplayer::rpc_encoded_arg(const Variant &p_arg) {
    Ref<NetwEntity> entity;
    Node *node = nullptr;
    if (Object *held = p_arg.get_type() == Variant::OBJECT
            ? Object::cast_to<Object>(p_arg)
            : nullptr) {
        entity = Ref<NetwEntity>(Object::cast_to<NetwEntity>(held));
        if (entity.is_valid()) {
            node = entity->get_owner();
        } else if (Node *bare = Object::cast_to<Node>(held)) {
            node = bare;
            entity = NetwEntity::of(bare);
        }
    }
    if (entity.is_null() && node != nullptr) {
        fault::raise(
            this,
            NETW_FAULT_UNSENDABLE_VALUE,
            {"An RPC argument",
             "a node with no entity",
             "a node spawned with {Netw.spawn}, or the node's path"},
            gd::instance_id(node)
        );
        return call_args::of_value(Variant());
    }
    if (entity.is_null()) {
        return call_args::of_value(p_arg);
    }
    const int64_t route = liveness_route_of(entity.ptr());
    if (route <= 0) {
        fault::raise(
            this,
            NETW_FAULT_UNSENDABLE_VALUE,
            {"An RPC argument",
             "a node that is not spawned on the network",
             "a node spawned with {Netw.spawn}"},
            gd::instance_id(node)
        );
        return call_args::of_value(Variant());
    }
    return call_args::of_node(
        route,
        entity->comp_of(node),
        entity->comp_path_of(node)
    );
}

void NetwMultiplayer::rpc_call(
    const Callable &p_callable,
    const Array &p_args,
    int64_t p_peer
) {
    Node *node = Object::cast_to<Node>(p_callable.get_object());
    const StringName method = p_callable.get_method();
    const CallTarget target = rpc_target(node, method);
    if (target.code != OK) {
        return;
    }
    const int64_t route = target.route;

    String refusal;
    if (rpc_declared(node, method, refusal) != OK) {
        return;
    }
    const Ref<Script> script = node->get_script();
    const Ref<NetwMemberConfig> options
        = netw::script::model::get_rpc_options(script, method);

    const Variant method_token = rpc_method_token(*target.record, node, method);
    const int64_t comp
        = NetwEntity::comp_in(*target.record, target.owner, node);
    const String comp_path
        = NetwEntity::comp_path_in(*target.record, target.owner, node);
    const bool reliable = options.is_valid() && options->is_transfer_declared()
        ? options->get_transfer_mode() == NetwMemberConfig::TRANSFER_RELIABLE
        : script.is_null()
            || netw::script::model::get_method_reliable(script, method);
    const LocalVector<call_args::Slot> encoded = rpc_encoded_args(p_args);

    const bool call_local = options.is_valid()
        ? options->get_is_call_local()
        : netw::script::model::get_method_call_local(script, method);
    const int64_t local_id = get_unique_id();
    if (p_peer == 0 || p_peer == local_id) {
        if (call_local) {
            const int64_t previous = dispatching_sender;
            dispatching_sender = local_id;
            game_call(p_callable, p_args, game_calls::RPC_LOCAL);
            dispatching_sender = previous;
        } else if (p_peer == local_id) {
            fault::raise(
                this,
                NETW_FAULT_BAD_VALUE,
                {vformat("RPC '%s'", String(method)),
                 "this peer as the target",
                 "another peer, or call {NetwMemberConfig.call_local} on it "
                 "to run it here"},
                0
            );
            return;
        }
    }

    const PackedByteArray payload = call_frame(
        p_peer == 0 ? CALL_TARGET_EVERYONE
                    : (p_peer == 1 ? CALL_TARGET_SERVER : CALL_TARGET_PEER),
        0,
        false,
        p_peer,
        method_token,
        encoded,
        options.is_valid() ? options->get_quantizers() : Array(),
        netw::script::model::get_method_arg_types(script, method),
        this
    );

    PackedInt32Array recipients;
    if (p_peer == 0) {
        recipients = rpc_recipients(target.entity);
    } else if (p_peer != local_id) {
        recipients.push_back(int32_t(p_peer));
    }
    NETW_NETWORK_PROFILE(
        wire::ProfileSubjectScope observation(
            profiler_outgoing,
            profiler_member(route, wire::CHANNEL_CALL, node, method)
        )
    );
    for (int at = 0; at < recipients.size(); ++at) {
        send_to(
            recipients[at],
            route,
            wire::CHANNEL_CALL,
            payload,
            reliable,
            comp,
            comp_path
        );
    }
}

Error NetwMultiplayer::rpc_request_prepare(
    const Callable &p_callable,
    const Array &p_args,
    double p_timeout_seconds,
    RpcRequest &r_out,
    String &r_detail
) {
    Node *node = Object::cast_to<Node>(p_callable.get_object());
    const StringName method = p_callable.get_method();
    const CallTarget target = rpc_target(node, method);
    if (target.code != OK) {
        r_detail = target.detail;
        return target.code;
    }
    const int64_t route = target.route;

    const Error declared = rpc_declared(node, method, r_detail);
    if (declared != OK) {
        return declared;
    }
    const Ref<Script> script = node->get_script();
    const Ref<NetwMemberConfig> options
        = netw::script::model::get_rpc_options(script, method);

    const bool timed = clock_engine().get_configured();
    const int64_t current = timed ? clock_engine().get_tick() : receive_tick();
    const int64_t tickrate = timed ? clock_engine().get_tickrate() : 30;
    const double declared_seconds
        = options.is_valid() ? options->get_timeout_seconds() : 0.0;
    double timeout_seconds = rpc_request_timeout_default();
    if (p_timeout_seconds > 0.0) {
        timeout_seconds = p_timeout_seconds;
    } else if (declared_seconds > 0.0) {
        timeout_seconds = declared_seconds;
    }

    r_out.entity = target.entity;
    r_out.node = node;
    r_out.route = route;
    r_out.comp = NetwEntity::comp_in(*target.record, target.owner, node);
    r_out.comp_path
        = NetwEntity::comp_path_in(*target.record, target.owner, node);
    r_out.method = method;
    r_out.method_token = rpc_method_token(*target.record, node, method);
    r_out.encoded_args = rpc_encoded_args(p_args);
    r_out.quantizers = options.is_valid() ? options->get_quantizers() : Array();
    r_out.arg_types = netw::script::model::get_method_arg_types(script, method);
    r_out.deadline = current + int64_t(timeout_seconds * double(tickrate));
    r_out.call_local = options.is_valid()
        ? options->get_is_call_local()
        : netw::script::model::get_method_call_local(script, method);
    return OK;
}

void NetwMultiplayer::rpc_open_txn(
    int64_t p_txn,
    const PackedInt64Array &p_addressed,
    const RpcSettle &p_settle,
    int64_t p_deadline
) {
    rpc_txns.open(p_txn, p_addressed, p_deadline);
    rpc_settle_open(p_txn, p_settle);
}

Ref<NetwPromise> NetwMultiplayer::rpc_request_call(
    int64_t p_peer,
    const Callable &p_callable,
    const Array &p_args,
    double p_timeout_seconds
) {
    RpcRequest request;
    String refusal;
    const Error prepared = rpc_request_prepare(
        p_callable,
        p_args,
        p_timeout_seconds,
        request,
        refusal
    );
    if (prepared != OK) {
        return NetwPromise::refused(prepared, refusal);
    }

    const int64_t txn = rpc_txns.mint();
    Ref<NetwPromise> promise;
    promise.instantiate();
    PackedInt64Array addressed;
    addressed.push_back(p_peer);
    rpc_open_txn(txn, addressed, promise, request.deadline);

    const PackedByteArray payload = call_frame(
        p_peer == 1 ? CALL_TARGET_SERVER
                    : (p_peer > 1 ? CALL_TARGET_PEER : CALL_TARGET_EVERYONE),
        txn,
        true,
        p_peer,
        request.method_token,
        request.encoded_args,
        request.quantizers,
        request.arg_types,
        this
    );

    NETW_NETWORK_PROFILE(
        const uint32_t subject = profiler_member(
            request.route,
            wire::CHANNEL_CALL,
            request.node,
            request.method
        )
    );
    NETW_NETWORK_PROFILE(
        wire::ProfileSubjectScope observation(profiler_outgoing, subject)
    );
    NETW_NETWORK_PROFILE(
        network_profiler.remember_reply(p_peer, txn, false, subject)
    );
    send_to(
        p_peer,
        request.route,
        wire::CHANNEL_CALL,
        payload,
        true,
        request.comp,
        request.comp_path
    );
    return promise;
}

Ref<NetwGroupPromise> NetwMultiplayer::rpc_request_call_group(
    const Callable &p_callable,
    const Array &p_args,
    double p_timeout_seconds
) {
    RpcRequest request;
    String refusal;
    const Error prepared = rpc_request_prepare(
        p_callable,
        p_args,
        p_timeout_seconds,
        request,
        refusal
    );
    if (prepared != OK) {
        return NetwGroupPromise::refused(prepared, refusal);
    }

    const PackedInt32Array peers = rpc_recipients(request.entity);
    const int64_t local_id = get_unique_id();
    const bool asks_caller
        = request.call_local && !peers.has(int32_t(local_id));
    PackedInt32Array expected = peers;
    if (asks_caller) {
        expected.push_back(int32_t(local_id));
    }
    const int64_t txn = rpc_txns.mint();
    const Ref<NetwGroupPromise> promise = NetwGroupPromise::create(expected);
    PackedInt64Array owed;
    for (int at = 0; at < expected.size(); ++at) {
        owed.push_back(expected[at]);
    }
    rpc_open_txn(txn, owed, promise, request.deadline);
    if (expected.is_empty()) {
        session_defer(
            Callable(promise.ptr(), StringName("resolve_all")),
            StringName()
        );
    }

    const PackedByteArray payload = call_frame(
        CALL_TARGET_EVERYONE,
        txn,
        true,
        0,
        request.method_token,
        request.encoded_args,
        request.quantizers,
        request.arg_types,
        this
    );

    NETW_NETWORK_PROFILE(
        const uint32_t subject = profiler_member(
            request.route,
            wire::CHANNEL_CALL,
            request.node,
            request.method
        )
    );
    NETW_NETWORK_PROFILE(
        wire::ProfileSubjectScope observation(profiler_outgoing, subject)
    );
    for (int at = 0; at < peers.size(); ++at) {
        NETW_NETWORK_PROFILE(
            network_profiler.remember_reply(peers[at], txn, false, subject)
        );
        send_to(
            peers[at],
            request.route,
            wire::CHANNEL_CALL,
            payload,
            true,
            request.comp,
            request.comp_path
        );
    }
    if (asks_caller) {
        const int64_t previous = dispatching_sender;
        dispatching_sender = local_id;
        rpc_execute_call(
            request.entity,
            request.node,
            request.method,
            p_args,
            txn,
            local_id,
            transfer_generation
        );
        dispatching_sender = previous;
    }
    return promise;
}

void NetwMultiplayer::rpc_call_controller(
    const Callable &p_callable,
    const Array &p_args
) {
    Node *node = Object::cast_to<Node>(p_callable.get_object());
    const CallTarget target = rpc_target(node, p_callable.get_method());
    if (target.code != OK) {
        return;
    }
    const int64_t peer = rpc_controller_peer(*target.record);
    const int64_t local_id = get_unique_id();
    String refusal;
    if (peer != local_id) {
        rpc_call(p_callable, p_args, peer);
        return;
    }
    if (rpc_declared(node, p_callable.get_method(), refusal) != OK) {
        return;
    }
    const int64_t previous = dispatching_sender;
    dispatching_sender = local_id;
    game_call(p_callable, p_args, game_calls::RPC_CONTROLLER_LOCAL);
    dispatching_sender = previous;
}

Ref<NetwPromise> NetwMultiplayer::rpc_request_controller(
    const Callable &p_callable,
    const Array &p_args
) {
    Node *node = Object::cast_to<Node>(p_callable.get_object());
    const CallTarget target = rpc_target(node, p_callable.get_method());
    if (target.code != OK) {
        return NetwPromise::refused(target.code, target.detail);
    }
    return rpc_request_call(
        rpc_controller_peer(*target.record),
        p_callable,
        p_args
    );
}

void NetwMultiplayer::rpc_send_reply(
    int64_t p_peer,
    int64_t p_route,
    int64_t p_txn,
    const Variant &p_value
) {
    wire::WriteStream stream;
    uint64_t txn = uint64_t(MAX(p_txn, int64_t(0)));
    bool failed = false;
    LocalVector<call_args::Slot> slots;
    slots.push_back(rpc_encoded_arg(p_value));
    if (!stream.varuint(txn, 5) || !stream.bool1(failed)
        || !call_args::write(stream, slots, Array(), Array(), this)
        || !stream.align_verify()) {
        return;
    }
    rpc_post_reply(p_peer, p_route, p_txn, stream.to_bytes());
}

void NetwMultiplayer::rpc_send_refusal(
    int64_t p_peer,
    int64_t p_route,
    int64_t p_txn,
    Error p_code,
    const String &p_detail
) {
    if (p_txn <= 0) {
        return;
    }
    wire::WriteStream stream;
    uint64_t txn = uint64_t(p_txn);
    bool failed = true;
    uint64_t code = uint64_t(p_code);
    String detail = p_detail.left(255);
    if (!stream.varuint(txn, 5) || !stream.bool1(failed)
        || !stream.varuint(code, 2) || !wire::string_field(stream, detail)
        || !stream.align_verify()) {
        return;
    }
    rpc_post_reply(p_peer, p_route, p_txn, stream.to_bytes());
}

void NetwMultiplayer::rpc_refuse_unrouted_call(
    int64_t p_peer,
    int64_t p_route,
    const PackedByteArray &p_payload
) {
    wire::ReadStream reader(p_payload);
    uint64_t flag = 0;
    uint64_t txn = 0;
    if (!reader.bits(flag, 8) || (flag & CALL_AWAITS_REPLY) == 0
        || !reader.varuint(txn, 5)) {
        return;
    }
    rpc_send_refusal(
        p_peer,
        p_route,
        int64_t(txn),
        ERR_DOES_NOT_EXIST,
        vformat(
            "Peer %d does not have the node this call targets, so the call "
            "was dropped. Build the same scene tree on every peer.",
            int(get_unique_id())
        )
    );
}

void NetwMultiplayer::rpc_post_reply(
    int64_t p_peer,
    int64_t p_route,
    int64_t p_txn,
    const PackedByteArray &p_payload
) {
    NETW_NETWORK_PROFILE(
        wire::ProfileSubjectScope observation(
            profiler_outgoing,
            network_profiler.take_reply(p_peer, p_txn, true)
        )
    );
    send_to(p_peer, p_route, wire::CHANNEL_REPLY, p_payload, true, 0, String());
}

void NetwMultiplayer::rpc_handle_reply_failed(
    int64_t p_txn,
    int64_t p_sender,
    Error p_code,
    const String &p_detail
) {
    if (!rpc_txns.admits(p_txn, p_sender)) {
        return;
    }
    const RpcSettle settle = rpc_settle_of(p_txn);
    if (settle.single.is_valid()) {
        settle.single->reject(p_code, p_detail);
        rpc_settle_close(p_txn);
        return;
    }
    if (settle.group.is_valid()) {
        settle.group->refuse_peer(p_sender, p_code, p_detail);
        if (settle.group->get_is_settled()) {
            rpc_settle_close(p_txn);
        }
    }
}

void NetwMultiplayer::rpc_handle_reply(
    int64_t p_txn,
    int64_t p_sender,
    const Variant &p_value
) {
    if (!rpc_txns.admits(p_txn, p_sender)) {
        return;
    }
    const RpcSettle settle = rpc_settle_of(p_txn);
    if (settle.single.is_valid()) {
        settle.single->resolve(p_value);
        rpc_settle_close(p_txn);
        return;
    }
    if (settle.group.is_valid()) {
        settle.group->resolve_peer(p_sender, p_value);
        if (settle.group->get_is_settled()) {
            rpc_settle_close(p_txn);
        }
    }
}

void NetwMultiplayer::rpc_reply_resolved(
    const Variant &p_value,
    int64_t p_peer,
    int64_t p_route,
    int64_t p_txn,
    uint64_t p_generation
) {
    if (p_generation != transfer_generation) {
        return;
    }
    rpc_send_reply(p_peer, p_route, p_txn, p_value);
}

void NetwMultiplayer::rpc_reply_rejected(
    int64_t p_code,
    const String &p_detail,
    int64_t p_peer,
    int64_t p_route,
    int64_t p_txn,
    uint64_t p_generation
) {
    if (p_generation != transfer_generation) {
        return;
    }
    rpc_send_refusal(p_peer, p_route, p_txn, Error(p_code), p_detail);
}

void NetwMultiplayer::rpc_reply_when_settled(
    const Ref<NetwPromise> &p_promise,
    int64_t p_peer,
    int64_t p_route,
    int64_t p_txn
) {
    p_promise->then(callable_mp(this, &NetwMultiplayer::rpc_reply_resolved)
                        .bind(p_peer, p_route, p_txn, transfer_generation));
    p_promise->catch_error(
        callable_mp(this, &NetwMultiplayer::rpc_reply_rejected)
            .bind(p_peer, p_route, p_txn, transfer_generation)
    );
}

void NetwMultiplayer::rpc_reply_returned(
    const Variant &p_value,
    const Variant &p_held_until_return,
    int64_t p_peer,
    int64_t p_route,
    int64_t p_txn,
    uint64_t p_generation
) {
    rpc_reply_resolved(p_value, p_peer, p_route, p_txn, p_generation);
}

bool NetwMultiplayer::rpc_reply_when_returned(
    const Variant &p_result,
    int64_t p_peer,
    int64_t p_route,
    int64_t p_txn
) {
    Object *suspended = gd::live_object(p_result);
    if (suspended == nullptr
        || !suspended->is_class(StringName("GDScriptFunctionState"))) {
        return false;
    }
    suspended->connect(
        StringName("completed"),
        callable_mp(this, &NetwMultiplayer::rpc_reply_returned)
            .bind(p_result, p_peer, p_route, p_txn, transfer_generation),
        Object::CONNECT_ONE_SHOT
    );
    return true;
}

bool NetwMultiplayer::rpc_defer_signal_fired(
    Node *p_node,
    const StringName &p_signal
) {
    if (p_signal == StringName("ready")) {
#if defined(NETW_MODULE)
        return p_node->is_ready();
#else
        return p_node->is_node_ready();
#endif
    }
    return bool(p_node->get_meta(
        StringName(String("_netw_fired_") + String(p_signal)),
        false
    ));
}

bool NetwMultiplayer::rpc_sender_allowed(
    const Ref<NetwMemberConfig> &p_options,
    const Ref<Script> &p_script,
    const StringName &p_method,
    const NetwEntityRecord *p_record,
    Node *p_node,
    int64_t p_sender
) const {
    if (p_sender == SERVER_PEER) {
        return true;
    }
    if (p_options.is_valid() && p_options->is_policy_declared()) {
        switch (p_options->get_write_policy()) {
            case NetwMemberConfig::POLICY_CONTROLLER:
                return p_record != nullptr
                    && p_sender
                    == entity::Control::public_peer(
                        p_record->get_control()->resolve()
                    );
            case NetwMemberConfig::POLICY_ANY_PEER:
                return true;
            case NetwMemberConfig::POLICY_SERVER:
                return false;
            default:
                return p_sender == p_node->get_multiplayer_authority();
        }
    }
    const int64_t mode
        = netw::script::model::get_method_rpc_mode(p_script, p_method);
    if (mode == 2) {
        return p_sender == p_node->get_multiplayer_authority();
    }
    return mode == 1;
}

void NetwMultiplayer::rpc_record_interpolated_args(
    Node *p_comp_node,
    const Array &p_args,
    const Array &p_interpolators
) {
    if (p_interpolators.is_empty()) {
        return;
    }
    const int64_t tick = receive_tick();
    for (int at = 0; at < p_interpolators.size(); ++at) {
        const Ref<NetwInterpolate> spec = p_interpolators[at];
        if (spec.is_null() || String(spec->get_target()).is_empty()) {
            continue;
        }
        if (at >= p_args.size()) {
            continue;
        }
        display_record(
            p_comp_node,
            spec->get_target(),
            p_args[at],
            tick,
            spec,
            false
        );
    }
}

void NetwMultiplayer::rpc_execute_call(
    const RID &p_entity,
    Node *p_comp_node,
    const StringName &p_method,
    const Array &p_args,
    int64_t p_txn,
    int64_t p_sender,
    uint64_t p_generation
) {
    const NetwEntityRecord *record = entity_record(p_entity);
    if (p_generation != transfer_generation || record == nullptr) {
        return;
    }
    const int64_t route = record->get_route();
    const Variant result = p_comp_node->callv(p_method, p_args);
    if (p_txn <= 0) {
        return;
    }
    const Ref<NetwPromise> promise = result;
    if (promise.is_valid()) {
        rpc_reply_when_settled(promise, p_sender, route, p_txn);
        return;
    }
    if (rpc_reply_when_returned(result, p_sender, route, p_txn)) {
        return;
    }
    rpc_send_reply(p_sender, route, p_txn, result);
}

ObjectID NetwMultiplayer::rpc_holder_of(const RID &p_entity) const {
    return gd::instance_id(views.holder_of(p_entity.get_id()));
}

Callable NetwMultiplayer::rpc_gate_opener(
    Object *p_node,
    const StringName &p_gate
) {
    return gd::dropping_signal_arguments(
        callable_mp(this, &NetwMultiplayer::rpc_open_gate)
            .bind(uint64_t(gd::instance_id(p_node)), p_gate),
        p_node,
        p_gate
    );
}

int64_t NetwMultiplayer::rpc_next_park(
    ObjectID p_node,
    const StringName &p_gate
) const {
    int64_t next = 0;
    for (const KeyValue<int64_t, RpcDeferred> &held : rpc_deferred) {
        if (held.value.node == p_node && held.value.gate == p_gate
            && (next == 0 || held.key < next)) {
            next = held.key;
        }
    }
    return next;
}

void NetwMultiplayer::rpc_release_gate(
    ObjectID p_node,
    const StringName &p_gate
) {
    if (rpc_next_park(p_node, p_gate) != 0) {
        return;
    }
    Object *node = gd::object_of(p_node);
    if (node == nullptr) {
        return;
    }
    const Callable opener = rpc_gate_opener(node, p_gate);
    if (node->is_connected(p_gate, opener)) {
        node->disconnect(p_gate, opener);
    }
}

void NetwMultiplayer::rpc_open_gate(uint64_t p_node, const StringName &p_gate) {
    const Ref<NetwMultiplayer> keep(this);
    const ObjectID node(p_node);
    for (int64_t park = rpc_next_park(node, p_gate); park != 0;
         park = rpc_next_park(node, p_gate)) {
        const RpcDeferred call = rpc_deferred[park];
        rpc_deferred.erase(park);
        rpc_execute_parked_call(call);
    }
    rpc_release_gate(node, p_gate);
}

void NetwMultiplayer::rpc_execute_parked_call(const RpcDeferred &p_call) {
    Node *node = Object::cast_to<Node>(gd::object_of(p_call.node));
    if (node == nullptr) {
        return;
    }
    rpc_execute_call(
        liveness_core.rid_from_route(int(p_call.route)),
        node,
        p_call.method,
        p_call.args,
        p_call.txn,
        p_call.sender,
        p_call.generation
    );
}

void NetwMultiplayer::rpc_handle_parked_call(
    int64_t p_route,
    uint64_t p_comp_node,
    const PackedByteArray &p_payload,
    int64_t p_sender,
    uint64_t p_generation
) {
    const RID entity = liveness_core.rid_from_route(int(p_route));
    Node *comp_node
        = Object::cast_to<Node>(gd::object_of(ObjectID(p_comp_node)));
    if (p_generation != transfer_generation || comp_node == nullptr
        || entity_record(entity) == nullptr) {
        return;
    }
    rpc_handle_call(entity, comp_node, p_payload, p_sender);
}

void NetwMultiplayer::rpc_handle_call(
    const RID &p_entity,
    Node *p_comp_node,
    const PackedByteArray &p_payload,
    int64_t p_sender
) {
    wire::ReadStream reader(p_payload);
    uint64_t flag = 0;
    if (!reader.bits(flag, 8)) {
        return;
    }
    const int64_t target_type = int64_t(flag) & 3;
    const bool awaits_reply = (flag & CALL_AWAITS_REPLY) != 0;

    int64_t txn = 0;
    if (awaits_reply) {
        uint64_t staged = 0;
        if (!reader.varuint(staged, 5)) {
            return;
        }
        txn = int64_t(staged);
    }
    const int64_t route = liveness_core.route_of(p_entity);
    const auto refuse = [&](Error p_code, const String &p_detail) {
        rpc_send_refusal(p_sender, route, txn, p_code, p_detail);
    };
    const String unreadable
        = String("A call could not be read, so it was dropped.");

    int64_t target_peer = 0;
    if (target_type == CALL_TARGET_SERVER) {
        target_peer = 1;
    } else if (target_type == CALL_TARGET_PEER) {
        uint64_t staged = 0;
        if (!reader.bits(staged, 32)) {
            refuse(ERR_INVALID_DATA, unreadable);
            return;
        }
        target_peer = int64_t(staged);
    }

    const Ref<Script> script = p_comp_node->get_script();
    Variant token;
    if (!netw::script::model::read_token(reader, token)) {
        refuse(ERR_INVALID_DATA, unreadable);
        return;
    }
    const bool numbered = script.is_valid() && token.get_type() == Variant::INT;
    const StringName method = numbered
        ? netw::script::model::get_method_name_by_id(script, token)
        : StringName(token);

    NETW_NETWORK_PROFILE(
        const uint32_t subject = profiler_member(
            route,
            wire::CHANNEL_CALL,
            p_comp_node,
            method
        )
    );
    NETW_NETWORK_PROFILE(network_profiler.name_arrival(subject));
    NETW_NETWORK_PROFILE(
        network_profiler.remember_reply(p_sender, txn, true, subject)
    );

    const Ref<NetwMemberConfig> options
        = netw::script::model::get_rpc_options(script, method);
    if (options.is_null()
        && (script.is_null()
            || !Dictionary(script->get_rpc_config()).has(method))) {
        const String detail = String(method).is_empty()
            ? vformat(
                  "A call on '%s' was dropped, because it names no RPC on "
                  "this peer. Declare it the same way on every peer.",
                  String(p_comp_node->get_name())
              )
            : vformat(
                  "A call for '%s' on '%s' was dropped, because it is not an "
                  "RPC on this peer. Declare it the same way on every peer.",
                  String(method),
                  String(p_comp_node->get_name())
              );
        fault::raise(
            this,
            NETW_FAULT_PEER_MISMATCH,
            {vformat("the call to '%s'", String(method)),
             String(),
             vformat("the RPCs declared on '%s'", p_comp_node->get_name()),
             p_sender},
            gd::instance_id(p_comp_node)
        );
        refuse(ERR_METHOD_NOT_FOUND, detail);
        return;
    }

    LocalVector<call_args::Slot> encoded_args;
    const bool decoded = call_args::read(
        reader,
        options.is_valid() ? options->get_quantizers() : Array(),
        netw::script::model::get_method_arg_types(script, method),
        encoded_args
    );
    if (!decoded || !reader.align_verify() || reader.bits_remaining() != 0) {
        const String detail = vformat(
            "A call for '%s' could not be read, so it was dropped. Declare the "
            "RPC with the same arguments on every peer.",
            String(method)
        );
        fault::raise(
            this,
            NETW_FAULT_PEER_MISMATCH,
            {vformat("the call to '%s'", String(method)),
             String(),
             vformat(
                 "the arguments of '%s' on '%s'",
                 String(method),
                 p_comp_node->get_name()
             ),
             p_sender},
            gd::instance_id(p_comp_node)
        );
        refuse(ERR_INVALID_DATA, detail);
        return;
    }

    if (!rpc_sender_allowed(
            options,
            script,
            method,
            entity_record(p_entity),
            p_comp_node,
            p_sender
        )) {
        const String detail = vformat(
            "Peer %d may not call '%s' on '%s', so the call was dropped.",
            p_sender,
            String(method),
            String(p_comp_node->get_name())
        );
        const bool policy_from_rpc_annotation
            = options.is_null() || !options->is_policy_declared();
        fault::raise(
            this,
            NETW_FAULT_RPC_REFUSED,
            {String(method),
             String(p_comp_node->get_name()),
             String(),
             p_sender,
             int64_t(policy_from_rpc_annotation)},
            gd::instance_id(p_comp_node)
        );
        refuse(ERR_UNAUTHORIZED, detail);
        return;
    }

    if (!netw::script::model::validate_argument_count(
            script,
            method,
            encoded_args.size()
        )) {
        const String detail = vformat(
            "A call for '%s' on '%s' had the wrong number of arguments, so it "
            "was dropped.",
            String(method),
            String(p_comp_node->get_name())
        );
        fault::raise(
            this,
            NETW_FAULT_PEER_MISMATCH,
            {vformat("the call to '%s'", String(method)),
             String(),
             vformat(
                 "the number of arguments of '%s' on '%s'",
                 String(method),
                 p_comp_node->get_name()
             ),
             p_sender},
            gd::instance_id(p_comp_node)
        );
        refuse(ERR_INVALID_DATA, detail);
        return;
    }

    Array args;
    for (uint32_t at = 0; at < encoded_args.size(); ++at) {
        const call_args::Slot &slot = encoded_args[at];
        if (!slot.addresses_node) {
            args.push_back(slot.value);
            continue;
        }
        const Ref<NetwEntity> arg_entity = wrapper_for_route(slot.node.route);
        if (arg_entity.is_null()
            && liveness_route_state(slot.node.route) == ENTITY_STATE_UNKNOWN) {
            liveness_when_live(
                slot.node.route,
                callable_mp(this, &NetwMultiplayer::rpc_handle_parked_call)
                    .bind(
                        route,
                        uint64_t(gd::instance_id(p_comp_node)),
                        p_payload,
                        p_sender,
                        transfer_generation
                    ),
                0,
                Callable()
            );
            return;
        }
        if (is_host() && arg_entity.is_valid()
            && !send_admits(p_sender, arg_entity->get_rid_handle())) {
            const String detail = vformat(
                "Peer %d sent a call naming a node it cannot see, so the "
                "call was dropped.",
                p_sender
            );
            count_verdict(ERR_UNAUTHORIZED, slot.node.route);
            fault::raise(
                this,
                NETW_FAULT_MALFORMED_DATAGRAM,
                {String(), String(), String(), p_sender},
                gd::instance_id(p_comp_node)
            );
            refuse(ERR_UNAUTHORIZED, detail);
            return;
        }
        Node *arg_node = nullptr;
        if (arg_entity.is_valid()) {
            arg_node = repl_comp_node(
                slot.node.route,
                slot.node.comp,
                slot.node.path,
                p_sender
            );
        }
        args.push_back(arg_node);
    }

    rpc_record_interpolated_args(
        p_comp_node,
        args,
        options.is_valid() ? options->get_interpolators() : Array()
    );

    if (target_type != CALL_TARGET_EVERYONE && target_peer != get_unique_id()) {
        fault::raise(
            this,
            NETW_FAULT_MALFORMED_DATAGRAM,
            {String(), String(), String(), p_sender},
            gd::instance_id(p_comp_node)
        );
        return;
    }

    if (options.is_valid()
        && !String(options->get_defer_signal_name()).is_empty()) {
        const StringName gate = options->get_defer_signal_name();
        const ObjectID node = gd::instance_id(p_comp_node);
        if ((!rpc_defer_signal_fired(p_comp_node, gate)
             || rpc_next_park(node, gate) != 0)
            && p_comp_node->has_signal(gate)) {
            rpc_calls_deferred += 1;
            NETW_PLOT(
                profile::names::RPC_CALLS_DEFERRED,
                double(rpc_calls_deferred)
            );
            NETW_TRACE(
                sys::SESSION,
                "'%s' is parked on '%s' rather than run now",
                String(method),
                String(gate)
            );
            rpc_deferred.insert(
                ++rpc_deferred_next,
                RpcDeferred{
                    node,
                    gate,
                    route,
                    rpc_holder_of(p_entity),
                    method,
                    args,
                    txn,
                    p_sender,
                    transfer_generation
                }
            );
            const Callable opener = rpc_gate_opener(p_comp_node, gate);
            if (!p_comp_node->is_connected(gate, opener)) {
                p_comp_node->connect(gate, opener);
            }
            return;
        }
    }

    rpc_execute_call(
        p_entity,
        p_comp_node,
        method,
        args,
        txn,
        p_sender,
        transfer_generation
    );
}

Error NetwMultiplayer::send_row_to(
    int64_t p_peer,
    int64_t p_route,
    int64_t p_channel,
    const PackedByteArray &p_payload,
    bool p_reliable,
    int64_t p_comp,
    const CarrierRow *p_row
) {
    return send_frame(
        p_peer,
        p_route,
        p_channel,
        p_payload,
        p_reliable,
        p_comp,
        String(),
        nullptr,
        p_row,
        true
    );
}

Error NetwMultiplayer::send_to(
    int64_t p_peer,
    int64_t p_route,
    int64_t p_channel,
    const PackedByteArray &p_payload,
    bool p_reliable,
    int64_t p_comp,
    const String &p_path
) {
    return send_frame(
        p_peer,
        p_route,
        p_channel,
        p_payload,
        p_reliable,
        p_comp,
        p_path,
        nullptr,
        nullptr,
        false
    );
}

Error NetwMultiplayer::send_to(
    int64_t p_peer,
    int64_t p_route,
    int64_t p_channel,
    const PackedByteArray &p_payload,
    bool p_reliable,
    int64_t p_comp,
    const String &p_path,
    wire::Delivery p_delivery
) {
    return send_frame(
        p_peer,
        p_route,
        p_channel,
        p_payload,
        p_reliable,
        p_comp,
        p_path,
        &p_delivery,
        nullptr,
        false
    );
}

Error NetwMultiplayer::rpc_send(
    int64_t p_peer,
    int64_t p_channel,
    const PackedByteArray &p_payload,
    bool p_reliable,
    bool p_batched
) {
    if (p_channel < CHANNEL_USER_FIRST || p_channel > CHANNEL_USER_LAST) {
        fault::raise(
            this,
            NETW_FAULT_BAD_VALUE,
            {String("{NetwMultiplayer.rpc_send}"),
             vformat("channel id %d", int(p_channel)),
             vformat("an id from %d to %d",
                     int(CHANNEL_USER_FIRST),
                     int(CHANNEL_USER_LAST))}
        );
        return ERR_INVALID_PARAMETER;
    }
    return send_to(
        p_peer,
        0,
        p_channel,
        p_payload,
        p_reliable,
        0,
        String(),
        p_batched ? wire::Delivery::TICK : wire::Delivery::IMMEDIATE
    );
}

void NetwMultiplayer::charge_frame(
    int64_t p_peer,
    int64_t p_channel,
    int64_t p_bytes
) {
    if (p_peer <= 0
        || (p_channel >= wire::CHANNEL_CLOCK_HANDSHAKE
            && p_channel <= wire::CHANNEL_CLOCK_PONG)) {
        return;
    }
    ReplicationCore *plane = get_replication_plane();
    SyncPipeline *pipeline
        = plane != nullptr ? plane->get_sync_pipeline() : nullptr;
    ReplicationSend *send
        = pipeline != nullptr ? pipeline->row_send() : nullptr;
    if (send != nullptr) {
        send->charge(p_peer, p_bytes);
    }
}

Error NetwMultiplayer::send_frame(
    int64_t p_peer,
    int64_t p_route,
    int64_t p_channel,
    const PackedByteArray &p_payload,
    bool p_reliable,
    int64_t p_comp,
    const String &p_path,
    const wire::Delivery *p_delivery,
    const CarrierRow *p_row,
    bool p_charged
) {
    NETW_ZONE_LEAF_F(
        "Send frame",
        colors::TRANSPORT,
        profile::SUBSYSTEM_WIRE,
        "entity %lld peer %lld",
        static_cast<long long>(p_route),
        static_cast<long long>(p_peer)
    );
    NETW_BUG_COND_V(p_route < 0, ERR_INVALID_PARAMETER);

    const int64_t subject_route = world.wire.take_subject(p_route);

    const bool connected
        = inner.is_valid() && inner->get_multiplayer_peer().is_valid();
    if (connected && p_peer == get_unique_id()) {
        ReplicationCore *plane = get_replication_plane();
        if (plane == nullptr) {
            fault::raise(
                this,
                NETW_FAULT_NOT_RUNNING,
                {vformat("Sending on channel %d", int(p_channel)),
                 String(),
                 "replication"}
            );
            return ERR_UNCONFIGURED;
        }
        NETW_TRACE(
            sys::TRANSPORT,
            "Delivered locally for route %d on channel %d.",
            int(p_route),
            int(p_channel)
        );
        plane->dispatch(
            p_route,
            p_comp,
            p_channel,
            p_payload,
            p_path,
            p_peer,
            p_reliable,
            -1
        );
        return OK;
    }

    wire::Wire &link = world.wire;
    const PackedByteArray framed = wire::frame_pack(
        link.frame_stream,
        p_route,
        uint8_t(p_comp),
        uint8_t(p_channel),
        p_payload,
        p_path
    );
    if (!p_charged) {
        charge_frame(p_peer, p_channel, framed.size());
    }

    wire::Attribution outgoing;
    outgoing.peer = p_peer;
    outgoing.channel = p_channel;
    outgoing.route = subject_route;
    outgoing.comp = p_comp;
    outgoing.bits = int64_t(framed.size()) * 8;
#if defined(DEBUG_ENABLED)
    if (network_profiler.active()) {
        outgoing.subject = profiler_outgoing != wire::NetworkProfiler::NONE
            ? profiler_outgoing
            : profiler_frame_subject(subject_route, p_comp, p_channel, p_path);
        if (outgoing.subject == wire::NetworkProfiler::SKIP) {
            outgoing.subject = wire::NetworkProfiler::NONE;
        }
        outgoing.epoch = network_profiler.epoch();
    }
#endif

    const wire::Delivery delivery = link.channel_delivery(p_channel, p_delivery);
    const int64_t deadline = p_reliable ? 0 : wire::ride_deadline_ms(delivery);
    if (p_reliable && wire::ride_deadline_ms(delivery) > 0) {
        NETW_BUG_REPORT();
    }
    if (delivery == wire::Delivery::IMMEDIATE) {
        if (p_reliable) {
            carrier_dispose(p_peer, true, link.carrier.take(p_peer, true));
        }
        link.attribution.stage(p_reliable, false, outgoing);
        CarrierBatch immediate;
        immediate.take_frame(framed);
        if (p_row != nullptr) {
            immediate.attach(*p_row);
        }
        carrier_dispose(p_peer, p_reliable, immediate, false);
        return OK;
    }

    carrier_append(p_peer, framed, p_reliable, p_row, deadline);
    link.attribution.stage(p_reliable, true, outgoing);
    return OK;
}

int64_t NetwMultiplayer::send_datagram(
    int64_t p_peer,
    const PackedByteArray &p_payload,
    bool p_reliable,
    bool p_carrier,
    bool *r_sent
) {
    wire::AttributionBook &attribution = world.wire.attribution;
    if (r_sent != nullptr) {
        *r_sent = false;
    }
    if (p_payload.is_empty() || !session_link_is_connected()) {
        attribution.discard(p_peer, p_reliable, p_carrier);
        return -1;
    }

    if (p_peer != 0 && reachable_peer_ids().find(int32_t(p_peer)) < 0) {
        attribution.discard(p_peer, p_reliable, p_carrier);
        return -1;
    }

    NetwCarrierDatagram datagram = world.wire.frame_datagram(
        p_peer,
        p_payload,
        p_reliable,
        p_carrier,
        wire_stamp()
#if defined(DEBUG_ENABLED)
        ,
        network_profiler.active()
#endif
    );
#if defined(NETW_TESTS)
    const int64_t before_transport = store::allocations_seen();
#endif
    const Error delivered = inner->send_bytes(
        datagram.bytes,
        int(p_peer),
        p_reliable ? MultiplayerPeer::TRANSFER_MODE_RELIABLE
                   : MultiplayerPeer::TRANSFER_MODE_UNRELIABLE,
        0
    );
#if defined(NETW_TESTS)
    idle_work.transport_allocations
        += store::allocations_seen() - before_transport;
#endif
#if defined(DEBUG_ENABLED)
    if (delivered == OK) {
        for (const wire::Attribution &frame : datagram.observations) {
            network_profiler.observe(
                frame.subject,
                frame.epoch,
                p_peer,
                false,
                frame.bits / 8
            );
        }
        profiler_datagram(
            false,
            p_peer,
            datagram.bytes.size() - p_payload.size()
        );
    }
    if (!datagram.observations.is_empty()) {
        attribution.recycle(
            p_peer,
            p_reliable,
            p_carrier,
            std::move(datagram.observations)
        );
    }
#endif
    if (delivered != OK && p_peer != 0) {
        if (session_link_is_connected()) {
            peer_mark_unreachable(p_peer);
        }
        attribution.discard(p_peer, p_reliable, p_carrier);
        return -1;
    }
    if (r_sent != nullptr) {
        *r_sent = true;
    }
    attribution.note_datagram(false, p_peer, int64_t(datagram.bytes.size()));
    capture_note(wire::CaptureDirection::OUT, p_peer, datagram.bytes);
    event_record(
        EventPlane::DATAGRAM_SENT,
        RID(),
        0,
        p_peer,
        OK,
        datagram.bytes.size(),
        datagram.seq
    );
    return datagram.seq;
}

int64_t NetwMultiplayer::carrier_append(
    int64_t p_peer,
    const PackedByteArray &p_frame,
    bool p_reliable,
    const CarrierRow *p_row,
    int64_t p_deadline_ms
) {
    const CarrierBatch owed = world.wire.carrier.append(
        p_peer,
        p_frame,
        p_reliable,
        p_reliable ? 0 : datagram_budget(),
        p_row,
        p_deadline_ms
    );
    return carrier_dispose(p_peer, p_reliable, owed);
}

int64_t NetwMultiplayer::carrier_dispose(
    int64_t p_peer,
    bool p_reliable,
    const CarrierBatch &p_batch,
    bool p_carrier
) {
    if (p_batch.is_empty()) {
        return -1;
    }
    bool sent = false;
    int64_t seq = -1;
    seq = send_datagram(p_peer, p_batch.bytes(), p_reliable, p_carrier, &sent);
    if (!sent) {
        carrier_cancel(p_peer, p_batch);
        return -1;
    }
    carrier_settle(p_peer, seq, p_batch);
    return seq;
}

void NetwMultiplayer::carrier_settle(
    int64_t p_peer,
    int64_t p_seq,
    const CarrierBatch &p_batch
) {
    ReplicationCore *plane = get_replication_plane();
    if (plane == nullptr) {
        return;
    }
    SyncPipeline *pipeline = plane->get_sync_pipeline();
    if (pipeline != nullptr) {
        pipeline->open_carrier_batch(
            p_peer,
            &p_batch.rows(),
            p_batch.frame_count(),
            p_batch.bit_count()
        );
    }
    plane->note_staged(p_peer, p_seq);
    if (pipeline != nullptr) {
        pipeline->close_carrier_batch();
    }
}

void NetwMultiplayer::carrier_cancel(
    int64_t p_peer,
    const CarrierBatch &p_batch
) {
    ReplicationCore *plane = get_replication_plane();
    SyncPipeline *pipeline
        = plane != nullptr ? plane->get_sync_pipeline() : nullptr;
    if (pipeline == nullptr) {
        return;
    }
    pipeline->open_carrier_batch(
        p_peer,
        &p_batch.rows(),
        p_batch.frame_count(),
        p_batch.bit_count()
    );
    pipeline->close_carrier_batch();
}

PackedInt64Array NetwMultiplayer::carrier_flush(
    int64_t p_now_ms,
    bool p_upkeep_rides
) {
#if defined(NETW_TESTS)
    store::AllocationTally flushing(idle_work.carrier_allocations);
#endif
    NetwCarrierBuffers &carrier = world.wire.carrier;
    PackedInt64Array staged;
    clock_board(p_now_ms, p_upkeep_rides);
    for (const int32_t peer : carrier.peers(false)) {
        if (p_upkeep_rides && carrier.rides(peer, p_now_ms)) {
            continue;
        }
        NETW_STAGE(FLUSH, "Flush unreliable datagram", colors::TRANSPORT);
        NETW_ZONE_TEXT_F("peer %d", int(peer));
        CarrierBatch batch = carrier.take(peer, false);
        const int64_t seq = carrier_dispose(peer, false, batch);
        carrier.recycle(peer, false, batch);
        if (seq >= 0) {
            staged.push_back(peer);
            staged.push_back(seq);
        }
    }
    for (const int32_t peer : carrier.peers(true)) {
        NETW_STAGE(FLUSH, "Flush reliable datagram", colors::TRANSPORT);
        NETW_ZONE_TEXT_F("peer %d", int(peer));
        CarrierBatch batch = carrier.take(peer, true);
        carrier_dispose(peer, true, batch);
        carrier.recycle(peer, true, batch);
    }
    world.wire.attribution.settle_retired_routes();
    return staged;
}

void NetwMultiplayer::carrier_clear() {
    NetwCarrierBuffers &carrier = world.wire.carrier;
    wire::AttributionBook &attribution = world.wire.attribution;
    for (const int32_t peer : carrier.peers(false)) {
        carrier_cancel(peer, carrier.take(peer, false));
        attribution.discard(peer, false, true);
    }
    for (const int32_t peer : carrier.peers(true)) {
        carrier_cancel(peer, carrier.take(peer, true));
        attribution.discard(peer, true, true);
    }
    carrier.clear();
}

wire::Wire::Stamp NetwMultiplayer::wire_stamp() const {
    wire::Wire::Stamp stamp;
    stamp.now_usec = Time::get_singleton()->get_ticks_usec();
    stamp.elapsed_ms = session_elapsed_ms();
    stamp.base_tick = datagram_base_tick();
    return stamp;
}

int64_t NetwMultiplayer::datagram_base_tick() const {
    return int64_t(clock_engine().get_send_tick());
}

int64_t NetwMultiplayer::session_elapsed_ms() const {
    const int64_t rate = int64_t(clock_engine().get_tickrate());
    if (rate <= 0) {
        return int64_t(Time::get_singleton()->get_ticks_msec());
    }
    return int64_t(clock_engine().get_tick()) * 1000 / rate;
}

bool NetwMultiplayer::note_peer_ack(
    int64_t p_peer,
    int64_t p_ack,
    uint32_t p_history,
    bool p_echo_waited
) {
    const bool advanced = world.wire.seq_book.note_peer_ack(
        p_peer,
        uint16_t(p_ack),
        p_history,
        Time::get_singleton()->get_ticks_usec(),
        p_echo_waited
    );
    if (advanced) {
        event_record(EventPlane::ACK_ADVANCED, RID(), 0, p_peer, OK, p_ack);
    }
    return advanced;
}

void NetwMultiplayer::note_ack(int64_t p_peer, int64_t p_sequence) {
    ReplicationCore *plane = (get_replication_plane());
    if (plane == nullptr) {
        NETW_TRACE(
            sys::SESSION,
            "A received acknowledgement was dropped, because replication is "
            "not running."
        );
        return;
    }
    SyncPipeline *pipeline = plane->get_sync_pipeline();
    if (pipeline == nullptr) {
        NETW_TRACE(
            sys::SESSION,
            "A received acknowledgement was dropped, because sync is not "
            "running."
        );
        return;
    }
    pipeline->note_peer_ack(
        p_peer,
        p_sequence,
        world.wire.seq_book.peer_ack_history(p_peer)
    );
}

void NetwMultiplayer::session_note_state_ack(
    int64_t p_peer,
    int64_t p_ack,
    uint32_t p_history,
    bool p_echo_waited
) {
    if (!note_peer_ack(p_peer, p_ack, p_history, p_echo_waited)) {
        return;
    }
    note_ack(p_peer, p_ack);
}

Error NetwMultiplayer::session_receive_inner_packet(
    int64_t p_sender,
    const PackedByteArray &p_packet
) {
    const NetwCarrierFrame header = receive_header(p_sender, p_packet);
    if (header.kind == NetwCarrierFrame::FOREIGN) {
        return OK;
    }
    if (header.kind == NetwCarrierFrame::MALFORMED) {
        return ERR_INVALID_DATA;
    }
    const PackedByteArray framed = p_packet.slice(int(header.payload_offset));
    if (header.kind == NetwCarrierFrame::RELIABLE) {
        return receive_carrier(framed, p_sender, true, -1, header.tick);
    }
    if (header.is_acked()) {
        world.wire.count_state_ack_in();
        session_note_state_ack(
            p_sender,
            header.ack,
            header.history,
            header.kind == NetwCarrierFrame::UNRELIABLE_ACKED_HELD
        );
    }
    world.wire.seq_book.note_inbound(
        p_sender,
        uint16_t(header.seq),
        session_elapsed_ms(),
        !framed.is_empty()
    );
    const Error verdict
        = receive_carrier(framed, p_sender, false, header.seq, header.tick);
    if (world.wire.seq_book.echo_is_over_limit(p_sender)
        && session_link_is_connected()
        && reachable_peer_ids().has(int32_t(p_sender))) {
        send_echo(p_sender);
    }
    return verdict;
}

void NetwMultiplayer::session_on_inner_packet(
    int64_t p_sender,
    const PackedByteArray &p_packet
) {
    sink_verdict(session_receive_inner_packet(p_sender, p_packet), 0, p_sender);
}

void NetwMultiplayer::note_sent(int64_t p_peer, int64_t p_sequence) {
    ReplicationCore *plane = (get_replication_plane());
    if (plane == nullptr) {
        NETW_TRACE(
            sys::SESSION,
            "A sent acknowledgement was not recorded, because replication is "
            "not running."
        );
        return;
    }
    SyncPipeline *pipeline = plane->get_sync_pipeline();
    if (pipeline == nullptr) {
        NETW_TRACE(
            sys::SESSION,
            "A sent acknowledgement was not recorded, because sync is not "
            "running."
        );
        return;
    }
    pipeline->commit_pending_masked(p_peer, p_sequence);
}

bool NetwMultiplayer::count_verdict(Error p_verdict, int64_t p_route) {
    const bool counted = verdict_book.count(p_verdict);
    event_record(EventPlane::VERDICT, RID(), p_route, 0, p_verdict);
    return counted;
}

void NetwMultiplayer::send_standalone_ack(int64_t p_peer, int64_t p_ack) {
    const PackedByteArray framed
        = world.wire.standalone_ack(p_peer, p_ack, wire_stamp());
    capture_note(wire::CaptureDirection::OUT, p_peer, framed);
    const Error delivered = inner->send_bytes(
        framed,
        int32_t(p_peer),
        MultiplayerPeer::TRANSFER_MODE_UNRELIABLE,
        0
    );
#if defined(DEBUG_ENABLED)
    if (delivered == OK) {
        network_profiler.observe(
            network_profiler.ack_subject(),
            network_profiler.epoch(),
            p_peer,
            false,
            framed.size()
        );
        profiler_datagram(false, p_peer, 0);
    }
#endif
    if (delivered != OK && session_link_is_connected()) {
        peer_mark_unreachable(p_peer);
    }
}

void NetwMultiplayer::flush_standalone_acks(
    int64_t p_now_ms,
    bool p_upkeep_rides
) {
    if (!session_link_is_connected()) {
        return;
    }
    const PackedInt32Array live = reachable_peer_ids();
    DatagramSeqBook &seqs = world.wire.seq_book;
    const PackedInt64Array owed = seqs.peers_owed_echo();
    for (int at = 0; at < owed.size(); at++) {
        const int64_t peer = owed[at];
        if (peer != 0 && !live.has(int32_t(peer))) {
            continue;
        }
        if (p_upkeep_rides && !seqs.echo_is_due(peer, p_now_ms)) {
            seqs.note_echo_held(peer);
            continue;
        }
        send_echo(peer);
    }
}

void NetwMultiplayer::send_echo(int64_t p_peer) {
    if (p_peer == SERVER_PEER && world.clock.exchange.ping_is_owed()) {
        clock_board_ping();
    }
    NetwCarrierBuffers &carrier = world.wire.carrier;
    if (carrier.pending(p_peer, false) > 0) {
        CarrierBatch batch = carrier.take(p_peer, false);
        carrier_dispose(p_peer, false, batch);
        carrier.recycle(p_peer, false, batch);
        return;
    }
    send_standalone_ack(p_peer, world.wire.seq_book.inbound_seq(p_peer));
}

Error NetwMultiplayer::send_bytes(
    const PackedByteArray &p_bytes,
    int64_t p_peer,
    MultiplayerPeer::TransferMode p_mode,
    int p_channel
) {
    if (inner.is_null()) {
        return ERR_UNCONFIGURED;
    }
    capture_note(wire::CaptureDirection::OUT, p_peer, p_bytes);
    return inner->send_bytes(p_bytes, int32_t(p_peer), p_mode, p_channel);
}

} // namespace netw
