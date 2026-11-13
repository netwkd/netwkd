#pragma once

#include <cstdint>

#include "godot/callable.hpp"
#include "godot/variant.hpp"
#include "netw/api/predict_stats.hpp"
#include "netw/api/prediction.hpp"

namespace netw::predict {

struct Decl {
    int input_source = NetwPrediction::INPUT_SOURCE_NONE;
    int sim_mode = NetwPrediction::SIM_MODE_DISPLAY;
    int recovery_policy = NetwPrediction::RECOVERY_POLICY_AUTO;
    int breach_response = NetwPrediction::BREACH_RESPONSE_PREDICT_THROUGH;
    godot::Callable input;
    godot::Dictionary sensors;
    int64_t epoch = -1;
    godot::Callable witness;
    godot::Callable correction_path;
    double teleport_threshold = NetwPrediction::DEFAULT_TELEPORT_THRESHOLD;
    int collision_cooldown_ticks = 6;
    bool sleeping = false;
    int missing_policy = NetwPrediction::MISSING_INPUT_STALL;
    int max_consume_per_tick = 2;
    int consume_buffer_ticks = 0;
    int replay_buffer_depth = 0;
    int max_consume_lag_ticks = 60;
    int ack_age_ticks = 0;
    double divergence_epsilon = 0.01;
    int reconcile = NetwPrediction::RECONCILE_INDEPENDENT;
    NetwPrediction::Archetype archetype = NetwPrediction::ARCHETYPE_NONE;
    NetwPredictStats::Book facts;
    godot::Ref<NetwPredictStats> counters;

    Decl() = default;
    Decl(const Decl &) = delete;
    Decl &operator=(const Decl &) = delete;

    ~Decl() {
        if (counters.is_valid()) {
            counters->keep(facts);
        }
    }

    godot::Ref<NetwPredictStats> stats() {
        if (counters.is_null()) {
            counters.instantiate();
            counters->view(&facts);
        }
        return counters;
    }
};

} // namespace netw::predict
