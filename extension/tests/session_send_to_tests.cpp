#include "support/fault_log.h"
#include "support/internals.h"
#include "support/netw_test.h"

#include "netw/api/netw_multiplayer.hpp"

using namespace godot;
using netw_test::Internals;

namespace TestNetwSessionSendTo {

using godot::PackedByteArray;
using godot::Ref;
using godot::String;
using netw::NetwMultiplayer;

constexpr int64_t SYNC_CHANNEL = 19;
constexpr int64_t LAGCOMP_DENY_CHANNEL = 13;
constexpr int64_t PEER = 5;

PackedByteArray payload(int p_size) {
    PackedByteArray out;
    out.resize(p_size);
    for (int index = 0; index < p_size; ++index) {
        out.set(index, uint8_t(index));
    }
    return out;
}

Ref<NetwMultiplayer> configured_core() {
    Ref<NetwMultiplayer> core;
    core.instantiate();
    Internals::clock_engine(core).set_configured(true);
    return core;
}

TEST_CASE(
    "[Netw][Transport][Hosted] S1 a frame on an aggregating channel waits "
    "in the peer's run rather than leaving on its own"
) {
    const Ref<NetwMultiplayer> core = configured_core();

    NETW_CHECK_EQ(
        core->send_to(PEER, 3, SYNC_CHANNEL, payload(4), false, 0, String()),
        godot::OK
    );

    NETW_CHECK_EQ(core->wire().carrier.pending(PEER, false), 8);
    NETW_CHECK_EQ(core->wire().carrier.pending(PEER, true), 0);
}

TEST_CASE(
    "[Netw][Transport][Hosted] S2 a frame sent before the clock is "
    "configured still joins the peer's run, because the poll pump drains it "
    "on every frame whether the clock ticks or not"
) {
    Ref<NetwMultiplayer> core;
    core.instantiate();

    core->send_to(PEER, 3, SYNC_CHANNEL, payload(4), false, 0, String());
    NETW_CHECK_EQ(core->wire().carrier.pending(PEER, false), 8);

    core->send_to(PEER, 4, SYNC_CHANNEL, payload(4), false, 0, String());
    const bool one_run_rather_than_two
        = core->wire().carrier.pending(PEER, false) == 16;
    CHECK(one_run_rather_than_two);
}

TEST_CASE(
    "[Netw][Transport][Hosted] S3 an immediate channel leaves now even "
    "with a running clock, and even when the sender asks to batch"
) {
    const Ref<NetwMultiplayer> core = configured_core();

    core->send_to(
        PEER,
        0,
        LAGCOMP_DENY_CHANNEL,
        payload(4),
        false,
        0,
        String(),
        netw::wire::Delivery::TICK
    );

    NETW_CHECK_EQ(core->wire().carrier.pending(PEER, false), 0);
}

TEST_CASE(
    "[Netw][Transport][Hosted] S5 the reliable and unreliable runs of one "
    "peer are separate, so a reliable frame never rides an unreliable stamp"
) {
    const Ref<NetwMultiplayer> core = configured_core();

    core->send_to(PEER, 3, SYNC_CHANNEL, payload(4), true, 0, String());

    NETW_CHECK_EQ(core->wire().carrier.pending(PEER, true), 8);
    NETW_CHECK_EQ(core->wire().carrier.pending(PEER, false), 0);
}

TEST_CASE(
    "[Netw][Transport][Hosted] S6 an immediate reliable frame drains "
    "the reliable run already queued for its peer, so source order is "
    "transport order across batching modes"
) {
    const Ref<NetwMultiplayer> core = configured_core();

    core->send_to(PEER, 3, SYNC_CHANNEL, payload(4), true, 0, String());
    NETW_CHECK_EQ(core->wire().carrier.pending(PEER, true), 8);

    core->send_to(PEER, 3, LAGCOMP_DENY_CHANNEL, payload(4), true, 0, String());

    NETW_CHECK_EQ(core->wire().carrier.pending(PEER, true), 0);
}

TEST_CASE(
    "[Netw][Transport][Hosted] S7 a cleared route leaves no row in the "
    "peer's run, so the run's ack names only live routes"
) {
    const Ref<NetwMultiplayer> core = configured_core();
    netw::CarrierRow row;
    row.route = 3;
    row.token = 1;
    row.revision = 1;
    core->wire().carrier.append(PEER, payload(4), false, 1024, &row);

    core->replication_clear_route(3);

    const netw::CarrierBatch run = core->wire().carrier.take(PEER, false);
    NETW_CHECK_EQ(int64_t(run.rows().size()), 0);
}

} // namespace TestNetwSessionSendTo
