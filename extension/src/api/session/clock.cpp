#include "godot/class_db.hpp"
#include "godot/engine.hpp"
#include "godot/time.hpp"
#include "netw/api/clock_config.hpp"
#include "netw/api/netw_multiplayer.hpp"

using namespace godot;

namespace netw {

Error NetwMultiplayer::clock_configure(const Ref<NetwClockConfig> &p_config) {
    if (p_config.is_null()) {
        return ERR_INVALID_PARAMETER;
    }
    clock_apply_config(p_config);
    clock_engine().set_configured(true);
    clock_engine().set_sweeps_effects(true);
    wire_lagcomp_service();
    return OK;
}

Variant NetwMultiplayer::clock_get_param(ClockParam p_param) const {
    const double value = clock_engine().param(Clock::Param(p_param));
    switch (p_param) {
        case CLOCK_PARAM_TICKRATE:
        case CLOCK_PARAM_SYNC_MODE:
        case CLOCK_PARAM_MAX_TICKS_PER_FRAME:
        case CLOCK_PARAM_PANIC_SNAP_THRESHOLD:
        case CLOCK_PARAM_DISPLAY_OFFSET:
        case CLOCK_PARAM_JITTER_WINDOW:
            return int(value);
        case CLOCK_PARAM_USE_PHYSICS_INTERPOLATION:
        case CLOCK_PARAM_MANUAL_TICK:
            return value != 0.0;
        case CLOCK_PARAM_STALL_THRESHOLD:
        case CLOCK_PARAM_STRETCH_NUDGE_FACTOR:
        case CLOCK_PARAM_LEAD_TICKS:
        case CLOCK_PARAM_JITTER_MULTIPLIER:
        case CLOCK_PARAM_JITTER_STABILITY_THRESHOLD:
        case CLOCK_PARAM_TICK_FACTOR_OVERRIDE:
            return value;
    }
    return Variant();
}

Error NetwMultiplayer::clock_set_param(
    ClockParam p_param,
    const Variant &p_value
) {
    if (p_param == CLOCK_PARAM_TICKRATE) {
        return ERR_UNAUTHORIZED;
    }
    double value = 0.0;
    switch (p_param) {
        case CLOCK_PARAM_SYNC_MODE:
        case CLOCK_PARAM_MAX_TICKS_PER_FRAME:
        case CLOCK_PARAM_PANIC_SNAP_THRESHOLD:
        case CLOCK_PARAM_DISPLAY_OFFSET:
        case CLOCK_PARAM_JITTER_WINDOW:
            value = double(int(p_value));
            break;
        case CLOCK_PARAM_USE_PHYSICS_INTERPOLATION:
        case CLOCK_PARAM_MANUAL_TICK:
            value = bool(p_value) ? 1.0 : 0.0;
            break;
        default:
            value = double(p_value);
            break;
    }
    return clock_engine().set_param(Clock::Param(p_param), value)
        ? OK
        : ERR_INVALID_PARAMETER;
}

double NetwMultiplayer::clock_get_monitor(ClockMonitor p_monitor) const {
    return clock_engine().monitor(Clock::Monitor(p_monitor), clock_reading());
}

double NetwMultiplayer::clock_get_tick_factor() const {
    return clock_engine().tick_factor(clock_reading());
}

double NetwMultiplayer::clock_get_tick_phase() const {
    return clock_engine().tick_phase();
}

double NetwMultiplayer::clock_get_physics_factor() const {
    return clock_engine().physics_factor(clock_reading());
}

int64_t NetwMultiplayer::clock_get_recommended_display_offset() const {
    return clock_engine().recommended_display_offset();
}

int64_t NetwMultiplayer::clock_get_tick() const {
    return clock_engine().get_tick();
}

int64_t NetwMultiplayer::clock_get_simulation_behind_count() const {
    return clock_engine().get_simulation_behind_count();
}

bool NetwMultiplayer::clock_is_configured() const {
    return clock_engine().get_configured();
}

bool NetwMultiplayer::clock_is_synchronized() const {
    return clock_engine().get_synchronized();
}

bool NetwMultiplayer::clock_is_simulating() const {
    return clock_engine().is_simulating();
}

bool NetwMultiplayer::clock_is_gated() const {
    return clock_engine().is_gated();
}

Clock::Reading NetwMultiplayer::clock_reading() const {
    Clock::Reading now;
    if (const Time *clock = Time::get_singleton()) {
        now.wall_usec = clock->get_ticks_usec();
    }
    if (const Engine *engine = Engine::get_singleton()) {
        now.physics_hz = engine->get_physics_ticks_per_second();
        now.interpolation_fraction
            = engine->get_physics_interpolation_fraction();
        now.editor_hint = engine->is_editor_hint();
    }
    return now;
}

void NetwMultiplayer::clock_physics_step(double p_delta) {
    const Ref<NetwMultiplayer> keep(this);
    ++step_depth;
    physics_frame(p_delta);
    --step_depth;
    announce_flush();
    effect_drain();
    predict_free_retired();
}

bool NetwMultiplayer::clock_consume_ping_due(double p_delta) {
    return clock_engine().consume_ping_due(p_delta);
}

void NetwMultiplayer::clock_set_gate(bool p_armed) {
    if (p_armed) {
        clock_engine().arm_gate();
        return;
    }
    clock_engine().release_gate();
}

void NetwMultiplayer::clock_set_synchronized(bool p_value) {
    clock_engine().set_synchronized(p_value);
}

Clock::Pong NetwMultiplayer::clock_ingest_pong(
    const session::ClockPong &p_pong,
    uint64_t p_now_usec,
    bool p_apply_lead
) {
    const bool was_synchronized = clock_engine().get_synchronized();
    const Clock::Pong metrics
        = clock_engine().pong_received(p_pong, p_now_usec, p_apply_lead);
    if (!was_synchronized && clock_engine().get_synchronized()
        && views.clock_handle.is_valid()) {
        Announcement row;
        row.kind = AnnouncementKind::SYNCHRONIZED;
        announce(row);
    }
    return metrics;
}

} // namespace netw
