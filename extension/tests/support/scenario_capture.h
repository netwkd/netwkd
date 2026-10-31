#pragma once

#include "netw_test.h"

#include "loopback_rig.h"
#include "scenario_run.h"
#include "scenario_text.h"

#include "godot/json.hpp"
#include "godot/local_vector.hpp"
#include "godot/utility.hpp"
#include "godot/variant.hpp"
#include "netw/wire/capture.hpp"

namespace netw_test {

constexpr int SCENARIO_TICKRATE = 30;
constexpr int SCENARIO_HISTORY = 3;

inline void adopt_into_memory(
    netw::NetwMultiplayer *p_api,
    const godot::Ref<netw::LocalMultiplayerPeer> &p_peer
) {
    p_api->capture_to_memory();
    p_api->set("multiplayer_peer", p_peer);
}

struct MemoryCapture {
    godot::Dictionary header;
    godot::LocalVector<netw::wire::CaptureRecord> records;
    int64_t trailing_bits = 0;
};

inline MemoryCapture read_memory(const godot::PackedByteArray &p_capture) {
    MemoryCapture out;
    const int64_t newline = p_capture.find(uint8_t('\n'));
    REQUIRE(newline > 0);
    out.header = godot::JSON::parse_string(
        p_capture.slice(0, newline).get_string_from_utf8()
    );
    netw::wire::ReadStream read(p_capture.slice(newline + 1));
    netw::wire::CaptureRecord record;
    while (read.bits_remaining() >= 8
           && netw::wire::capture_record_next(read, record)) {
        out.records.push_back(record);
    }
    out.trailing_bits = read.bits_remaining();
    return out;
}

inline godot::LocalVector<godot::PackedByteArray> capture_scenario(
    const Scenario &p_scenario
) {
    Scenario scenario = p_scenario;
    scenario.world = WorldDecl().clocked(SCENARIO_TICKRATE, SCENARIO_HISTORY);
    if (scenario.declares(godot::StringName("hold_input"))) {
        scenario.epsilon = 0.01;
        scenario.world.player(
            EntityDecl()
                .named("Runner")
                .on_schema("PredictedPose")
                .synced("position")
                .placed_at(godot::Vector2())
                .predicted(0.01)
                .scheduled(netw::Schedule::TICK),
            0
        );
    }
    netw::gd::seed(scenario.seed);
    LoopbackRig rig(
        scenario.clients,
        scenario.declares(godot::StringName("transfer")),
        adopt_into_memory
    );
    ScenarioRun::session(rig, scenario);

    godot::LocalVector<godot::PackedByteArray> worlds;
    worlds.push_back(rig.server()->capture_writer().memory());
    for (int index = 0; index < rig.count(); ++index) {
        worlds.push_back(rig.client(index)->capture_writer().memory());
    }
    return worlds;
}

} // namespace netw_test
