#include "support/netw_test.h"

#if defined(NETW_TIER_HOSTED)

#include "godot/file_access.hpp"
#include "godot/file_system.hpp"
#include "netw/wire/capture.hpp"
#include "netw/wire/spec.hpp"
#include "support/scenario_capture.h"

namespace TestWireCaptureFile {

using namespace godot;
using netw_test::adopt_into_memory;
using netw_test::LoopbackRig;
using netw_test::MemoryCapture;
using netw_test::read_memory;
using netw::wire::CaptureDirection;
using netw::wire::CaptureRecord;
using netw::wire::CaptureWriter;
using netw::wire::ReadStream;

PackedByteArray blob(std::initializer_list<uint8_t> p_bytes) {
    PackedByteArray out;
    for (const uint8_t byte : p_bytes) {
        out.push_back(byte);
    }
    return out;
}

String law_path(const char *p_name) {
    const String dir("user://netw_capture_laws");
    DirAccess::make_dir_recursive_absolute(dir);
    return dir + String("/") + String(p_name);
}

TEST_CASE(
    "[Networked][Wire][Capture] C5 a datagram past the record's cap is counted "
    "rather than truncated into the capture, because a truncated datagram "
    "decodes to a lie while a missing one is a number"
) {
    CaptureWriter writer;
    Dictionary header;
    header[StringName("format")] = int64_t(10);
    REQUIRE(writer.open(law_path("netw_capture_cap.netwcap"), header));

    PackedByteArray oversized;
    oversized.resize(netw::wire::CAPTURE_BYTES_CAP + 1);
    writer.note(CaptureDirection::OUT, 1, 0, oversized);
    writer.note(CaptureDirection::OUT, 1, 0, blob({0x57, 0x01}));
    writer.close();

    NETW_CHECK_EQ(writer.oversized_count(), int64_t(1));
    NETW_CHECK_EQ(writer.record_count(), int64_t(1));
}

TEST_CASE(
    "[Networked][Wire][Capture] C6 a capture opens with one JSON header line "
    "and the records that follow decode back in the order they were written"
) {
    const String path = law_path("netw_capture_round.netwcap");
    CaptureWriter writer;
    Dictionary header = netw::wire::spec_document();
    header[StringName("peer")] = int64_t(3);
    REQUIRE(writer.open(path, header));
    writer.note(CaptureDirection::OUT, 1, 7, blob({0x57, 0x01}));
    writer.note(CaptureDirection::IN, 1, 8, blob({0x77, 0x09, 0x00, 0x00}));
    writer.close();
    NETW_CHECK_EQ(writer.record_count(), int64_t(2));

    const Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ);
    REQUIRE(file.is_valid());
    const String line = file->get_line();
    const bool names_format = line.contains("\"format\"");
    const bool names_records = line.contains("CaptureRecord");
    CHECK(names_format);
    CHECK(names_records);

    PackedByteArray body;
    while (!file->eof_reached()) {
        const PackedByteArray chunk = file->get_buffer(4096);
        if (chunk.is_empty()) {
            break;
        }
        body.append_array(chunk);
    }

    ReadStream read(body);
    CaptureRecord out;
    REQUIRE(netw::wire::capture_record_next(read, out));
    NETW_CHECK_EQ(int64_t(out.dir), int64_t(CaptureDirection::OUT));
    NETW_CHECK_EQ(int64_t(out.tick), int64_t(8));

    CaptureRecord in;
    REQUIRE(netw::wire::capture_record_next(read, in));
    NETW_CHECK_EQ(int64_t(in.dir), int64_t(CaptureDirection::IN));
    NETW_CHECK_EQ(int64_t(in.tick), int64_t(9));
    NETW_CHECK_EQ(read.bits_remaining(), int64_t(0));
}

LocalVector<PackedByteArray> datagrams(
    const MemoryCapture &p_capture,
    CaptureDirection p_dir,
    int64_t p_peer
) {
    LocalVector<PackedByteArray> out;
    for (const CaptureRecord &record : p_capture.records) {
        if (record.dir == uint64_t(p_dir) && record.peer == uint64_t(p_peer)) {
            out.push_back(record.bytes);
        }
    }
    return out;
}

bool same_datagrams(
    const LocalVector<PackedByteArray> &p_left,
    const LocalVector<PackedByteArray> &p_right
) {
    if (p_left.size() != p_right.size()) {
        return false;
    }
    for (uint32_t at = 0; at < p_left.size(); ++at) {
        if (p_left[at] != p_right[at]) {
            return false;
        }
    }
    return true;
}

TEST_CASE(
    "[Networked][Wire][Capture] C7 two worlds in one process each capture "
    "into memory exactly the datagrams they sent and received"
) {
    LoopbackRig rig(0, false, adopt_into_memory);
    rig.arrive(StringName("ada"));
    rig.arrive(StringName("bo"));
    rig.step_ticks(10);

    const MemoryCapture server
        = read_memory(rig.server()->capture_writer().memory());
    NETW_CHECK_EQ(int64_t(server.header.get("peer", -1)), int64_t(1));
    NETW_CHECK_EQ(server.trailing_bits < 8, true);
    NETW_CHECK_EQ(
        int64_t(server.records.size()),
        rig.server()->capture_writer().record_count()
    );

    for (int index = 0; index < rig.count(); ++index) {
        const int64_t id = rig.peer_id(index);
        const MemoryCapture client
            = read_memory(rig.client(index)->capture_writer().memory());
        NETW_CHECK_EQ(int64_t(client.header.get("peer", -1)), id);
        NETW_CHECK_EQ(client.trailing_bits < 8, true);

        const LocalVector<PackedByteArray> sent
            = datagrams(server, CaptureDirection::OUT, id);
        const LocalVector<PackedByteArray> received
            = datagrams(client, CaptureDirection::IN, 1);
        const LocalVector<PackedByteArray> answered
            = datagrams(client, CaptureDirection::OUT, 1);
        REQUIRE(sent.size() > 0);
        CHECK(same_datagrams(sent, received));
        CHECK(same_datagrams(
            answered,
            datagrams(server, CaptureDirection::IN, id)
        ));
        NETW_CHECK_EQ(
            int64_t(received.size() + answered.size()),
            int64_t(client.records.size())
        );
    }
}

} // namespace TestWireCaptureFile

#endif
