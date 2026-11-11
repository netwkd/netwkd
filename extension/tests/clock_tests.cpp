#include "support/fault_log.h"
#include "support/frame_drive.h"
#include "support/loopback_rig.h"
#include "support/netw_cells.h"
#include "support/netw_recorder.h"
#include "support/netw_test.h"

#if defined(NETW_TIER_HOSTED)

#include <cmath>
#include <memory>

#include "godot/engine.hpp"
#include "godot/scene_tree.hpp"
#include "netw/api/clock.hpp"
#include "netw/api/context.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/session.hpp"

namespace TestNetwClock {

using namespace godot;
using netw::Netw;
using netw::NetwClock;
using netw::NetwMultiplayer;
using netw_test::FaultWatch;
using netw_test::law_broken;
using netw_test::law_held;
using netw_test::LawVerdict;
using netw_test::LoopbackRig;
using netw_test::WorldDecl;

constexpr int EXACT_RATE = 8;
constexpr double TICK = 0.125;
constexpr double HALF_TICK = 0.0625;

struct Stand {
    String label;
};

typedef netw_test::LawRowFor<Stand> Row;

void clock_world(LoopbackRig &p_rig, int p_rate) {
    WorldDecl world;
    world.clocked(p_rate);
    p_rig.declare_world(world);
}

NetwMultiplayer *fresh_client(LoopbackRig &p_rig, int p_client = 0) {
    NetwMultiplayer *client = p_rig.client(p_client);
    client->clock_set_synchronized(false);
    client->clock_set_param(NetwMultiplayer::CLOCK_PARAM_LEAD_TICKS, 0.0);
    return client;
}

LawVerdict short_frames_carry(const Stand &) {
    LoopbackRig rig(1);
    clock_world(rig, EXACT_RATE);
    NetwMultiplayer *client = rig.client(0);
    const int64_t start = client->clock_get_tick();

    client->clock_physics_step(HALF_TICK);
    const int64_t after_one = client->clock_get_tick() - start;
    client->clock_physics_step(HALF_TICK);
    const int64_t after_two = client->clock_get_tick() - start;
    client->clock_physics_step(TICK);
    const int64_t after_whole = client->clock_get_tick() - start;

    if (after_one != 0 || after_two != 1 || after_whole != 2) {
        return law_broken(
            "two half frames and a whole one ran %d, %d and %d ticks",
            int(after_one),
            int(after_two),
            int(after_whole)
        );
    }
    return law_held();
}

LawVerdict long_frame_is_capped(const Stand &) {
    LoopbackRig rig(1);
    clock_world(rig, EXACT_RATE);
    NetwMultiplayer *client = rig.client(0);
    client->clock_set_param(
        NetwMultiplayer::CLOCK_PARAM_MAX_TICKS_PER_FRAME,
        2
    );
    const int64_t start = client->clock_get_tick();

    client->clock_physics_step(TICK * 5.0);
    const int64_t capped = client->clock_get_tick() - start;
    client->clock_physics_step(0.0);
    const int64_t owed = client->clock_get_tick() - start;

    if (capped != 2 || owed != 4) {
        return law_broken(
            "a five tick frame ran %d ticks and the next empty one %d",
            int(capped),
            int(owed - capped)
        );
    }
    return law_held();
}

LawVerdict stall_discards_the_bank(const Stand &) {
    const FaultWatch faults;
    LoopbackRig rig(2);
    clock_world(rig, EXACT_RATE);
    NetwMultiplayer *kept = rig.client(0);
    NetwMultiplayer *discarded = rig.client(1);
    for (NetwMultiplayer *client : {kept, discarded}) {
        client->clock_set_param(
            NetwMultiplayer::CLOCK_PARAM_MAX_TICKS_PER_FRAME,
            100
        );
    }
    kept->clock_set_param(
        NetwMultiplayer::CLOCK_PARAM_STALL_THRESHOLD,
        TICK * 6.0
    );
    discarded->clock_set_param(
        NetwMultiplayer::CLOCK_PARAM_STALL_THRESHOLD,
        TICK * 4.0
    );
    const int64_t kept_start = kept->clock_get_tick();
    const int64_t discarded_start = discarded->clock_get_tick();

    for (NetwMultiplayer *client : {kept, discarded}) {
        client->clock_physics_step(HALF_TICK);
        client->clock_physics_step(TICK * 5.0);
        client->clock_physics_step(HALF_TICK);
    }

    const int64_t kept_ran = kept->clock_get_tick() - kept_start;
    const int64_t discarded_ran
        = discarded->clock_get_tick() - discarded_start;
    if (kept_ran != 6 || discarded_ran != 5) {
        return law_broken(
            "the kept clock ran %d ticks and the stalled one %d",
            int(kept_ran),
            int(discarded_ran)
        );
    }
    const int64_t warned = faults.warnings_containing("NW1018");
    if (warned != 1) {
        return law_broken("the stall warned %d times", int(warned));
    }
    return law_held();
}

LawVerdict slow_peer_reports_behind(const Stand &) {
    LoopbackRig rig(1);
    clock_world(rig, 60);
    NetwMultiplayer *client = rig.client(0);
    client->clock_set_gate(true);

    client->clock_physics_step(3.0 / 60.0);

    if (client->clock_get_simulation_behind_count() <= 0) {
        return law_broken("a gated clock three ticks late reported nothing");
    }
    if (!client->clock_is_simulating()) {
        return law_broken("a gated clock that ran a tick held its world");
    }
    return law_held();
}

LawVerdict snap_takes_the_whole_correction(const Stand &) {
    LoopbackRig rig(2);
    clock_world(rig, 30);
    NetwMultiplayer *snap = fresh_client(rig, 0);
    NetwMultiplayer *stretch = fresh_client(rig, 1);
    snap->clock_set_param(
        NetwMultiplayer::CLOCK_PARAM_SYNC_MODE,
        int64_t(NetwMultiplayer::SYNC_MODE_SNAP)
    );
    stretch->clock_set_param(
        NetwMultiplayer::CLOCK_PARAM_SYNC_MODE,
        int64_t(NetwMultiplayer::SYNC_MODE_STRETCH)
    );
    stretch->clock_set_param(
        NetwMultiplayer::CLOCK_PARAM_STRETCH_NUDGE_FACTOR,
        0.5
    );
    stretch->clock_set_param(
        NetwMultiplayer::CLOCK_PARAM_PANIC_SNAP_THRESHOLD,
        100
    );
    for (NetwMultiplayer *client : {snap, stretch}) {
        client->clock_ingest_pong(0.0, 100, 0.0, true);
        client->clock_ingest_pong(0.0, 150, 0.0, true);
    }
    if (snap->clock_get_tick() != 150) {
        return law_broken("snap reached tick %d", int(snap->clock_get_tick()));
    }
    if (stretch->clock_get_tick() != 100) {
        return law_broken(
            "stretch jumped to tick %d before a frame ran",
            int(stretch->clock_get_tick())
        );
    }

    stretch->clock_physics_step(0.0);
    const int64_t crawled = stretch->clock_get_tick();
    if (crawled <= 100 || crawled >= 150) {
        return law_broken("one stretch frame reached tick %d", int(crawled));
    }

    stretch->clock_set_param(
        NetwMultiplayer::CLOCK_PARAM_PANIC_SNAP_THRESHOLD,
        2
    );
    stretch->clock_ingest_pong(0.0, 400, 0.0, true);
    stretch->clock_physics_step(0.0);
    if (stretch->clock_get_tick() != 400) {
        return law_broken(
            "a divergence past the panic threshold reached tick %d",
            int(stretch->clock_get_tick())
        );
    }
    return law_held();
}

LawVerdict gated_hitch_leaves_no_debt(const Stand &) {
    LoopbackRig rig(2);
    clock_world(rig, 60);
    const double step = 1.0 / 60.0;
    NetwMultiplayer *hitched = fresh_client(rig, 0);
    NetwMultiplayer *jittered = fresh_client(rig, 1);
    for (NetwMultiplayer *client : {hitched, jittered}) {
        client->clock_set_param(
            NetwMultiplayer::CLOCK_PARAM_SYNC_MODE,
            int64_t(NetwMultiplayer::SYNC_MODE_STRETCH)
        );
        client->clock_set_gate(true);
    }

    int server_tick = 100;
    hitched->clock_ingest_pong(0.0, server_tick, 0.0, true);
    for (int frame = 0; frame < 120; ++frame) {
        hitched->clock_physics_step(step);
        server_tick += frame == 20 ? 11 : 1;
        hitched->clock_ingest_pong(0.0, server_tick, 0.0, true);
    }
    const int64_t gap = std::abs(server_tick - hitched->clock_get_tick());
    const double owed = hitched->clock_get_monitor(
        NetwMultiplayer::CLOCK_MONITOR_TICK_ACCUMULATOR
    );
    if (gap > 1 || owed >= step) {
        return law_broken(
            "after a hitch the clock sat %d ticks off and owed %f",
            int(gap),
            owed
        );
    }

    server_tick = 100;
    jittered->clock_ingest_pong(0.0, server_tick, 0.25, true);
    int skipped = 0;
    for (int frame = 0; frame < 600; ++frame) {
        const int64_t before = jittered->clock_get_tick();
        jittered->clock_physics_step(step);
        skipped += jittered->clock_get_tick() - before > 1 ? 1 : 0;
        server_tick += 1;
        const int flip = (frame / 60) % 2;
        jittered->clock_ingest_pong(0.0, server_tick + flip, 0.25, true);
    }
    if (skipped != 0) {
        return law_broken("one tick of jitter skipped %d ticks", skipped);
    }
    return law_held();
}

LawVerdict only_a_following_client_is_synchronized(const Stand &) {
    LoopbackRig rig(1);
    rig.mount();
    clock_world(rig, 30);
    const Ref<netw::NetwSession> server_session = Netw::session(rig.branch());
    const Ref<netw::NetwSession> client_session = Netw::session(rig.branch(0));
    if (server_session.is_null() || client_session.is_null()) {
        return law_broken("a mounted session answered no handle");
    }
    const Ref<NetwClock> server_clock = server_session->get_clock();
    const Ref<NetwClock> client_clock = client_session->get_clock();

    rig.server()->clock_set_synchronized(false);
    NetwMultiplayer *client = fresh_client(rig);
    if (!server_clock->get_is_synchronized()) {
        return law_broken("the server's clock read unsynchronized");
    }
    if (client_clock->get_is_synchronized()) {
        return law_broken("a client that heard no pong read synchronized");
    }

    client->clock_ingest_pong(0.0, 40, 0.0, true);
    if (!client_clock->get_is_synchronized()) {
        return law_broken("a client that heard a pong read unsynchronized");
    }
    return law_held();
}

LawVerdict clock_announces_its_edges(const Stand &) {
    LoopbackRig rig(1);
    rig.mount();
    clock_world(rig, 30);
    const Ref<netw::NetwSession> session = Netw::session(rig.branch(0));
    if (session.is_null()) {
        return law_broken("a mounted session answered no handle");
    }
    NetwMultiplayer *client = fresh_client(rig);
    const StringName before_tick("before_tick");
    const StringName ticked("ticked");
    const StringName synchronized("synchronized");
    const netw_test::Recorder heard(
        session->get_clock().ptr(),
        {before_tick, ticked, synchronized}
    );
    const int64_t tick = client->clock_get_tick();

    client->clock_step(1);
    if (heard.count(before_tick) != 1 || heard.count(ticked) != 1) {
        return law_broken(
            "one tick emitted before_tick %d and ticked %d times",
            heard.count(before_tick),
            heard.count(ticked)
        );
    }
    if (heard.order() != Vector<StringName>({before_tick, ticked})) {
        return law_broken("ticked came before before_tick");
    }
    if (int64_t(heard.args(ticked)[1]) != tick) {
        return law_broken(
            "ticked carried tick %d for tick %d",
            int(int64_t(heard.args(ticked)[1])),
            int(tick)
        );
    }

    client->clock_ingest_pong(0.0, 40, 0.0, true);
    client->clock_ingest_pong(0.0, 41, 0.0, true);
    if (heard.count(synchronized) != 1) {
        return law_broken(
            "two pongs emitted synchronized %d times",
            heard.count(synchronized)
        );
    }
    return law_held();
}

const Row ROWS[] = {
    {"two half tick frames, then a whole one",
     "the first runs no tick, the second runs one, and the whole one runs "
     "one more",
     &short_frames_carry},
    {"a five tick frame under a ceiling of two",
     "it runs two ticks and the next empty frame runs the two still owed",
     &long_frame_is_capped},
    {"a five tick frame on a clock whose stall threshold is four ticks",
     "it warns once and drops the half tick banked before it, where a "
     "clock with a higher threshold keeps it",
     &stall_discards_the_bank},
    {"a gated clock handed a frame three ticks long",
     "it reports falling behind and still simulates the tick it ran",
     &slow_peer_reports_behind},
    {"a client fifty ticks behind the server's pong",
     "snap jumps the whole way, stretch moves part of it a frame, and "
     "stretch past the panic threshold snaps",
     &snap_takes_the_whole_correction},
    {"a gated stretch client after a server hitch, and under one tick of "
     "jitter",
     "the hitch leaves it within a tick with nothing owed, and the jitter "
     "never skips a tick",
     &gated_hitch_leaves_no_debt},
    {"a server and a client that has not heard a pong",
     "the server's clock reads synchronized and the client's reads so only "
     "after a pong",
     &only_a_following_client_is_synchronized},
    {"a client's clock that runs one tick, then hears two pongs",
     "its NetwClock emits before_tick then ticked with that tick, and "
     "synchronized once",
     &clock_announces_its_edges},
};

TEST_CASE(
    "[Networked][Clock] a clock keeps the time a short frame left, caps a "
    "long one, and follows the server"
) {
    const Stand stand = {String("a loopback session with a clock")};
    for (const Row &row : ROWS) {
        NETW_CELL(row, stand);
        NETW_LAW_HOLDS(row, stand);
    }
}

constexpr int TICKRATE = 30;
constexpr int SYNC_BUDGET = 120;
constexpr int RUN_FRAMES = 30;
constexpr int FOLLOW_BUDGET = 90;
constexpr int SERVER_JUMP = 60;
constexpr int SCALED_FRAMES = 60;
constexpr int MEASURE_FRAMES = 330;
constexpr int64_t PLANTED_OFFSET = 50;

struct Evidence {
    String label;
    bool driven = false;
    bool synchronized = false;
    int64_t ticks_at_sync = 0;
    int64_t ticks_after_run = 0;
    int64_t ticks_from_extra_polls = 0;
    bool followed = false;
    int64_t gap_after_jump = 0;
    int64_t full_speed_ticks = 0;
    int64_t half_speed_ticks = 0;
    int64_t measured_offset = -1;
};

Evidence &evidence() {
    static Evidence seen;
    return seen;
}

class PumpScenario final : public netw_test::FrameScenario {
    int phase = 0;
    int frames = 0;
    int64_t mark = 0;
    std::unique_ptr<LoopbackRig> rig;

    void enter(int p_phase) {
        phase = p_phase;
        frames = 0;
    }

    void open() {
        rig = std::make_unique<LoopbackRig>(1);
        rig->mount();
        clock_world(*rig, TICKRATE);
        for (NetwMultiplayer *api : {rig->server(), rig->client(0)}) {
            api->clock_set_param(
                NetwMultiplayer::CLOCK_PARAM_MANUAL_TICK,
                false
            );
            api->clock_set_synchronized(false);
        }
    }

    void close() {
        Engine::get_singleton()->set_time_scale(1.0);
        rig.reset();
        evidence().driven = true;
    }

public:
    bool advance() override {
        if (netw::gd::scene_root() == nullptr || phase < 0) {
            return false;
        }
        if (phase == 0) {
            open();
            enter(1);
            return true;
        }
        Evidence &seen = evidence();
        NetwMultiplayer *client = rig->client(0);
        NetwMultiplayer *server = rig->server();
        rig->pump(1);
        frames += 1;

        if (phase == 1) {
            if (client->clock_is_synchronized()) {
                seen.synchronized = true;
                seen.ticks_at_sync = client->clock_get_tick();
                enter(2);
            } else if (frames > SYNC_BUDGET) {
                enter(9);
            }
        } else if (phase == 2) {
            const int64_t before = client->clock_get_tick();
            rig->pump(2);
            seen.ticks_from_extra_polls += client->clock_get_tick() - before;
            if (frames >= RUN_FRAMES) {
                seen.ticks_after_run = client->clock_get_tick();
                server->clock_step(SERVER_JUMP);
                enter(3);
            }
        } else if (phase == 3) {
            seen.gap_after_jump
                = std::abs(server->clock_get_tick() - client->clock_get_tick());
            if (seen.gap_after_jump <= 2) {
                seen.followed = true;
                enter(4);
            } else if (frames > FOLLOW_BUDGET) {
                enter(4);
            }
        } else if (phase == 4) {
            mark = client->clock_get_tick();
            Engine::get_singleton()->set_time_scale(0.5);
            enter(5);
        } else if (phase == 5 && frames >= SCALED_FRAMES) {
            seen.half_speed_ticks = client->clock_get_tick() - mark;
            mark = client->clock_get_tick();
            Engine::get_singleton()->set_time_scale(1.0);
            enter(6);
        } else if (phase == 6 && frames >= SCALED_FRAMES) {
            seen.full_speed_ticks = client->clock_get_tick() - mark;
            client->clock_auto_configure_offset();
            client->clock_set_param(
                NetwMultiplayer::CLOCK_PARAM_DISPLAY_OFFSET,
                PLANTED_OFFSET
            );
            enter(7);
        } else if (phase == 7 && frames >= MEASURE_FRAMES) {
            seen.measured_offset = int64_t(client->clock_get_param(
                NetwMultiplayer::CLOCK_PARAM_DISPLAY_OFFSET
            ));
            enter(9);
        }

        if (phase == 9) {
            close();
            phase = -1;
            return false;
        }
        return true;
    }
};

NETW_FRAME_SCENARIO(PumpScenario, clock_pump_scenario);

typedef netw_test::LawRowFor<Evidence> FrameRow;

LawVerdict physics_frame_ticks(const Evidence &p_seen) {
    if (!p_seen.synchronized) {
        return law_broken("the client never synchronized");
    }
    if (p_seen.ticks_after_run <= p_seen.ticks_at_sync) {
        return law_broken(
            "the client stayed at tick %d over %d frames",
            int(p_seen.ticks_after_run),
            RUN_FRAMES
        );
    }
    return law_held();
}

LawVerdict extra_poll_runs_no_tick(const Evidence &p_seen) {
    if (p_seen.ticks_from_extra_polls != 0) {
        return law_broken(
            "extra polls ran %d ticks",
            int(p_seen.ticks_from_extra_polls)
        );
    }
    return law_held();
}

LawVerdict client_keeps_following(const Evidence &p_seen) {
    if (!p_seen.followed) {
        return law_broken(
            "the client sat %d ticks off the server",
            int(p_seen.gap_after_jump)
        );
    }
    return law_held();
}

LawVerdict time_scale_slows_ticks(const Evidence &p_seen) {
    if (p_seen.full_speed_ticks <= 0) {
        return law_broken("the client ran no ticks at full speed");
    }
    if (p_seen.half_speed_ticks * 2 > p_seen.full_speed_ticks * 3 / 2) {
        return law_broken(
            "half speed ran %d ticks against %d at full speed",
            int(p_seen.half_speed_ticks),
            int(p_seen.full_speed_ticks)
        );
    }
    return law_held();
}

LawVerdict measured_offset_applies(const Evidence &p_seen) {
    if (p_seen.measured_offset < 0
        || p_seen.measured_offset >= PLANTED_OFFSET) {
        return law_broken(
            "the display offset read %d after the measurement",
            int(p_seen.measured_offset)
        );
    }
    return law_held();
}

const FrameRow FRAME_ROWS[] = {
    {"a client with a declared clock and nothing calling a step",
     "it synchronizes and its tick advances on the physics frame",
     &physics_frame_ticks},
    {"a client polled twice more inside each frame",
     "the extra polls run no tick",
     &extra_poll_runs_no_tick},
    {"a server whose clock jumps sixty ticks after the client synchronized",
     "the client's later pings bring it back within two ticks",
     &client_keeps_following},
    {"the engine's time scale at one half for sixty frames",
     "the client runs about half the ticks it runs at full speed",
     &time_scale_slows_ticks},
    {"auto_configure_offset on a client whose display offset is fifty",
     "after five seconds of frames the offset is the one it measured",
     &measured_offset_applies},
};

TEST_CASE(
    "[Networked][Clock][Frame] a clock pumped by the physics frame ticks on "
    "its own, follows the server, and reads the frame's real delta"
) {
    Evidence &seen = evidence();
    REQUIRE(seen.driven);
    seen.label = String("a loopback session pumped by the physics frame");
    for (const FrameRow &row : FRAME_ROWS) {
        NETW_CELL(row, seen);
        NETW_LAW_HOLDS(row, seen);
    }
}

} // namespace TestNetwClock

#endif
