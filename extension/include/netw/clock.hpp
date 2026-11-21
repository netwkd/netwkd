#pragma once

#include <cstdint>

#include "godot/local_vector.hpp"
#include "netw/clock_exchange.hpp"

namespace netw {

struct TickTime {
    double delta = 0.0;
    int64_t tick = 0;
};

class Clock {
public:
    enum SyncMode {
        SYNC_SNAP = 0,
        SYNC_STRETCH = 1,
    };

    enum Param {
        PARAM_TICKRATE = 0,
        PARAM_SYNC_MODE = 1,
        PARAM_MAX_TICKS_PER_FRAME = 3,
        PARAM_STALL_THRESHOLD = 4,
        PARAM_PANIC_SNAP_THRESHOLD = 5,
        PARAM_STRETCH_NUDGE_FACTOR = 6,
        PARAM_DISPLAY_OFFSET = 7,
        PARAM_LEAD_TICKS = 8,
        PARAM_JITTER_MULTIPLIER = 9,
        PARAM_JITTER_WINDOW = 10,
        PARAM_JITTER_STABILITY_THRESHOLD = 11,
        PARAM_USE_PHYSICS_INTERPOLATION = 12,
        PARAM_MANUAL_TICK = 14,
        PARAM_TICK_FACTOR_OVERRIDE = 15,
    };

    enum Monitor {
        MONITOR_RTT = 0,
        MONITOR_RTT_AVG = 1,
        MONITOR_RTT_JITTER = 2,
        MONITOR_ONE_WAY_LATENCY = 3,
        MONITOR_TICKTIME = 4,
        MONITOR_TICK_ACCUMULATOR = 5,
        MONITOR_PHYSICS_FRAMES = 6,
        MONITOR_POLLS = 7,
        MONITOR_WALL_SECONDS = 8,
        MONITOR_PHYSICS_HZ = 9,
        MONITOR_POLL_HZ = 10,
    };

    enum Mismatch {
        MISMATCH_WARN = 0,
        MISMATCH_DISCONNECT = 1,
        MISMATCH_SIGNAL = 2,
    };

    enum RateVerdict {
        RATE_AGREES,
        RATE_WARNS,
        RATE_DISCONNECTS,
        RATE_SIGNALS,
    };

    static constexpr int64_t NO_MEASUREMENT = -1;
    static constexpr double AUTO_CONFIG_SECONDS = 5.0;

    struct Reading {
        uint64_t wall_usec = 0;
        int physics_hz = 60;
        double interpolation_fraction = 0.0;
        bool editor_hint = false;
    };

    struct Frame {
        bool runs = true;
        bool stalled = false;
    };

    struct Pong {
        double rtt_raw = 0.0;
        double rtt_avg = 0.0;
        double rtt_jitter = 0.0;
        int diff = 0;
        int tick = 0;
        int display_offset = 0;
        int recommended_display_offset = 0;
        bool is_stable = true;
        bool is_synchronized = false;
    };

    struct Cadence {
        int64_t physics_frames = 0;
        int64_t polls = 0;
        double wall_seconds = 0.0;
        double physics_hz = 0.0;
        double poll_hz = 0.0;
    };

    static int64_t pumps_for(double seconds, double rate);

    struct OpenFrame {
        int32_t ticks = 0;
        int32_t closing_tick = 0;
        bool metered = false;
    };

private:
    static constexpr int64_t NEVER_STAMPED = INT64_MIN;

    struct Stats {
        double rtt = 0.0;
        double avg = 0.0;
        double jitter = 0.0;
        bool is_stable = true;
        godot::LocalVector<double> samples;

        void record(double sample, double stability_threshold, int window);
        void clear();
    };

    int32_t tickrate = 30;
    int32_t send_interval = 1;
    bool configured = false;
    bool use_physics_interpolation = true;
    double tick_factor_override = -1.0;
    int32_t max_ticks_per_frame = 8;
    double stall_threshold = 1.0;
    SyncMode sync_mode = SYNC_STRETCH;
    int32_t panic_snap_threshold = 20;
    double stretch_nudge_factor = 0.05;
    double ping_interval = 0.1;
    double lead_ticks = 1.0;
    int32_t display_offset = 2;
    double jitter_multiplier = 2.0;
    int32_t jitter_window = 16;
    double jitter_stability_threshold = 0.05;
    bool manual_tick = false;
    bool enable_drift_logging = false;

    static constexpr int32_t NO_CLOSING_TICK = INT32_MIN;
    static constexpr int32_t NEVER_SENT = INT32_MIN / 2;
    static constexpr double SETTLED_PING_PERIOD = 1.0;

    int32_t tick = 0;
    int32_t closing_tick = NO_CLOSING_TICK;
    int32_t opened_tick = 0;
    int32_t sent_tick = NEVER_SENT;
    bool is_synchronized = false;
    bool simulating = true;
    bool held = false;
    int32_t simulation_behind_count = 0;

    double accumulator = 0.0;
    double target_tick_estimate = 0.0;
    double ping_timer = 0.0;
    int64_t step_stamp_usec = NEVER_STAMPED;

    int32_t simulation_gates = 0;
    int32_t simulation_credit = 0;

    int32_t frame_ticks = 0;
    int32_t frame_ceiling = 0;
    double frame_ticktime = 0.0;
    bool frame_metered = false;

    int64_t physics_frames = 0;
    int64_t polls = 0;
    int64_t ticks_run = 0;
    uint64_t cadence_started_usec = 0;

    Stats stats;

    Mismatch mismatch_action = MISMATCH_WARN;
    double auto_config_left = 0.0;
    int64_t auto_config_best = 0;
    uint64_t handshake_generation = 0;
    bool sweeps_effects = false;
    bool handshakes_on_enter = false;

    double seconds_into_frame(const Reading &p_now) const;
    void calibrate(double target);
    void nudge_toward_estimate();

public:
    ClockExchange exchange;

    void set_mismatch_action(Mismatch value);
    Mismatch get_mismatch_action() const;
    RateVerdict tickrate_verdict(int p_server_tickrate) const;
    void set_sweeps_effects(bool value);
    bool get_sweeps_effects() const;
    void set_handshakes_on_enter(bool value);
    bool get_handshakes_on_enter() const;
    bool handshake_opens(uint64_t p_generation);
    void auto_config_start();
    void auto_config_cancel();
    int64_t auto_config_step(double p_delta);
    void release();

    void set_tickrate(int value);
    int get_tickrate() const;
    void set_send_interval(int value);
    int get_send_interval() const;
    int send_rate() const;
    void set_max_ticks_per_frame(int value);
    int get_max_ticks_per_frame() const;
    void set_stall_threshold(double value);
    double get_stall_threshold() const;
    void set_sync_mode(SyncMode value);
    SyncMode get_sync_mode() const;
    void set_panic_snap_threshold(int value);
    int get_panic_snap_threshold() const;
    void set_stretch_nudge_factor(double value);
    double get_stretch_nudge_factor() const;
    void set_ping_interval(double value);
    double get_ping_interval() const;
    void set_lead_ticks(double value);
    double get_lead_ticks() const;
    void set_display_offset(int value);
    int get_display_offset() const;
    void set_jitter_multiplier(double value);
    double get_jitter_multiplier() const;
    void set_jitter_window(int value);
    int get_jitter_window() const;
    void set_jitter_stability_threshold(double value);
    double get_jitter_stability_threshold() const;
    void set_configured(bool value);
    bool get_configured() const;
    void set_use_physics_interpolation(bool value);
    bool get_use_physics_interpolation() const;
    void set_tick_factor_override(double value);
    double get_tick_factor_override() const;
    void set_manual_tick(bool value);
    bool get_manual_tick() const;
    void set_enable_drift_logging(bool value);
    bool get_enable_drift_logging() const;

    void set_tick(int value);
    int get_tick() const;
    int64_t get_ticks_run() const;
    bool is_closing_tick() const;
    bool closed_a_tick() const;
    int32_t get_send_tick() const;
    bool is_send_tick() const;
    bool sent_last_tick() const;
    void mark_sent();
    void set_synchronized(bool value);
    bool get_synchronized() const;

    double ticktime() const;
    int display_tick() const;
    double physics_factor(const Reading &p_now) const;
    int physics_steps_per_tick(const Reading &p_now) const;
    double tick_phase() const;
    double tick_accumulator() const;
    double tick_factor(const Reading &p_now) const;
    double rtt() const;
    double rtt_avg() const;
    double rtt_jitter() const;
    double one_way_latency() const;
    int recommended_display_offset() const;
    bool is_stable() const;

    Frame frame_open(double delta, const Reading &p_now);
    void accumulate(double delta);
    bool tick_due() const;
    TickTime tick_open();
    void tick_close();
    void resolve_simulation_gate(const Reading &p_now);
    void frame_close();
    OpenFrame frame_set_aside();
    void frame_resume(const OpenFrame &p_frame);

    void count_poll(const Reading &p_now);
    Cadence cadence(const Reading &p_now) const;

    double seconds_since_step(const Reading &p_now) const;
    void mark_step(const Reading &p_now, double seconds_ago = 0.0);

    bool is_simulating() const;
    int get_simulation_behind_count() const;

    void arm_gate();
    void release_gate();
    bool is_gated() const;

    void set_held(bool p_held);
    bool is_held() const;

    Pong pong_received(
        const session::ClockPong &p_pong,
        uint64_t p_now_usec,
        bool p_apply_lead
    );

    double param(Param p_param) const;
    bool set_param(Param p_param, double p_value);
    double monitor(Monitor p_monitor, const Reading &p_now) const;

    bool is_settled() const;
    bool consume_ping_due(double delta);

    void forget_rtt();
    void clear();
};

} // namespace netw
