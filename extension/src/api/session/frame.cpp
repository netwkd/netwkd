#include "netw/api/clock.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/colors.hpp"
#include "netw/profile.hpp"
#include "netw/spawn/pipeline.hpp"

using namespace godot;

namespace netw {

void NetwMultiplayer::emit_before_tick(TickTime p_now) {
    if (views.clock_handle.is_valid()) {
        views.clock_handle->announce_before_tick(p_now.delta, p_now.tick);
    }
}

void NetwMultiplayer::sweep_effects(TickTime p_now) {
    if (world.clock.get_sweeps_effects()) {
        clock_sweep_effects(p_now.delta, p_now.tick);
    }
}

void NetwMultiplayer::reveal_spawns(TickTime p_now) {
    if (spawn::Pipeline *pipeline = spawn_plane()) {
        if (pipeline->reveals_action_gates()) {
            spawn_on_action_reveal_tick(p_now.delta, p_now.tick);
        }
        if (pipeline->awaits_unadded()) {
            pipeline->report_unadded(world.clock.get_ticks_run());
        }
    }
}

void NetwMultiplayer::display_record_tick(TickTime p_now) {
    if (display_session_bound) {
        display_on_clock_tick(p_now.delta, p_now.tick);
    }
}

void NetwMultiplayer::flush_tick(TickTime p_now) {
    session_on_clock_tick(p_now.delta, p_now.tick);
}

void NetwMultiplayer::emit_ticked(TickTime p_now) {
    if (views.clock_handle.is_valid()) {
        views.clock_handle->announce_ticked(p_now.delta, p_now.tick);
    }
}

void NetwMultiplayer::frame_begin() {
    before_frame_step();
}

void NetwMultiplayer::run_tick() {
    NETW_FRAME(profile::names::TICK_FRAME);
    NETW_ZONE_NC("Tick", colors::CLOCK);
    const TickTime now = world.clock.tick_open();
    NETW_ZONE_VALUE(now.tick);
    emit_before_tick(now);
    tick_step(now.delta, now.tick);
    sweep_effects(now);
    reveal_spawns(now);
    display_record_tick(now);
    flush_tick(now);
    emit_ticked(now);
    NETW_TALLY_FLUSH();
    world.clock.tick_close();
}

void NetwMultiplayer::frame_end() {
    frame_step();
    world.clock.frame_close();
}

void NetwMultiplayer::physics_frame(double p_delta) {
    NETW_FRAME(profile::names::PHYSICS_FRAME);
    NETW_ZONE_NC("Physics frame", colors::CLOCK);
    const Clock::Frame frame = world.clock.frame_open(p_delta, clock_reading());
    if (frame.stalled) {
        fault::raise(
            this,
            NETW_FAULT_FRAME_TOO_LONG,
            {String(),
             String(),
             String(),
             0,
             int64_t(p_delta * 1000.0),
             int64_t(world.clock.get_stall_threshold() * 1000.0)}
        );
    }
    if (!frame.runs) {
        return;
    }
    frame_begin();
    world.clock.accumulate(p_delta);
    while (world.clock.tick_due()) {
        run_tick();
    }
    world.clock.resolve_simulation_gate(clock_reading());
    frame_end();
}

void NetwMultiplayer::clock_step(int64_t p_count) {
    const Ref<NetwMultiplayer> keep(this);
    ++step_depth;
    const Clock::OpenFrame outer = world.clock.frame_set_aside();
    frame_begin();
    const int64_t count = world.clock.is_held() ? 0 : p_count;
    for (int64_t at = 0; at < count; ++at) {
        run_tick();
    }
    world.clock.resolve_simulation_gate(clock_reading());
    frame_end();
    world.clock.frame_resume(outer);
    --step_depth;
    announce_flush();
    effect_drain();
    predict_free_retired();
}

} // namespace netw
