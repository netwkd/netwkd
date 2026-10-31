#include "support/netw_test.h"

#if defined(NETW_TIER_HOSTED)

#include <string>

#include "godot/file_access.hpp"
#include "godot/file_system.hpp"
#include "godot/json.hpp"
#include "godot/os.hpp"
#include "netw/carrier_frame.hpp"
#include "netw/wire/frame.hpp"
#include "netw/wire/registry.hpp"
#include "support/netw_cells.h"
#include "support/netw_reset.h"
#include "support/scenario_capture.h"

namespace TestGoldenCapture {

using namespace godot;
using namespace netw_test;
using netw::NetwCarrierFrame;
using netw::wire::CaptureRecord;
using netw::wire::ChannelDecl;
using netw::wire::Frame;
using netw::wire::FrameWalk;
using netw::wire::WireRegistry;

const char *const GOLDEN_DIR = "res://extension/tests/golden";
const char *const RECORD_FLAG = "--record";

struct GoldenLaw {
    const char *name;
    const char *claim;
};

const GoldenLaw GOLDEN = {
    "golden",
    "each world captures the datagrams its golden holds, record by record, "
    "and a second seeded run captures the same datagrams again",
};

struct PayloadMask {
    const char *channel;
    int from;
    int bytes;
};

const char *const HEADER_MASKS[] = {"started"};

const PayloadMask PAYLOAD_MASKS[] = {
    {"CLOCK_PING", 0, 4},
    {"CLOCK_PONG", 0, 4},
};

String channel_name(const WireRegistry &p_registry, uint8_t p_channel) {
    const ChannelDecl *decl = p_registry.find_channel(p_channel);
    return decl == nullptr ? vformat("channel %d", int64_t(p_channel))
                           : String(decl->name);
}

PackedByteArray masked_payload(
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

String first_byte_difference(
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

String frame_difference(
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

String record_difference(
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

String header_difference(Dictionary p_golden, Dictionary p_run) {
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

String capture_difference(
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
    const MemoryCapture golden = read_memory(p_golden);
    const MemoryCapture run = read_memory(p_run);
    const String header = header_difference(golden.header, run.header);
    if (!header.is_empty()) {
        return header;
    }

    const WireRegistry registry = WireRegistry::create_default();
    const uint32_t shorter = MIN(golden.records.size(), run.records.size());
    for (uint32_t at = 0; at < shorter; ++at) {
        const String difference
            = record_difference(registry, golden.records[at], run.records[at]);
        if (!difference.is_empty()) {
            return vformat("Record %d %s", int64_t(at), difference);
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

String golden_path(const String &p_label, int64_t p_world) {
    return vformat("%s/%s.w%d.netwcap", GOLDEN_DIR, p_label, p_world);
}

void record_golden(const String &p_path, const PackedByteArray &p_capture) {
    const Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
    REQUIRE_MESSAGE(file.is_valid(), p_path.utf8().get_data());
    file->store_buffer(p_capture);
}

void judge_world(
    const String &p_path,
    const PackedByteArray &p_first,
    const PackedByteArray &p_second
) {
    INFO(std::string(p_path.utf8().get_data()));
    if (!FileAccess::file_exists(p_path)) {
        FAIL_CHECK("This world has no golden. Record one with --record.");
    } else {
        const PackedByteArray golden = FileAccess::get_file_as_bytes(p_path);
        const std::string drift
            = capture_difference(golden, p_first).utf8().get_data();
        CHECK_MESSAGE(drift.empty(), drift);
    }
    const std::string flap
        = capture_difference(p_first, p_second).utf8().get_data();
    CHECK_MESSAGE(flap.empty(), ("The second run differs. " + flap));
}

TEST_CASE(
    "[Networked][Golden] each scenario captures in every world the datagrams "
    "its golden holds, and a second seeded run captures the same"
) {
    const bool recording
        = OS::get_singleton()->get_cmdline_user_args().has(RECORD_FLAG);
    const PackedStringArray files = DirAccess::get_files_at(GOLDEN_DIR);
    int scenarios = 0;
    for (const String &file : files) {
        if (file.get_extension() != "netwscn") {
            continue;
        }
        scenarios += 1;
        INFO(std::string(file.utf8().get_data()));
        const ScenarioText text = read_scenario(FileAccess::get_file_as_string(
            String(GOLDEN_DIR) + "/" + file
        ));
        if (text.refused_line != 0) {
            FAIL_CHECK(text.refusal.utf8().get_data());
            continue;
        }
        NETW_CELL(GOLDEN, text.scenario);

        reset_for_case();
        const LocalVector<PackedByteArray> first
            = capture_scenario(text.scenario);
        reset_for_case();
        const LocalVector<PackedByteArray> second
            = capture_scenario(text.scenario);
        REQUIRE(first.size() == second.size());

        const String label = text.scenario.label;
        for (uint32_t world = 0; world < first.size(); ++world) {
            const String path = golden_path(label, world);
            if (recording) {
                record_golden(path, first[world]);
            }
            judge_world(path, first[world], second[world]);
        }
        const bool extra = FileAccess::file_exists(
            golden_path(label, first.size())
        );
        CHECK_MESSAGE(
            !extra,
            "A golden exists for a world this run did not build."
        );
    }
    CHECK(scenarios > 0);
}

} // namespace TestGoldenCapture

#endif
