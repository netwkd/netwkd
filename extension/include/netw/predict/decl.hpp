#pragma once

#include <cstdint>
#include <functional>

#include "godot/callable.hpp"
#include "godot/variant.hpp"
#include "netw/api/predict_stats.hpp"
#include "netw/api/prediction.hpp"

namespace netw::predict {

struct Decl {
    int input_source = NetwPrediction::INPUT_SOURCE_NONE;
    int sim_mode = NetwPrediction::SIM_MODE_DISPLAY;
    int mode = NetwPrediction::MODE_AUTO;
    bool pause_on_unpredicted_contact = false;
    godot::Callable input;
    godot::Dictionary sensors;
    int64_t world_version = -1;
    godot::Callable correction_path;
    double teleport_threshold = NetwPrediction::DEFAULT_TELEPORT_THRESHOLD;
    int correction_cooldown_ticks = 6;
    bool sleeping = false;
    bool repeat_missing_input = false;
    int max_inputs_per_tick = 2;
    int input_buffer_ticks = 0;
    int max_input_lag_ticks = 60;
    int ack_age_ticks = 0;
    double correction_threshold = 0.01;
    bool rollback_together = false;
    bool wrote_teleport_threshold = false;
    bool wrote_repeat_missing_input = false;
    bool wrote_rollback_together = false;
    NetwPredictStats::Book facts;
#if defined(NETW_TESTS)
    std::function<void(const godot::StringName &, const godot::Array &)>
        test_report;
#endif

    Decl() = default;
    Decl(const Decl &) = delete;
    Decl &operator=(const Decl &) = delete;
};

} // namespace netw::predict
