#pragma once

#include "netw_test.h"

#include "godot/json.hpp"
#include "godot/local_vector.hpp"
#include "godot/utility.hpp"
#include "godot/variant.hpp"
#include "netw/api/loopback.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/carrier_frame.hpp"
#include "netw/wire/capture.hpp"
#include "netw/wire/frame.hpp"
#include "netw/wire/registry.hpp"

namespace netw_test {

inline void adopt_into_memory(
    netw::NetwMultiplayer *p_api,
    const godot::Ref<netw::LocalMultiplayerPeer> &p_peer
) {
    p_api->capture_to_memory();
    p_api->set("multiplayer_peer", p_peer);
}

namespace capture {

using namespace godot;
using netw::NetwCarrierFrame;
using netw::wire::CaptureRecord;
using netw::wire::ChannelDecl;
using netw::wire::Frame;
using netw::wire::FrameWalk;
using netw::wire::WireRegistry;

struct Memory {
    Dictionary header;
    LocalVector<CaptureRecord> records;
};

inline Memory read(const PackedByteArray &p_capture) {
    Memory out;
    const int64_t newline = p_capture.find(uint8_t('\n'));
    REQUIRE(newline > 0);
    out.header = JSON::parse_string(
        p_capture.slice(0, newline).get_string_from_utf8()
    );
    netw::wire::ReadStream stream(p_capture.slice(newline + 1));
    CaptureRecord record;
    while (stream.bits_remaining() >= 8
           && netw::wire::capture_record_next(stream, record)) {
        out.records.push_back(record);
    }
    return out;
}

struct PayloadMask {
    const char *channel;
    int from;
    int bytes;
};

const char *const HEADER_MASKS[] = {"started"};

const PayloadMask PAYLOAD_MASKS[] = {
    {"CLOCK_PING", 0, 4},
    {"CLOCK_PONG", 0, 4},
    {"CLOCK_PONG", 9, 2},
};

inline String channel_name(const WireRegistry &p_registry, uint8_t p_channel) {
    const ChannelDecl *decl = p_registry.find_channel(p_channel);
    return decl == nullptr ? vformat("channel %d", int64_t(p_channel))
                           : String(decl->name);
}

inline PackedByteArray masked_payload(
    const WireRegistry &p_registry,
    const Frame &p_frame
) {
    PackedByteArray payload = p_frame.payload;
    const StringName channel(channel_name(p_registry, p_frame.channel));
    for (const PayloadMask &mask : PAYLOAD_MASKS) {
        if (channel != StringName(mask.channel)) {
            continue;
        }
        const int64_t end = mask.from + mask.bytes;
        for (int64_t at = mask.from; at < MIN(end, payload.size()); ++at) {
            payload.set(at, 0);
        }
    }
    return payload;
}

inline String first_byte_difference(
    const PackedByteArray &p_golden,
    const PackedByteArray &p_run
) {
    const int64_t shorter = MIN(p_golden.size(), p_run.size());
    for (int64_t at = 0; at < shorter; ++at) {
        if (p_golden[at] != p_run[at]) {
            return vformat(
                "byte %d. It should hold %02x and holds %02x.",
                at,
                int64_t(p_golden[at]),
                int64_t(p_run[at])
            );
        }
    }
    return vformat(
        "its length. It should hold %d bytes and holds %d.",
        p_golden.size(),
        p_run.size()
    );
}

inline String frame_difference(
    const WireRegistry &p_registry,
    const Frame &p_golden,
    const Frame &p_run
) {
    if (p_golden.channel != p_run.channel) {
        return vformat(
            "differs in its channel. It should hold %s and holds %s.",
            channel_name(p_registry, p_golden.channel),
            channel_name(p_registry, p_run.channel)
        );
    }
    const String on = "on " + channel_name(p_registry, p_golden.channel);
    const char *const names[] = {"route", "comp"};
    const int64_t golden[] = {p_golden.route, int64_t(p_golden.comp)};
    const int64_t run[] = {p_run.route, int64_t(p_run.comp)};
    for (int at = 0; at < 2; ++at) {
        if (golden[at] != run[at]) {
            return vformat(
                "%s differs in its %s. It should hold %d and holds %d.",
                on,
                names[at],
                golden[at],
                run[at]
            );
        }
    }
    if (p_golden.path != p_run.path) {
        return vformat(
            "%s differs in its path. It should hold %s and holds %s.",
            on,
            p_golden.path,
            p_run.path
        );
    }
    const PackedByteArray golden_payload = masked_payload(p_registry, p_golden);
    const PackedByteArray run_payload = masked_payload(p_registry, p_run);
    if (golden_payload == run_payload) {
        return String();
    }
    return vformat(
        "%s differs in its payload at %s",
        on,
        first_byte_difference(golden_payload, run_payload)
    );
}

inline String record_difference(
    const WireRegistry &p_registry,
    const CaptureRecord &p_golden,
    const CaptureRecord &p_run
) {
    const char *const names[] = {"dir", "peer", "tick"};
    const uint64_t golden[] = {p_golden.dir, p_golden.peer, p_golden.tick};
    const uint64_t run[] = {p_run.dir, p_run.peer, p_run.tick};
    for (int at = 0; at < 3; ++at) {
        if (golden[at] != run[at]) {
            return vformat(
                "differs in its %s. It should hold %d and holds %d.",
                names[at],
                int64_t(golden[at]),
                int64_t(run[at])
            );
        }
    }
    if (p_golden.bytes == p_run.bytes) {
        return String();
    }

    const NetwCarrierFrame golden_head = NetwCarrierFrame::read(p_golden.bytes);
    const NetwCarrierFrame run_head = NetwCarrierFrame::read(p_run.bytes);
    const char *const heads[] = {"kind", "seq", "ack", "history", "base tick"};
    const int64_t golden_fields[] = {
        golden_head.kind,
        golden_head.seq,
        golden_head.ack,
        int64_t(golden_head.history),
        golden_head.tick,
    };
    const int64_t run_fields[] = {
        run_head.kind,
        run_head.seq,
        run_head.ack,
        int64_t(run_head.history),
        run_head.tick,
    };
    for (int at = 0; at < 5; ++at) {
        if (golden_fields[at] != run_fields[at]) {
            return vformat(
                "differs in its datagram %s. It should hold %d and holds %d.",
                heads[at],
                golden_fields[at],
                run_fields[at]
            );
        }
    }
    if (golden_head.kind < NetwCarrierFrame::RELIABLE) {
        return String("holds no frames and differs at ")
            + first_byte_difference(p_golden.bytes, p_run.bytes);
    }

    const FrameWalk golden_walk = netw::wire::frame_unpack_all(
        p_golden.bytes,
        golden_head.payload_offset
    );
    const FrameWalk run_walk
        = netw::wire::frame_unpack_all(p_run.bytes, run_head.payload_offset);
    const uint32_t shorter
        = MIN(golden_walk.frames.size(), run_walk.frames.size());
    for (uint32_t at = 0; at < shorter; ++at) {
        const String difference = frame_difference(
            p_registry,
            golden_walk.frames[at],
            run_walk.frames[at]
        );
        if (!difference.is_empty()) {
            return vformat("frame %d %s", int64_t(at), difference);
        }
    }
    if (golden_walk.frames.size() != run_walk.frames.size()
        || golden_walk.whole != run_walk.whole) {
        return vformat(
            "should hold %d frames and holds %d.",
            int64_t(golden_walk.frames.size()),
            int64_t(run_walk.frames.size())
        );
    }
    return String();
}

inline String header_difference(Dictionary p_golden, Dictionary p_run) {
    for (const char *mask : HEADER_MASKS) {
        p_golden.erase(mask);
        p_run.erase(mask);
    }
    Array keys = p_golden.keys();
    keys.append_array(p_run.keys());
    for (int64_t at = 0; at < keys.size(); ++at) {
        const Variant key = keys[at];
        if (!p_golden.has(key) || !p_run.has(key)
            || p_golden[key] != p_run[key]) {
            return vformat(
                "The capture header differs in its %s key.",
                String(key)
            );
        }
    }
    return String();
}

inline String difference(
    const PackedByteArray &p_golden,
    const PackedByteArray &p_run
) {
    if (p_golden.is_empty() || p_run.is_empty()) {
        return p_golden.size() == p_run.size()
            ? String()
            : vformat(
                  "The capture should hold %d bytes and holds %d.",
                  p_golden.size(),
                  p_run.size()
              );
    }
    const Memory golden = read(p_golden);
    const Memory run = read(p_run);
    const String header = header_difference(golden.header, run.header);
    if (!header.is_empty()) {
        return header;
    }

    const WireRegistry registry = WireRegistry::create_default();
    const uint32_t shorter = MIN(golden.records.size(), run.records.size());
    for (uint32_t at = 0; at < shorter; ++at) {
        const String found
            = record_difference(registry, golden.records[at], run.records[at]);
        if (!found.is_empty()) {
            return vformat("Record %d %s", int64_t(at), found);
        }
    }
    if (golden.records.size() != run.records.size()) {
        return vformat(
            "The capture should hold %d records and holds %d.",
            int64_t(golden.records.size()),
            int64_t(run.records.size())
        );
    }
    return String();
}

} // namespace capture

} // namespace netw_test
