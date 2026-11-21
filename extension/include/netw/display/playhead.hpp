#pragma once

#include <cstdint>

#include "netw/display/decl.hpp"

namespace netw::display {

class Playhead {
private:
    double display_lag = 0.0;
    double smoothed_floor = 0.0;
    int32_t starvation_ticks = 0;
    int32_t expected_interval_ticks = 3;
    int32_t send_interval_ticks = 1;
    int32_t display_tick = -1;
    int64_t newest_heard = -1;
    bool has_arrival_age = false;
    double arrival_age = 0.0;
    double arrival_age_mean = 0.0;
    double arrival_age_deviation = 0.0;

public:
    void hear_newest(const Decl &decl, int64_t newest_tick, double now);
    void forget_arrivals();
    double arrival_floor(const Decl &decl) const;

    double min_lag(
        const Decl &decl,
        int display_offset,
        int recommended_display_offset
    ) const;

    int effective_tick(int clock_display_tick, double tick_factor) const;

    void dilate(
        const Decl &decl,
        int display_offset,
        int recommended_display_offset,
        bool starving
    );

    double place(int clock_display_tick, double tick_factor, bool bracketed);

    void settle(
        const Decl &decl,
        int display_offset,
        int recommended_display_offset
    );

    void set_display_lag(double value) {
        display_lag = value;
    }
    double get_display_lag() const {
        return display_lag;
    }
    int get_starvation_ticks() const {
        return starvation_ticks;
    }
    void set_expected_interval_ticks(int value) {
        expected_interval_ticks = value;
    }
    int get_expected_interval_ticks() const {
        return expected_interval_ticks;
    }
    void set_send_interval_ticks(int value) {
        send_interval_ticks = value;
    }
    void set_display_tick(int value) {
        display_tick = value;
    }
    int get_display_tick() const {
        return display_tick;
    }
    bool hears_arrivals() const {
        return has_arrival_age;
    }
    double get_arrival_age() const {
        return arrival_age;
    }
};

} // namespace netw::display
