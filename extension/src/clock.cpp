#include "netw/sealed.hpp"

#include "netw/clock.hpp"

#include <algorithm>
#include <cmath>

using namespace godot;

namespace netw {

void Clock::Stats::record(
    double sample,
    double stability_threshold,
    int window
) {
    rtt = sample;
    samples.push_back(sample);
    const uint32_t keep = uint32_t(window > 1 ? window : 1);
    while (samples.size() > keep) {
        samples.remove_at(0);
    }

    double sum = 0.0;
    for (const double value : samples) {
        sum += value;
    }
    avg = sum / double(samples.size());

    double deviation = 0.0;
    for (const double value : samples) {
        deviation += std::fabs(value - avg);
    }
    jitter = deviation / double(samples.size());
    is_stable = jitter < stability_threshold;
}

void Clock::Stats::clear() {
    rtt = 0.0;
    avg = 0.0;
    jitter = 0.0;
    is_stable = true;
    samples.clear();
}

#define NETW_CLOCK_ACCESSOR(m_type, m_name, m_field) \
    void Clock::set_##m_name(m_type value) { \
        m_field = value; \
    } \
    m_type Clock::get_##m_name() const { \
        return m_field; \
    }

NETW_CLOCK_ACCESSOR(int, tickrate, tickrate)
NETW_CLOCK_ACCESSOR(int, send_interval, send_interval)
NETW_CLOCK_ACCESSOR(int, max_ticks_per_frame, max_ticks_per_frame)
NETW_CLOCK_ACCESSOR(double, stall_threshold, stall_threshold)
NETW_CLOCK_ACCESSOR(int, panic_snap_threshold, panic_snap_threshold)
NETW_CLOCK_ACCESSOR(double, stretch_nudge_factor, stretch_nudge_factor)
NETW_CLOCK_ACCESSOR(double, lead_ticks, lead_ticks)
NETW_CLOCK_ACCESSOR(int, display_offset, display_offset)
NETW_CLOCK_ACCESSOR(double, jitter_multiplier, jitter_multiplier)
NETW_CLOCK_ACCESSOR(int, jitter_window, jitter_window)
NETW_CLOCK_ACCESSOR(
    double,
    jitter_stability_threshold,
    jitter_stability_threshold
)
NETW_CLOCK_ACCESSOR(int, tick, tick)
NETW_CLOCK_ACCESSOR(bool, synchronized, is_synchronized)
NETW_CLOCK_ACCESSOR(bool, configured, configured)
NETW_CLOCK_ACCESSOR(bool, use_physics_interpolation, use_physics_interpolation)
NETW_CLOCK_ACCESSOR(double, tick_factor_override, tick_factor_override)
NETW_CLOCK_ACCESSOR(Clock::Mismatch, mismatch_action, mismatch_action)
NETW_CLOCK_ACCESSOR(bool, sweeps_effects, sweeps_effects)
NETW_CLOCK_ACCESSOR(bool, handshakes_on_enter, handshakes_on_enter)

#undef NETW_CLOCK_ACCESSOR

Clock::RateVerdict Clock::tickrate_verdict(int p_server_tickrate) const {
    if (p_server_tickrate == tickrate) {
        return RATE_AGREES;
    }
    if (mismatch_action == MISMATCH_DISCONNECT) {
        return RATE_DISCONNECTS;
    }
    if (mismatch_action == MISMATCH_SIGNAL) {
        return RATE_SIGNALS;
    }
    return RATE_WARNS;
}

bool Clock::handshake_opens(uint64_t p_generation) {
    if (handshake_generation == p_generation) {
        return false;
    }
    handshake_generation = p_generation;
    return true;
}

void Clock::auto_config_start() {
    auto_config_left = AUTO_CONFIG_SECONDS;
    auto_config_best = 0;
}

void Clock::auto_config_cancel() {
    auto_config_left = 0.0;
}

int64_t Clock::auto_config_step(double p_delta) {
    if (auto_config_left <= 0.0) {
        return NO_MEASUREMENT;
    }
    const int64_t seen = recommended_display_offset();
    auto_config_best = seen > auto_config_best ? seen : auto_config_best;
    auto_config_left -= p_delta;
    if (auto_config_left > 0.0) {
        return NO_MEASUREMENT;
    }
    auto_config_left = 0.0;
    set_display_offset(int(auto_config_best));
    return auto_config_best;
}

void Clock::release() {
    auto_config_left = 0.0;
    handshake_generation = 0;
}

void Clock::set_sync_mode(SyncMode value) {
    sync_mode = value;
}

Clock::SyncMode Clock::get_sync_mode() const {
    return sync_mode;
}

double Clock::ticktime() const {
    return 1.0 / double(tickrate);
}

int Clock::display_tick() const {
    const int shown = tick - display_offset;
    return shown > 0 ? shown : 0;
}

double Clock::physics_factor(const Reading &p_now) const {
    return double(p_now.physics_hz) / double(tickrate);
}

int Clock::physics_steps_per_tick(const Reading &p_now) const {
    const int steps = int(std::lround(physics_factor(p_now)));
    return steps > 1 ? steps : 1;
}

double Clock::tick_phase() const {
    const double phase = accumulator / ticktime();
    if (phase < 0.0) {
        return 0.0;
    }
    return phase > 1.0 ? 1.0 : phase;
}

double Clock::tick_accumulator() const {
    return accumulator;
}

double Clock::seconds_into_frame(const Reading &p_now) const {
    if (use_physics_interpolation) {
        return p_now.interpolation_fraction / double(p_now.physics_hz);
    }
    const double span = seconds_since_step(p_now);
    return span > 0.0 ? span : 0.0;
}

double Clock::tick_factor(const Reading &p_now) const {
    if (tick_factor_override >= 0.0) {
        return tick_factor_override;
    }
    if (!configured || p_now.editor_hint) {
        return 0.0;
    }
    return (accumulator + seconds_into_frame(p_now)) / ticktime();
}

double Clock::seconds_since_step(const Reading &p_now) const {
    if (step_stamp_usec == NEVER_STAMPED) {
        return -1.0;
    }
    return double(int64_t(p_now.wall_usec) - step_stamp_usec) / 1'000'000.0;
}

void Clock::mark_step(const Reading &p_now, double seconds_ago) {
    const int64_t back
        = seconds_ago > 0.0 ? int64_t(seconds_ago * 1'000'000.0) : 0;
    step_stamp_usec = int64_t(p_now.wall_usec) - back;
}

double Clock::rtt() const {
    return stats.rtt;
}

double Clock::rtt_avg() const {
    return stats.avg;
}

double Clock::rtt_jitter() const {
    return stats.jitter;
}

double Clock::one_way_latency() const {
    return stats.avg * 0.5;
}

int Clock::recommended_display_offset() const {
    if (!is_synchronized) {
        return display_offset;
    }
    return int(std::ceil(
               (one_way_latency() + stats.jitter * jitter_multiplier)
               * double(tickrate)
           ))
        + send_interval - 1;
}

bool Clock::is_stable() const {
    return stats.is_stable;
}

bool Clock::is_simulating() const {
    return simulating;
}

int Clock::get_simulation_behind_count() const {
    return simulation_behind_count;
}

int64_t Clock::get_ticks_run() const {
    return ticks_run;
}

Clock::Frame Clock::frame_open(double delta, const Reading &p_now) {
    Frame frame;
    physics_frames += 1;
    if (cadence_started_usec == 0) {
        cadence_started_usec = p_now.wall_usec;
    }
    mark_step(p_now);
    if (delta > stall_threshold) {
        frame.stalled = true;
        accumulator = 0.0;
        simulation_credit = 0;
    }
    if (held) {
        accumulator = 0.0;
        resolve_simulation_gate(p_now);
        frame.runs = false;
    }
    return frame;
}

void Clock::accumulate(double delta) {
    accumulator += delta;

    if (is_synchronized && sync_mode == SYNC_STRETCH) {
        target_tick_estimate += delta * double(tickrate);
        nudge_toward_estimate();
    }

    frame_ceiling = simulation_gates > 0 ? 1 : max_ticks_per_frame;
    frame_ticktime = ticktime();
    frame_metered = true;
}

bool Clock::tick_due() const {
    return accumulator >= frame_ticktime && frame_ticks < frame_ceiling;
}

TickTime Clock::tick_open() {
    if (frame_metered) {
        accumulator -= frame_ticktime;
        if (accumulator < frame_ticktime || frame_ticks + 1 >= frame_ceiling) {
            closing_tick = tick;
        }
    }
    opened_tick = tick;
    return {ticktime(), tick};
}

void Clock::tick_close() {
    tick += 1;
    ticks_run += 1;
    frame_ticks += 1;
}

void Clock::frame_close() {
    closing_tick = NO_CLOSING_TICK;
    frame_metered = false;
}

Clock::OpenFrame Clock::frame_set_aside() {
    const OpenFrame open = {frame_ticks, closing_tick, frame_metered};
    frame_ticks = 0;
    frame_metered = false;
    return open;
}

void Clock::frame_resume(const OpenFrame &p_frame) {
    frame_ticks = p_frame.ticks;
    closing_tick = p_frame.closing_tick;
    frame_metered = p_frame.metered;
}

bool Clock::is_closing_tick() const {
    return closing_tick == tick;
}

bool Clock::closed_a_tick() const {
    return closing_tick != NO_CLOSING_TICK;
}

int32_t Clock::get_send_tick() const {
    return closing_tick != NO_CLOSING_TICK ? closing_tick : tick;
}

int Clock::send_rate() const {
    return tickrate / std::max(1, send_interval);
}

bool Clock::is_send_tick() const {
    const int64_t waited = int64_t(opened_tick) - int64_t(sent_tick);
    return send_interval <= 1 || waited <= 0 || waited >= send_interval;
}

bool Clock::sent_last_tick() const {
    return send_interval <= 1 || int64_t(opened_tick) - int64_t(sent_tick) <= 1;
}

void Clock::mark_sent() {
    sent_tick = opened_tick;
}

void Clock::set_manual_tick(bool value) {
    manual_tick = value;
    if (value) {
        is_synchronized = true;
    }
}

bool Clock::get_manual_tick() const {
    return manual_tick;
}

void Clock::set_enable_drift_logging(bool value) {
    enable_drift_logging = value;
}

bool Clock::get_enable_drift_logging() const {
    return enable_drift_logging;
}

void Clock::count_poll(const Reading &p_now) {
    polls += 1;
    if (cadence_started_usec == 0) {
        cadence_started_usec = p_now.wall_usec;
    }
}

Clock::Cadence Clock::cadence(const Reading &p_now) const {
    const double wall = cadence_started_usec == 0
        ? 0.0
        : double(p_now.wall_usec - cadence_started_usec) / 1'000'000.0;
    Cadence out;
    out.physics_frames = physics_frames;
    out.polls = polls;
    out.wall_seconds = wall;
    out.physics_hz = wall > 0.0 ? double(physics_frames) / wall : 0.0;
    out.poll_hz = wall > 0.0 ? double(polls) / wall : 0.0;
    return out;
}

double Clock::param(Param p_param) const {
    switch (p_param) {
        case PARAM_TICKRATE:
            return double(get_tickrate());
        case PARAM_SYNC_MODE:
            return double(int(get_sync_mode()));
        case PARAM_MAX_TICKS_PER_FRAME:
            return double(get_max_ticks_per_frame());
        case PARAM_STALL_THRESHOLD:
            return get_stall_threshold();
        case PARAM_PANIC_SNAP_THRESHOLD:
            return double(get_panic_snap_threshold());
        case PARAM_STRETCH_NUDGE_FACTOR:
            return get_stretch_nudge_factor();
        case PARAM_DISPLAY_OFFSET:
            return double(get_display_offset());
        case PARAM_LEAD_TICKS:
            return get_lead_ticks();
        case PARAM_JITTER_MULTIPLIER:
            return get_jitter_multiplier();
        case PARAM_JITTER_WINDOW:
            return double(get_jitter_window());
        case PARAM_JITTER_STABILITY_THRESHOLD:
            return get_jitter_stability_threshold();
        case PARAM_USE_PHYSICS_INTERPOLATION:
            return get_use_physics_interpolation() ? 1.0 : 0.0;
        case PARAM_MANUAL_TICK:
            return get_manual_tick() ? 1.0 : 0.0;
        case PARAM_TICK_FACTOR_OVERRIDE:
            return get_tick_factor_override();
    }
    return 0.0;
}

bool Clock::set_param(Param p_param, double p_value) {
    switch (p_param) {
        case PARAM_TICKRATE:
            return false;
        case PARAM_SYNC_MODE:
            set_sync_mode(SyncMode(int(p_value)));
            return true;
        case PARAM_MAX_TICKS_PER_FRAME:
            set_max_ticks_per_frame(int(p_value));
            return true;
        case PARAM_STALL_THRESHOLD:
            set_stall_threshold(p_value);
            return true;
        case PARAM_PANIC_SNAP_THRESHOLD:
            set_panic_snap_threshold(int(p_value));
            return true;
        case PARAM_STRETCH_NUDGE_FACTOR:
            set_stretch_nudge_factor(p_value);
            return true;
        case PARAM_DISPLAY_OFFSET:
            set_display_offset(int(p_value));
            return true;
        case PARAM_LEAD_TICKS:
            set_lead_ticks(p_value);
            return true;
        case PARAM_JITTER_MULTIPLIER:
            set_jitter_multiplier(p_value);
            return true;
        case PARAM_JITTER_WINDOW:
            set_jitter_window(int(p_value));
            return true;
        case PARAM_JITTER_STABILITY_THRESHOLD:
            set_jitter_stability_threshold(p_value);
            return true;
        case PARAM_USE_PHYSICS_INTERPOLATION:
            set_use_physics_interpolation(p_value != 0.0);
            return true;
        case PARAM_MANUAL_TICK:
            set_manual_tick(p_value != 0.0);
            return true;
        case PARAM_TICK_FACTOR_OVERRIDE:
            set_tick_factor_override(p_value);
            return true;
    }
    return false;
}

double Clock::monitor(Monitor p_monitor, const Reading &p_now) const {
    switch (p_monitor) {
        case MONITOR_RTT:
            return rtt();
        case MONITOR_RTT_AVG:
            return rtt_avg();
        case MONITOR_RTT_JITTER:
            return rtt_jitter();
        case MONITOR_ONE_WAY_LATENCY:
            return one_way_latency();
        case MONITOR_TICKTIME:
            return ticktime();
        case MONITOR_TICK_ACCUMULATOR:
            return tick_accumulator();
        case MONITOR_PHYSICS_FRAMES:
            return double(cadence(p_now).physics_frames);
        case MONITOR_POLLS:
            return double(cadence(p_now).polls);
        case MONITOR_WALL_SECONDS:
            return cadence(p_now).wall_seconds;
        case MONITOR_PHYSICS_HZ:
            return cadence(p_now).physics_hz;
        case MONITOR_POLL_HZ:
            return cadence(p_now).poll_hz;
    }
    return 0.0;
}

void Clock::resolve_simulation_gate(const Reading &p_now) {
    const int32_t ticks_this_frame = frame_ticks;
    frame_ticks = 0;
    if (simulation_gates <= 0) {
        simulating = true;
        simulation_credit = 0;
        return;
    }
    if (accumulator >= ticktime()) {
        simulation_behind_count += 1;
    }
    simulation_credit += ticks_this_frame * physics_steps_per_tick(p_now);
    simulating = simulation_credit > 0;
    if (simulating) {
        simulation_credit -= 1;
    }
}

void Clock::arm_gate() {
    simulation_gates += 1;
}

void Clock::release_gate() {
    simulation_gates = simulation_gates > 1 ? simulation_gates - 1 : 0;
    if (simulation_gates == 0) {
        simulating = true;
        simulation_credit = 0;
    }
}

bool Clock::is_gated() const {
    return simulation_gates > 0;
}

void Clock::set_held(bool p_held) {
    held = p_held;
    accumulator = 0.0;
}

bool Clock::is_held() const {
    return held;
}

Clock::Pong Clock::pong_received(
    const session::ClockPong &p_pong,
    uint64_t p_now_usec,
    bool p_apply_lead
) {
    const uint32_t elapsed
        = uint32_t(p_now_usec & 0xFFFFFFFF) - uint32_t(p_pong.origin);
    const uint64_t waited = p_pong.wait * ClockExchange::WAIT_UNIT_USEC;
    const uint64_t travelled = elapsed > waited ? elapsed - waited : 0;
    const double sample = double(travelled) / 1'000'000.0;
    const int server_tick_at_pong = int(uint32_t(p_pong.tick));
    const double server_tick_phase = double(p_pong.phase) / 255.0;

    stats.record(sample, jitter_stability_threshold, jitter_window);

    const double lead = p_apply_lead
        ? stats.avg * 0.5 / ticktime() + lead_ticks + double(send_interval - 1)
        : 0.0;
    const double target
        = double(server_tick_at_pong) + server_tick_phase + lead;
    const int pre_calibrate_diff = int(std::lround(target)) - tick;

    calibrate(target);

    Pong metrics;
    metrics.rtt_raw = sample;
    metrics.rtt_avg = stats.avg;
    metrics.rtt_jitter = stats.jitter;
    metrics.diff = pre_calibrate_diff;
    metrics.tick = tick;
    metrics.display_offset = display_offset;
    metrics.recommended_display_offset = recommended_display_offset();
    metrics.is_stable = stats.is_stable;
    metrics.is_synchronized = is_synchronized;
    return metrics;
}

void Clock::calibrate(double target) {
    const int whole = int(std::floor(target));

    if (!is_synchronized) {
        tick = whole;
        accumulator = (target - double(whole)) * ticktime();
        target_tick_estimate = target;
        is_synchronized = true;
        return;
    }

    if (sync_mode == SYNC_SNAP) {
        tick = whole;
        accumulator = (target - double(whole)) * ticktime();
    } else {
        target_tick_estimate = target;
    }
}

void Clock::nudge_toward_estimate() {
    const double step = ticktime();
    const double current = double(tick) + accumulator / step;
    const double divergence = target_tick_estimate - current;
    const bool owes_past_catch_up = simulation_gates > 0
        && accumulator > double(max_ticks_per_frame) * step;

    if (std::fabs(divergence) > double(panic_snap_threshold)
        || owes_past_catch_up) {
        tick = int(std::lround(target_tick_estimate));
        accumulator = (target_tick_estimate - double(tick)) * step;
        return;
    }

    accumulator += divergence * step * stretch_nudge_factor;
}

bool Clock::is_settled() const {
    return int(stats.samples.size()) >= jitter_window && stats.is_stable
        && stats.jitter < ticktime() * 0.5;
}

bool Clock::consume_ping_due(double delta) {
    ping_timer += delta;
    const double due = is_settled() ? SETTLED_PING_PERIOD : ping_interval;
    if (ping_timer >= due) {
        ping_timer = 0.0;
        return true;
    }
    return false;
}

void Clock::clear() {
    tick = 0;
    opened_tick = 0;
    sent_tick = NEVER_SENT;
    is_synchronized = false;
    simulating = true;
    held = false;
    simulation_behind_count = 0;
    accumulator = 0.0;
    target_tick_estimate = 0.0;
    ping_timer = 0.0;
    step_stamp_usec = NEVER_STAMPED;
    simulation_gates = 0;
    simulation_credit = 0;
    physics_frames = 0;
    polls = 0;
    cadence_started_usec = 0;
    stats.clear();
}

void Clock::forget_rtt() {
    stats.clear();
}

int64_t Clock::pumps_for(double seconds, double rate) {
    return int64_t(std::ceil(seconds * std::max(1.0, rate)));
}

} // namespace netw
