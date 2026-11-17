#include "support/netw_test.h"

#if defined(NETW_TIER_HOSTED)

#include "support/interp_stand.h"
#include "support/netw_cells.h"

namespace TestInterpCalculus {

using namespace godot;
using namespace netw_test;

constexpr double DURATION = 5.0;
constexpr int64_t SEED = 1234;
constexpr double STEP_SLACK = 2.5;
constexpr double STEP_SLACK_DEGRADED = 3.5;
constexpr double JERK_SLACK = 3.5;

struct Cell {
    String label;
    Oracle truth;
    LinkPreset link;
    double step_slack = STEP_SLACK;
    bool judges_smooth = true;
    bool judges_trail = false;
};

struct Displayed {
    Metrics measured;
    double step_bound = 0.0;
    double jerk_bound = 0.0;
    double lag_max = 0.0;
    double truth_speed = 0.0;
};

Displayed display_run(const Cell &p_cell) {
    InterpHarness harness;
    harness.configure(0.05, p_cell.truth.value_at(0.0));
    harness.run(p_cell.truth, p_cell.link, DURATION, SEED);

    Displayed out;
    out.measured = analyze(harness, p_cell.truth, harness.warmup_hint());
    const double dt = harness.frame_dt();
    out.step_bound = p_cell.truth.speed_max() * dt * p_cell.step_slack;
    out.jerk_bound = p_cell.truth.accel_max() * dt * dt
        + JERK_SLACK * p_cell.truth.speed_max() * dt;
    out.lag_max = double(harness.display_offset) / harness.tickrate + 0.30;
    out.truth_speed = p_cell.truth.speed_max();
    return out;
}

typedef LawRowFor<Displayed> CalculusLaw;

LawVerdict law_sampled(const Displayed &p_run) {
    if (p_run.measured.samples <= 0) {
        return law_broken("the run displayed nothing past its warmup");
    }
    return law_held();
}

LawVerdict law_monotonic(const Displayed &p_run) {
    if (!p_run.measured.playhead_monotonic) {
        return law_broken(
            "the playhead ran backward by %.4f ticks",
            p_run.measured.max_playhead_backstep
        );
    }
    return law_held();
}

LawVerdict law_continuous(const Displayed &p_run) {
    if (p_run.measured.max_step > p_run.step_bound) {
        return law_broken(
            "one frame stepped %.3f against a %.3f motion bound",
            p_run.measured.max_step,
            p_run.step_bound
        );
    }
    return law_held();
}

LawVerdict law_steady(const Displayed &p_run) {
    if (p_run.measured.regressions != 0) {
        return law_broken(
            "%d frames moved backward",
            p_run.measured.regressions
        );
    }
    if (p_run.measured.stalls != 0) {
        return law_broken(
            "%d frames stalled while the truth moved",
            p_run.measured.stalls
        );
    }
    if (p_run.measured.moved <= 0) {
        return law_broken("the display never moved at all");
    }
    const double error = std::abs(p_run.measured.mean_speed - p_run.truth_speed)
        / p_run.truth_speed;
    if (error >= 0.05) {
        return law_broken(
            "the mean speed %.2f is %.1f%% off the truth",
            p_run.measured.mean_speed,
            error * 100.0
        );
    }
    return law_held();
}

LawVerdict law_trails(const Displayed &p_run) {
    if (p_run.measured.led_truth) {
        return law_broken(
            "the display led the truth by %.3f s",
            -p_run.measured.min_lag_sec
        );
    }
    if (p_run.measured.max_lag_sec > p_run.lag_max) {
        return law_broken(
            "the display trailed %.3f s against a %.3f s ceiling",
            p_run.measured.max_lag_sec,
            p_run.lag_max
        );
    }
    return law_held();
}

LawVerdict law_smooth(const Displayed &p_run) {
    if (p_run.measured.max_jerk > p_run.jerk_bound) {
        return law_broken(
            "the jerk %.3f exceeds the %.3f curvature bound",
            p_run.measured.max_jerk,
            p_run.jerk_bound
        );
    }
    return law_held();
}

const CalculusLaw L_SAMPLED = {
    "sampled",
    "the run produced frames past its warmup, so the laws read something",
    &law_sampled,
};

const CalculusLaw L_MONOTONIC = {
    "monotonic",
    "display time never runs backward",
    &law_monotonic,
};

const CalculusLaw L_CONTINUOUS = {
    "continuous",
    "no frame steps further than the motion allows, so nothing teleports",
    &law_continuous,
};

const CalculusLaw L_STEADY = {
    "steady",
    "the display moves forward every frame at the truth's own speed",
    &law_steady,
};

const CalculusLaw L_TRAILS = {
    "trails",
    "the display trails the truth by a bounded, non-negative delay",
    &law_trails,
};

const CalculusLaw L_SMOOTH = {
    "smooth",
    "the second difference stays under the curvature and dilation bound",
    &law_smooth,
};

void link_cells(
    const LinkPreset &p_link,
    double p_slack,
    bool p_smooth,
    std::vector<Cell> &r_cells
) {
    const Oracle TRUTHS[] = {linear_truth(), circle_truth()};
    for (const Oracle &truth : TRUTHS) {
        Cell out;
        out.label = String(truth.label) + "/" + String(p_link.label);
        out.truth = truth;
        out.link = p_link;
        out.step_slack = p_slack;
        out.judges_smooth = p_smooth;
        out.judges_trail = p_smooth && truth.kind == Oracle::LINEAR;
        r_cells.push_back(out);
    }
}

TEST_CASE(
    "[Netw][Display][Calculus] a remote body drawn over links from "
    "clean to heavy loss moves forward at its own speed, trails by a "
    "bounded delay, and never jumps"
) {
    std::vector<Cell> cells;
    link_cells(perfect_link(), STEP_SLACK, true, cells);
    link_cells(wifi_link(), STEP_SLACK, true, cells);
    link_cells(mobile_4g_link(), STEP_SLACK, true, cells);
    link_cells(poor_3g_link(), STEP_SLACK_DEGRADED, true, cells);
    link_cells(satellite_link(), STEP_SLACK_DEGRADED, true, cells);
    link_cells(heavy_loss_link(), STEP_SLACK_DEGRADED, false, cells);

    const CalculusLaw LAWS[] = {L_SAMPLED, L_MONOTONIC, L_CONTINUOUS, L_STEADY};
    for (const Cell &declared : cells) {
        const Displayed run = display_run(declared);
        for (const CalculusLaw &law : LAWS) {
            NETW_CELL(law, declared);
            NETW_LAW_HOLDS(law, run);
        }
        if (declared.judges_smooth) {
            NETW_CELL(L_SMOOTH, declared);
            NETW_LAW_HOLDS(L_SMOOTH, run);
        }
        if (declared.judges_trail) {
            NETW_CELL(L_TRAILS, declared);
            NETW_LAW_HOLDS(L_TRAILS, run);
        }
    }
}

struct Bench {
    String label;
};

typedef LawRowFor<Bench> Row;

void settle_chase(InterpHarness &r_harness) {
    r_harness.configure_chase(0.005, Vector2(0, 0));
    r_harness.set_body(Vector2(0, 0));
    for (int at = 0; at < 30; ++at) {
        r_harness.step_chase(double(at) / r_harness.fps);
    }
}

LawVerdict chase_is_smooth(const Bench &) {
    const Oracle TRUTHS[] = {linear_truth(), circle_truth()};
    for (const Oracle &truth : TRUTHS) {
        InterpHarness harness;
        harness.configure_chase(0.05, truth.value_at(0.0));
        harness.run_chase(truth, DURATION);
        const Metrics measured = analyze(harness, truth, 0.5);
        const double dt = harness.frame_dt();
        const double bound
            = truth.accel_max() * dt * dt + JERK_SLACK * truth.speed_max() * dt;
        if (!measured.playhead_monotonic || measured.regressions != 0) {
            return law_broken("the %s chase moved backward", truth.label);
        }
        if (measured.max_jerk > bound) {
            return law_broken(
                "the %s chase jerked %.3f against %.3f",
                truth.label,
                measured.max_jerk,
                bound
            );
        }
    }
    return law_held();
}

LawVerdict correction_glides(const Bench &) {
    InterpHarness harness;
    settle_chase(harness);
    harness.set_body(Vector2(1, 0));
    harness.absorb_recovery(Vector2(1, 0), false);
    harness.step_chase(0.6);
    if (harness.displayed.back().length() >= 0.3) {
        return law_broken("the first frame jumped to %f", double(
            harness.displayed.back().length()
        ));
    }
    for (int at = 0; at < 90; ++at) {
        harness.step_chase(0.7 + double(at) / harness.fps);
    }
    const double left = harness.displayed.back().distance_to(Vector2(1, 0));
    if (left > 0.02) {
        return law_broken("the display stopped %f short", left);
    }
    return law_held();
}

LawVerdict correction_train_glides(const Bench &) {
    InterpHarness harness;
    settle_chase(harness);
    Vector2 body;
    Vector2 last = harness.displayed.back();
    double worst_step = 0.0;
    for (int at = 0; at < 60; ++at) {
        if (at % 3 == 0) {
            body += Vector2(0.1, 0);
            harness.set_body(body);
            harness.absorb_recovery(Vector2(0.1, 0), false);
        }
        harness.step_chase(0.6 + double(at) / harness.fps);
        const Vector2 shown = harness.displayed.back();
        worst_step = std::max(worst_step, double(shown.distance_to(last)));
        last = shown;
    }
    if (worst_step >= 0.05) {
        return law_broken("one frame stepped %f", worst_step);
    }
    return law_held();
}

LawVerdict replay_is_identical(const Bench &) {
    const Oracle truth = circle_truth();
    InterpHarness first;
    first.configure(0.05, truth.value_at(0.0));
    first.run(truth, poor_3g_link(), DURATION, SEED);
    InterpHarness second;
    second.configure(0.05, truth.value_at(0.0));
    second.run(truth, poor_3g_link(), DURATION, SEED);
    if (first_difference(first.displayed, second.displayed) != -1
        || first_difference(first.written->samples, second.written->samples)
            != -1
        || !same_stats(first.run_stats, second.run_stats)) {
        return law_broken("the two runs differ");
    }
    return law_held();
}

ScheduleHarness scheduled(const std::vector<int> &p_order) {
    ScheduleHarness harness;
    harness.add_lane(LaneDecl{linear_truth(), wifi_link(), 11});
    harness.add_lane(LaneDecl{circle_truth(), poor_3g_link(), 22});
    harness.add_lane(LaneDecl{linear_truth(), mobile_4g_link(), 33});
    harness.run(DURATION, p_order);
    return harness;
}

LawVerdict pump_order_is_free(const Bench &) {
    ScheduleHarness forward = scheduled({0, 1, 2});
    ScheduleHarness permuted = scheduled({2, 0, 1});
    for (size_t at = 0; at < forward.lanes.size(); ++at) {
        if (first_difference(
                forward.lanes[at].displayed,
                permuted.lanes[at].displayed
            )
            != -1) {
            return law_broken("lane %d drew other frames", int(at));
        }
    }
    if (!same_stats(forward.run_stats, permuted.run_stats)) {
        return law_broken("the stats differ");
    }
    return law_held();
}

const Row ROWS[] = {
    {"a body that snaps once a tick, chased by the display",
     "the display never goes back and stays under its jerk bound",
     &chase_is_smooth},
    {"a one unit correction on a resting chased body",
     "the first frame barely moves and the display glides onto the new pose",
     &correction_glides},
    {"a correction every third frame for a second",
     "no frame steps further than 0.05",
     &correction_train_glides},
    {"one lossy run played twice",
     "both draw the same frames, write the same values and count the same",
     &replay_is_identical},
    {"three remote bodies pumped in two orders",
     "each body draws the same frames either way",
     &pump_order_is_free},
};

TEST_CASE(
    "[Netw][Display][Calculus] a chased body absorbs corrections "
    "without a jump, and a display draws the same frames however it is "
    "replayed or ordered"
) {
    const Bench bench = {String("an interpolation runtime with no session")};
    for (const Row &row : ROWS) {
        NETW_CELL(row, bench);
        NETW_LAW_HOLDS(row, bench);
    }
}

} // namespace TestInterpCalculus

#endif
