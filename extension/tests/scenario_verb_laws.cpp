#include "support/netw_test.h"

#if defined(NETW_TIER_HOSTED)

#include <string>

#include "netw/carrier_frame.hpp"
#include "netw/wire/capture.hpp"
#include "netw/wire/frame.hpp"
#include "netw/wire/registry.hpp"
#include "support/scenario_capture.h"

namespace TestScenarioVerbs {

using namespace godot;
using namespace netw_test;
using netw::NetwCarrierFrame;
using netw::wire::CaptureDirection;

constexpr int SERVER = -1;
constexpr int C1 = 0;
constexpr CaptureDirection OUT = CaptureDirection::OUT;
constexpr CaptureDirection IN = CaptureDirection::IN;

const char *const HEADER = "label verbs\nclients 2\nticks 30\nseed 7\nlaws\n";
const char *const PAUSED = "4 pause server paused:true reason:busy\n";
const char *const SPAWNED = "3 spawn car scene:car parent:World name:Car\n"
                            "3 spawn wheel scene:wheel parent:car name:Wheel\n";

struct VerbLaw {
    const char *before;
    const char *row;
    int world;
    CaptureDirection dir;
    const char *adds;
};

const VerbLaw LAWS[] = {
    {"", "2 join c3 name:cy", SERVER, OUT, "SESSION_ACCEPT"},
    {"", "6 leave c1", C1, OUT, "SESSION_LEAVE"},
    {"", "6 kick c2 reason:spam", SERVER, OUT, "SESSION_KICKED"},
    {"", "6 pause server paused:true reason:busy", SERVER, OUT,
     "SESSION_PAUSE"},
    {PAUSED, "8 pause server paused:false", SERVER, OUT, "SESSION_UNPAUSE"},
    {"", "6 send_bytes c1 to:server bytes:5800", SERVER, IN, "5800"},
    {"", "3 spawn car scene:car parent:World name:Car", SERVER, OUT, "SPAWN"},
    {SPAWNED, "20 despawn car", SERVER, OUT, "DESPAWN"},
    {SPAWNED, "20 reparent wheel parent:World", SERVER, OUT, "REPARENT"},
    {SPAWNED, "20 set car position:4,0,2", SERVER, OUT, "SYNC_ROW"},
};

PackedByteArray records_of(const String &p_text, int p_world) {
    const ScenarioText text = read_scenario(p_text);
    REQUIRE_MESSAGE(text.refused_line == 0, text.refusal.utf8().get_data());
    const LocalVector<PackedByteArray> worlds = capture_scenario(text.scenario);
    REQUIRE(uint32_t(p_world + 1) < worlds.size());
    const PackedByteArray capture = worlds[p_world + 1];
    const int64_t newline = capture.find(uint8_t('\n'));
    REQUIRE(newline > 0);
    return capture.slice(newline + 1);
}

String arrivals(const String &p_text, int p_world) {
    netw::wire::ReadStream read(records_of(p_text, p_world));
    netw::wire::CaptureRecord record;
    String out;
    while (read.bits_remaining() >= 8
           && netw::wire::capture_record_next(read, record)) {
        if (record.dir == uint64_t(IN)) {
            out += vformat(
                "\n%d %s",
                int64_t(record.tick),
                record.bytes.hex_encode()
            );
        }
    }
    return out;
}

int64_t carried(const String &p_text, const VerbLaw &p_law) {
    netw::wire::ReadStream read(records_of(p_text, p_law.world));
    const netw::wire::ChannelDecl *channel
        = netw::wire::WireRegistry::create_default().find_channel_by_name(
            StringName(p_law.adds)
        );
    netw::wire::CaptureRecord record;
    int64_t bytes = 0;
    while (read.bits_remaining() >= 8
           && netw::wire::capture_record_next(read, record)) {
        if (record.dir != uint64_t(p_law.dir)) {
            continue;
        }
        if (record.bytes.hex_encode() == String(p_law.adds)) {
            bytes += record.bytes.size();
            continue;
        }
        const NetwCarrierFrame head = NetwCarrierFrame::read(record.bytes);
        if (channel == nullptr || head.kind < NetwCarrierFrame::RELIABLE) {
            continue;
        }
        const netw::wire::FrameWalk walk
            = netw::wire::frame_unpack_all(record.bytes, head.payload_offset);
        for (const netw::wire::Frame &frame : walk.frames) {
            if (frame.channel == channel->id) {
                bytes += frame.payload.size() + 1;
            }
        }
    }
    return bytes;
}

TEST_CASE(
    "[Networked][Scenario] each verb row adds to one world's capture the "
    "bytes its name says, against the same scenario without that row"
) {
    for (const VerbLaw &law : LAWS) {
        INFO(std::string(law.row));
        const String before = String(HEADER) + String(law.before);
        const int64_t without = carried(before, law);
        const int64_t with = carried(before + String(law.row) + "\n", law);
        NETW_CHECK_GT(with, without);
    }
}

TEST_CASE(
    "[Networked][Scenario] a drop row removes exactly that datagram from the "
    "receiving capture, and a delay row makes it arrive that many ticks later"
) {
    const String before = "label loss\nclients 0\nticks 12\nseed 7\nlaws\n";
    const String joined = "2 join c1 name:cy\n";
    const String without = arrivals(before + joined, C1);
    const int64_t second = without.find("\n", 1);
    REQUIRE(second > 0);
    const String first = without.left(second);

    const String dropped
        = arrivals(before + String("1 drop s>c1 index:3\n") + joined, C1);
    const String delayed = arrivals(
        before + String("1 delay s>c1 index:3 ticks:2\n") + joined,
        C1
    );
    CHECK(dropped == without.substr(second));
    CHECK((delayed + String("\n")).contains(vformat(
        "\n%d %s\n",
        first.get_slice(" ", 0).to_int() + 2,
        first.get_slice(" ", 1)
    )));
}

} // namespace TestScenarioVerbs

#endif
