#include "support/fault_log.h"
#include "support/loopback_rig.h"
#include "support/netw_cells.h"
#include "support/netw_recorder.h"
#include "support/store_reader.h"

#include <cstdint>
#include <initializer_list>

#include "godot/node.hpp"
#include "godot/variant.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/quantize.hpp"
#include "netw/api/schema_core.hpp"
#include "netw/call_args.hpp"
#include "netw/carrier_frame.hpp"
#include "netw/comp_table.hpp"
#include "netw/datagram_seq_book.hpp"
#include "netw/rate_window.hpp"
#include "netw/repl/snapshot_frame.hpp"
#include "netw/store/world.hpp"
#include "netw/wire/ack_book.hpp"
#include "netw/wire/code_row.hpp"
#include "netw/wire/frame.hpp"
#include "netw/wire/plan.hpp"
#include "netw/wire/registry.hpp"
#include "netw/wire/value_row.hpp"

#if defined(NETW_TIER_HOSTED)

namespace TestNetwCarrier {

using namespace godot;
using netw::NetwCarrierFrame;
using netw::NetwCompTable;
using netw::NetwMultiplayer;
using netw::SchemaCore;
using netw::call_args::Slot;
using netw::repl::SnapshotHeader;
using netw::repl::write_snapshot_row;
using netw::store::Landing;
using netw::table::SchemaRecord;
using netw::wire::CodeRow;
using netw::wire::FRAME_COMP_PATH;
using netw::wire::frame_pack;
using netw::wire::frame_unpack_all;
using netw::wire::FrameWalk;
using netw::wire::WirePlan;
using netw_test::law_broken;
using netw_test::law_held;
using netw_test::LawVerdict;
using netw_test::Recorder;

struct Carrier {
    String label;
};

typedef netw_test::LawRowFor<Carrier> Row;

constexpr int PEER = 4;

PackedByteArray bytes(const std::initializer_list<uint8_t> &p_values) {
    PackedByteArray out;
    for (const uint8_t value : p_values) {
        out.push_back(value);
    }
    return out;
}

PackedByteArray joined(
    const PackedByteArray &p_first,
    const PackedByteArray &p_second
) {
    PackedByteArray out = p_first;
    out.append_array(p_second);
    return out;
}

PackedByteArray endless_varint() {
    return bytes({0xff, 0xff, 0xff, 0xff, 0xff});
}

LawVerdict walks(
    const PackedByteArray &p_framed,
    bool p_whole,
    const std::initializer_list<int64_t> &p_routes
) {
    const FrameWalk walk = frame_unpack_all(p_framed, 0);
    if (walk.whole != p_whole) {
        return law_broken("the walk read whole=%d", int(walk.whole));
    }
    if (walk.frames.size() != p_routes.size()) {
        return law_broken("the walk read %d frames", int(walk.frames.size()));
    }
    uint32_t at = 0;
    for (const int64_t route : p_routes) {
        if (walk.frames[at].route != route) {
            return law_broken(
                "frame %d read route %lld",
                int(at),
                (long long)walk.frames[at].route
            );
        }
        at += 1;
    }
    return law_held();
}

LawVerdict endless_route(const Carrier &) {
    return walks(endless_varint(), false, {});
}

LawVerdict endless_length(const Carrier &) {
    return walks(
        joined(
            frame_pack(1, 0, 7, bytes({9}), String()),
            joined(bytes({2, 0, 8}), endless_varint())
        ),
        false,
        {1}
    );
}

LawVerdict path_past_body(const Carrier &) {
    return walks(
        joined(bytes({1, FRAME_COMP_PATH, 3, 5}), endless_varint()),
        false,
        {}
    );
}

LawVerdict header_cut(const Carrier &) {
    return walks(bytes({0x07}), false, {});
}

LawVerdict body_past_datagram(const Carrier &) {
    const PackedByteArray framed
        = frame_pack(1, 0, 0, bytes({1, 2, 3, 4}), String());
    return walks(framed.slice(0, 6), false, {});
}

LawVerdict trailing_partial(const Carrier &) {
    return walks(
        joined(
            joined(
                frame_pack(1, 0, 7, bytes({0xa1}), String()),
                frame_pack(2, 0, 8, bytes({0xb1}), String())
            ),
            bytes({0x07})
        ),
        false,
        {1, 2}
    );
}

LawVerdict redundant_route(const Carrier &) {
    const LawVerdict canonical
        = walks(bytes({0xac, 0x02, 0x00, 0x13, 0x00}), true, {300});
    if (!canonical.held) {
        return canonical;
    }
    return walks(bytes({0xac, 0x82, 0x00, 0x00, 0x13, 0x00}), false, {});
}

LawVerdict pathed_frame(const Carrier &) {
    const FrameWalk named = frame_unpack_all(
        frame_pack(2, FRAME_COMP_PATH, 5, bytes({7, 8}), String("Body/Gun")),
        0
    );
    if (!named.whole || named.frames.size() != 1
        || named.frames[0].path != String("Body/Gun")
        || named.frames[0].payload != bytes({7, 8})) {
        return law_broken("a named path did not come back beside its payload");
    }
    const FrameWalk unnamed = frame_unpack_all(
        frame_pack(1, FRAME_COMP_PATH, 2, bytes({5, 6}), String()),
        0
    );
    if (!unnamed.whole || unnamed.frames.size() != 1
        || !unnamed.frames[0].path.is_empty()
        || unnamed.frames[0].payload != bytes({5, 6})) {
        return law_broken("an empty path took bytes from the payload");
    }
    return law_held();
}

struct Heard {
    int announced = 0;
    Array args;
    int64_t ack = -1;
};

Heard hear(const PackedByteArray &p_datagram) {
    Ref<NetwMultiplayer> session;
    session.instantiate();
    Recorder recorder(session.ptr(), Vector<StringName>({"peer_packet"}));
    session->session_get_inner()->emit_signal(
        "peer_packet",
        PEER,
        p_datagram
    );
    Heard out;
    out.announced = recorder.count("peer_packet");
    if (out.announced > 0) {
        out.args = recorder.args("peer_packet");
    }
    out.ack = session->peer_ack(PEER);
    return out;
}

LawVerdict announced_as_sent(const PackedByteArray &p_datagram) {
    const Heard heard = hear(p_datagram);
    if (heard.announced != 1 || heard.args.size() != 2) {
        return law_broken("peer_packet fired %d times", heard.announced);
    }
    if (int(heard.args[0]) != PEER
        || PackedByteArray(heard.args[1]) != p_datagram) {
        return law_broken("peer_packet carried another peer or other bytes");
    }
    return law_held();
}

LawVerdict unannounced(const PackedByteArray &p_datagram) {
    const Heard heard = hear(p_datagram);
    if (heard.announced != 0) {
        return law_broken("peer_packet fired %d times", heard.announced);
    }
    return law_held();
}

LawVerdict game_bytes(const Carrier &) {
    const LawVerdict short_one = announced_as_sent(bytes({0x01}));
    if (!short_one.held) {
        return short_one;
    }
    return announced_as_sent(bytes({0x11, 0x11, 0x11, 0x11, 0x11, 0x11}));
}

LawVerdict acknowledging(const Carrier &) {
    const Heard heard
        = hear(NetwCarrierFrame::build(PackedByteArray(), false, 11, 900, 0, -1)
        );
    if (heard.announced != 0) {
        return law_broken("our own datagram reached peer_packet");
    }
    if (heard.ack != 900) {
        return law_broken("the peer's ack reads %lld", (long long)heard.ack);
    }
    return law_held();
}

LawVerdict cut_header(const Carrier &) {
    netw_test::FaultWatch faults;
    faults.expect_warning("*[NW0007]*", 3);
    const LawVerdict unacked
        = unannounced(bytes({NetwCarrierFrame::MAGIC_UNRELIABLE, 0}));
    if (!unacked.held) {
        return unacked;
    }
    const LawVerdict acked
        = unannounced(bytes({NetwCarrierFrame::MAGIC_UNRELIABLE_ACKED, 0, 0, 0})
        );
    if (!acked.held) {
        return acked;
    }
    return unannounced(PackedByteArray());
}

LawVerdict retired_magic(const Carrier &) {
    const uint8_t retired[] = {0x4E, 0x6E, 0x8E, 0x56, 0x76, 0x96};
    netw_test::FaultWatch faults;
    faults.expect_warning("*[NW0007]*", int64_t(sizeof(retired)));
    for (const uint8_t magic : retired) {
        PackedByteArray datagram;
        datagram.resize(12);
        datagram.fill(0);
        datagram.set(0, magic);
        const LawVerdict verdict = unannounced(datagram);
        if (!verdict.held) {
            return verdict;
        }
    }
    return law_held();
}

LawVerdict round_trip_per_peer(const Carrier &) {
    netw::DatagramSeqBook book;
    const uint16_t to_near = book.next_send_seq(2, 1000000);
    const uint16_t to_far = book.next_send_seq(3, 1000000);
    book.note_peer_ack(2, to_near, 0, 1040000);
    book.note_peer_ack(3, to_far, 0, 1340000);
    if (int64_t(book.round_trip_ms(2)) != 40
        || int64_t(book.round_trip_ms(3)) != 340
        || int64_t(book.round_trip_ms(4)) != 0) {
        return law_broken(
            "round trips read %g, %g and %g",
            book.round_trip_ms(2),
            book.round_trip_ms(3),
            book.round_trip_ms(4)
        );
    }
    return law_held();
}

Node *build_body(const Variant &) {
    Node *made = memnew(Node);
    made->set_name("Body");
    return made;
}

LawVerdict spawn_refused(PackedByteArray (*p_damage)(const PackedByteArray &)
) {
    netw_test::FaultWatch faults;
    faults.expect_warning("*[NW0007]*", 1);
    netw_test::LoopbackRig rig(1);
    rig.mount();
    Node *arena = rig.mirror_child("Arena");
    rig.hold(0);
    Array args;
    args.push_back(String("body"));
    Array types;
    types.push_back(int(Variant::STRING));
    const int route = rig.spawn_registered(
        StringName("carrier_body"),
        callable_mp_static(&build_body),
        args,
        types,
        arena,
        0,
        false
    );
    rig.pump(10);
    const PackedByteArray frame = rig.spawn_frame_of(route);
    if (frame.size() <= 4) {
        return law_broken("the spawn frame is %d bytes", int(frame.size()));
    }
    rig.deliver_spawn(0, p_damage(frame));
    if (rig.route_node(route, 0) != nullptr) {
        return law_broken("the client built the node");
    }
    return law_held();
}

PackedByteArray with_extra_field(const PackedByteArray &p_frame) {
    PackedByteArray out = p_frame.slice(0, 4);
    out.push_back(0x01);
    out.append_array(p_frame.slice(4, p_frame.size()));
    return out;
}

PackedByteArray without_tail(const PackedByteArray &p_frame) {
    return p_frame.slice(0, p_frame.size() - 1);
}

LawVerdict spawn_extra_field(const Carrier &) {
    return spawn_refused(&with_extra_field);
}

LawVerdict spawn_cut_short(const Carrier &) {
    return spawn_refused(&without_tail);
}

LawVerdict call_cut_short(const Carrier &) {
    LocalVector<Slot> slots;
    slots.push_back(netw::call_args::of_value(int64_t(5)));
    slots.push_back(netw::call_args::of_value(true));
    netw::wire::WriteStream stream;
    if (!netw::call_args::write(stream, slots, Array(), Array())
        || !stream.align_verify()) {
        return law_broken("the argument row did not write");
    }
    const PackedByteArray whole = stream.to_bytes();

    netw::wire::ReadStream cut(whole.slice(0, whole.size() - 2));
    LocalVector<Slot> partial;
    if (netw::call_args::read(cut, Array(), Array(), partial)
        || partial.size() != 0) {
        return law_broken("a cut row read %d arguments", int(partial.size()));
    }
    netw::wire::ReadStream sound(whole);
    LocalVector<Slot> read;
    if (!netw::call_args::read(sound, Array(), Array(), read)
        || read.size() != 2 || int64_t(read[0].value) != 5
        || !bool(read[1].value)) {
        return law_broken("the whole row did not read back");
    }
    return law_held();
}

constexpr int64_t BASE_TICK = 100;
constexpr uint64_t TOKEN = 77;

SchemaRecord counter_schema() {
    SchemaRecord record;
    record.name = StringName("Counter");
    SchemaCore::append_column(&record, StringName("count"), SchemaCore::I16, 1);
    record.at(0)->delta = netw::table::DeltaMode::LADDER;
    SchemaCore::fix(&record);
    return record;
}

CodeRow counted(const WirePlan &p_plan, uint64_t p_count) {
    CodeRow row = CodeRow::for_plan(p_plan);
    row.write(p_plan.column(0), 0, p_count);
    return row;
}

PackedByteArray snapshot(
    const WirePlan &p_plan,
    uint64_t p_count,
    uint64_t p_revision,
    int64_t p_tick,
    const CodeRow *p_base = nullptr,
    uint64_t p_distance = 0
) {
    const CodeRow row = counted(p_plan, p_count);
    SnapshotHeader header;
    header.token = TOKEN;
    header.revision = p_revision;
    header.tick = p_tick;
    if (p_base != nullptr) {
        header.distance = p_distance;
        header.mask = CodeRow::changed_mask(p_plan, *p_base, row);
    }
    return write_snapshot_row(header, BASE_TICK, p_plan, row, p_base);
}

LawVerdict partial_decode_writes_nothing() {
    SchemaRecord schema;
    schema.name = StringName("Partial");
    SchemaCore::append_column(&schema, StringName("a"), SchemaCore::I32, 1);
    SchemaCore::append_column(&schema, StringName("b"), SchemaCore::BOOL, 1);
    SchemaCore::append_column(&schema, StringName("c"), SchemaCore::I32, 1);
    Ref<netw::NetwQuantizeScalar> packer;
    packer.instantiate();
    packer->bits(8);
    packer->limits(0.0, 1.0);
    schema.at(1)->quantizer = packer;
    SchemaCore::fix(&schema);
    const WirePlan plan = WirePlan::compile(schema);
    if (!plan.valid()) {
        return law_broken("the partial schema did not compile");
    }
    const CodeRow row = CodeRow::for_plan(plan);
    Array out;
    out.push_back(StringName("sentinel"));
    ERR_PRINT_OFF;
    const bool decoded = netw::wire::decode_scalar_row(schema, row, out);
    ERR_PRINT_ON;
    if (decoded || out.size() != 1 || String(out[0]) != String("sentinel")) {
        return law_broken("a decode that failed partway wrote the row");
    }
    return law_held();
}

LawVerdict refused_snapshot(const Carrier &) {
    netw_test::StoreReader reader;
    reader.declare(counter_schema(), 7, BASE_TICK);
    const WirePlan &plan = reader.held().plan;
    const int sender = 5;
    if (reader.land(sender, snapshot(plan, 9, 2, 101)) != Landing::ACCEPTED) {
        return law_broken("the first frame was not accepted");
    }
    const Variant value = reader.held().read_value(reader.at.row, 0);
    const int64_t newest = reader.held().history_tick(reader.at.row, 0);
    const CodeRow base = counted(plan, 9);
    PackedByteArray torn = snapshot(plan, 4, 7, 102);
    torn.resize(torn.size() - 1);
    const PackedByteArray refused[] = {
        snapshot(plan, 4, 1, 102),
        snapshot(plan, 4, 2, 102),
        snapshot(plan, 4, 6, 102, &base, 3),
        torn,
    };
    for (const PackedByteArray &frame : refused) {
        if (reader.land(sender, frame) == Landing::ACCEPTED) {
            return law_broken("a frame the receiver should refuse landed");
        }
    }
    if (reader.held().read_value(reader.at.row, 0) != value
        || reader.held().history_tick(reader.at.row, 0) != newest) {
        return law_broken("a refused frame moved the received value");
    }
    return partial_decode_writes_nothing();
}

constexpr int64_t NOW = 1000000;

LawVerdict whole_budget(const Carrier &) {
    netw::RateWindow window;
    for (int attempt = 0; attempt < window.limit; ++attempt) {
        if (window.exceeded(7, NOW)) {
            return law_broken("request %d inside the budget refused", attempt);
        }
    }
    if (!window.exceeded(7, NOW)) {
        return law_broken("the request past the budget was admitted");
    }
    return law_held();
}

LawVerdict span_expires(const Carrier &) {
    netw::RateWindow window;
    for (int attempt = 0; attempt <= window.limit; ++attempt) {
        window.exceeded(7, NOW);
    }
    if (!window.exceeded(7, NOW + window.span_msec - 1)) {
        return law_broken("the flood was forgiven before the span ended");
    }
    if (window.exceeded(7, NOW + window.span_msec + 1)) {
        return law_broken("the flood was still refused after the span");
    }
    return law_held();
}

LawVerdict span_slides(const Carrier &) {
    netw::RateWindow window;
    const int64_t gap = window.span_msec * 2 / window.limit;
    for (int step = 0; step < 60; ++step) {
        if (window.exceeded(7, NOW + int64_t(step) * gap)) {
            return law_broken("a steady sender was refused at step %d", step);
        }
    }
    return law_held();
}

NetwCompTable comp_table(const std::initializer_list<const char *> &p_paths) {
    PackedStringArray paths;
    for (const char *path : p_paths) {
        paths.push_back(String(path));
    }
    NetwCompTable out;
    out.assign(paths);
    return out;
}

LawVerdict registration_order(const Carrier &) {
    const NetwCompTable first = comp_table({"Gun", "Body", "Wheel"});
    const NetwCompTable second = comp_table({"Wheel", "Gun", "Body"});
    for (int64_t id = 1; id <= 3; ++id) {
        if (first.path_for_id(id).is_empty()
            || first.path_for_id(id) != second.path_for_id(id)) {
            return law_broken("id %lld names different paths", (long long)id);
        }
    }
    return law_held();
}

LawVerdict digest_disagrees(const Carrier &) {
    NetwCompTable authority = comp_table({"Gun"});
    authority.table_hash = 4242;
    authority.reconcile(true);
    if (authority.wire_hash != 4242 || authority.poisoned) {
        return law_broken("the authority did not author its digest");
    }
    NetwCompTable agreeing = comp_table({"Gun"});
    agreeing.table_hash = 4242;
    agreeing.wire_hash = 4242;
    agreeing.reconcile(false);
    if (agreeing.classify(1, String()) != NetwCompTable::ADDRESS_MAPPED) {
        return law_broken("an agreeing peer lost its compact ids");
    }
    NetwCompTable diverged = comp_table({"Gun"});
    diverged.table_hash = 4242;
    diverged.wire_hash = 9999;
    diverged.reconcile(false);
    if (diverged.classify(1, String()) != NetwCompTable::ADDRESS_UNMAPPED) {
        return law_broken("a disagreeing peer still trusts compact ids");
    }
    if (diverged.classify(0, String()) != NetwCompTable::ADDRESS_ROOT
        || diverged.classify(FRAME_COMP_PATH, String("Gun"))
            != NetwCompTable::ADDRESS_RELATIVE) {
        return law_broken("a disagreeing peer lost the root or a path");
    }
    return law_held();
}

struct Declared {
    const char *name;
    int64_t id;
};

const Declared TRANSCRIBED[] = {
    {"ACTION", 2},
    {"CALL", 3},
    {"REPLY", 4},
    {"SIGNAL", 5},
    {"PROPERTY_SYNC", 6},
    {"INTEREST_AWARENESS", 8},
    {"CLOCK_HANDSHAKE", 9},
    {"CLOCK_HANDSHAKE_REPLY", 10},
    {"CLOCK_PING", 11},
    {"CLOCK_PONG", 12},
    {"LAGCOMP_DENY", 13},
    {"SPAWN", 14},
    {"DESPAWN", 15},
    {"REPARENT", 16},
    {"TABLE", 17},
    {"HIDE", 18},
    {"SYNC", 19},
    {"SYNC_DELTA", 20},
    {"CONTROL_REQUEST", 21},
    {"CONTROL_APPLY", 22},
    {"SESSION_JOIN", 23},
    {"SESSION_ACCEPT", 24},
    {"SESSION_ROSTER", 25},
    {"SESSION_PAUSE", 26},
    {"SESSION_UNPAUSE", 27},
    {"SESSION_KICKED", 28},
    {"SESSION_SHUTDOWN", 31},
    {"SESSION_SCENE_RELEASED", 32},
    {"SESSION_KICK_REQUEST", 33},
    {"PREDICT_COMMAND", 35},
    {"PREDICT_ACK", 36},
    {"PREDICT_RELAY", 37},
    {"PREDICT_RELAY_REQUEST", 38},
    {"SYNC_ROW", 39},
    {"SYNC_ROW_DELTA", 40},
    {"SYNC_ROW_WINDOW", 41},
    {"SESSION_SCENE_VIEWERS", 42},
    {"ROW_CONTROL", 43},
    {"ROUTE_LEASE", 44},
    {"LIFECYCLE_DECISION", 45},
    {"CLAIM_IMAGE", 46},
    {"SESSION_RESYNC", 47},
    {"LAGCOMP_CONFIRM", 48},
    {"SESSION_LEAVE", 49},
    {"SESSION_PLAYER_LEFT", 50},
    {"SESSION_SPAWN_FAILED", 51},
};

LawVerdict channel_ids(const Carrier &) {
    const netw::wire::WireRegistry registry
        = netw::wire::WireRegistry::create_default();
    const int count = int(sizeof(TRANSCRIBED) / sizeof(TRANSCRIBED[0]));
    if (int(registry.active_count()) != count) {
        return law_broken(
            "the registry declares %d channels",
            int(registry.active_count())
        );
    }
    for (const Declared &declared : TRANSCRIBED) {
        const netw::wire::ChannelDecl *decl
            = registry.find_channel(uint8_t(declared.id));
        if (netw::wire::builtin_channel(StringName(declared.name))
                != declared.id
            || decl == nullptr || decl->name != StringName(declared.name)) {
            return law_broken("%s does not carry its id", declared.name);
        }
    }
    for (const uint8_t id : {uint8_t(0), uint8_t(1), uint8_t(7)}) {
        const netw::wire::ChannelDecl *decl = registry.find_channel(id);
        if (decl == nullptr || !decl->is_reserved) {
            return law_broken("id %d is not reserved", int(id));
        }
    }
    return law_held();
}

LawVerdict ack_past_wrap(const Carrier &) {
    netw::wire::AckBook book;
    LocalVector<netw::wire::AckEntry> delivered;
    LocalVector<netw::wire::AckEntry> lost;
    book.record_send(65500, 1, 500);
    book.process_ack(20, 0, delivered, lost);
    if (lost.size() != 1 || lost[0].bits != 500) {
        return law_broken("%d sends read lost", int(lost.size()));
    }
    return law_held();
}

LawVerdict revisions_past_wrap(const Carrier &) {
    netw_test::StoreReader reader;
    reader.declare(counter_schema(), 7, BASE_TICK);
    const WirePlan &plan = reader.held().plan;
    const int sender = 5;
    for (uint64_t revision = 1; revision <= 70000; ++revision) {
        reader.land(sender, snapshot(plan, revision & 0x3ff, revision, 101));
    }
    if (reader.accepted(sender, TOKEN) != 70000) {
        return law_broken(
            "the stream stopped at revision %llu",
            (unsigned long long)reader.accepted(sender, TOKEN)
        );
    }
    netw_test::StoreReader wide;
    wide.declare(counter_schema(), 7, BASE_TICK);
    wide.land(sender, snapshot(plan, 11, 1, 101));
    wide.land(sender, snapshot(plan, 22, 65537, 101));
    if (!wide.keeps(sender, 1) || !wide.keeps(sender, 65537)
        || wide.image_at(sender, 1).equals(wide.image_at(sender, 65537))) {
        return law_broken("revisions 1 and 65537 were not held apart");
    }
    return law_held();
}

const Row ROWS[] = {
    {"a route varint that never ends",
     "the walk stops and applies no frame",
     &endless_route},
    {"a body length varint that never ends",
     "the walk stops and keeps only the whole frames before it",
     &endless_length},
    {"a path longer than the body that holds it",
     "the walk stops and applies no frame",
     &path_past_body},
    {"a frame header cut after one byte",
     "the walk stops and applies no frame",
     &header_cut},
    {"a body length past the end of the datagram",
     "the walk stops and applies no frame",
     &body_past_datagram},
    {"a partial frame after two whole ones",
     "the walk stops and keeps the two whole frames",
     &trailing_partial},
    {"a route spelled with a redundant final group",
     "the walk refuses it, so one route has one spelling",
     &redundant_route},
    {"a frame for a node outside the component table",
     "its path comes back beside the payload, empty or not",
     &pathed_frame},
    {"bytes a game sent with send_bytes",
     "peer_packet fires once with the sender and the same bytes",
     &game_bytes},
    {"an acknowledging datagram of ours",
     "the peer's ack advances and peer_packet stays silent",
     &acknowledging},
    {"our datagram with its header cut short, or an empty one",
     "peer_packet stays silent and each one warns",
     &cut_header},
    {"a datagram with a magic byte an older version used",
     "peer_packet stays silent and the receiver warns",
     &retired_magic},
    {"two peers on links of different latency",
     "each peer's round trip is measured on its own link",
     &round_trip_per_peer},
    {"a SPAWN carrying one field the server never wrote",
     "the client builds no node and warns",
     &spawn_extra_field},
    {"a SPAWN cut short of its tail",
     "the client builds no node and warns",
     &spawn_cut_short},
    {"a call whose argument row is cut short",
     "the call reads no arguments at all",
     &call_cut_short},
    {"a stale, repeated, unknown baseline or torn snapshot",
     "the received value stays as the last accepted frame left it",
     &refused_snapshot},
    {"a peer spending its whole request budget at once",
     "the budget admits its width and refuses the next",
     &whole_budget},
    {"a flooding peer that waits out the span",
     "it is refused until the span ends and admitted after",
     &span_expires},
    {"a peer sending steadily under the budget",
     "it is never refused, because the span slides",
     &span_slides},
    {"two builds registering components in different orders",
     "both assign the same component ids",
     &registration_order},
    {"a peer whose component digest disagrees with the server's",
     "it stops trusting compact ids and keeps the root and paths",
     &digest_disagrees},
    {"two builds of this version",
     "every channel carries its transcribed id and ids 0, 1 and 7 are "
     "reserved",
     &channel_ids},
    {"an ack after the datagram sequence wraps",
     "the send from before the wrap reads lost",
     &ack_past_wrap},
    {"a stream past 65,536 revisions",
     "it keeps accepting and holds revisions 1 and 65537 apart",
     &revisions_past_wrap},
};

TEST_CASE("[Networked][Wire] what a peer sends is read or refused whole") {
    const Carrier carrier = {String("a peer sends any bytes")};
    for (const Row &row : ROWS) {
        NETW_CELL(row, carrier);
        NETW_LAW_HOLDS(row, carrier);
    }
}

} // namespace TestNetwCarrier

#endif
