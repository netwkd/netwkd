#include "support/link.h"
#include "support/netw_test.h"

#if defined(NETW_TIER_HOSTED)

#include "support/fault_log.h"
#include "support/internals.h"
#include "support/loopback_rig.h"
#include "support/netw_cells.h"
#include "support/netw_recorder.h"

#include "netw/api/entity.hpp"
#include "netw/api/interest_layer.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/predict_stats.hpp"
#include "netw/api/prediction.hpp"
#include "netw/api/simulation.hpp"
#include "netw/predict/drive.hpp"

namespace TestPrediction {

using namespace godot;
using netw::NetwEntity;
using netw::NetwInterestLayer;
using netw::NetwMultiplayer;
using netw::NetwPrediction;
using netw::NetwSimulation;
using netw_test::EntityDecl;
using netw_test::FaultWatch;
using netw_test::law_broken;
using netw_test::law_held;
using netw_test::LawRowFor;
using netw_test::LawVerdict;
using netw_test::Internals;
using netw_test::LoopbackRig;
using netw_test::PredictionReports;
using netw_test::Recorder;
using netw_test::WorldDecl;

constexpr int SERVER = -1;
constexpr int SETTLE_TICKS = 30;
constexpr int RECOVER_TICKS = 70;
constexpr int PAST_HORIZON = netw::predict::ACK_AGE_MAX + 16;

struct World {
    String label;
};

typedef LawRowFor<World> Row;

EntityDecl runner(const char *p_name) {
    return EntityDecl()
        .named(p_name)
        .on_schema("RunnerPose")
        .synced("position")
        .placed_at(Vector2())
        .predicted(0.01);
}

Node2D *body(LoopbackRig &p_rig, const char *p_name, int p_at) {
    return Object::cast_to<Node2D>(
        p_rig.node_of(p_rig.entity_of(StringName(p_name), p_at), p_at)
    );
}

Ref<NetwPrediction> prediction(
    LoopbackRig &p_rig,
    const char *p_name,
    int p_at
) {
    return NetwEntity::of(body(p_rig, p_name, p_at))->get_prediction();
}

int ack_age(LoopbackRig &p_rig, const char *p_name, int p_at) {
    return Internals::predict_declaration(
               NetwEntity::of(body(p_rig, p_name, p_at)).ptr()
    )
        .ack_age_ticks;
}

void run(LoopbackRig &p_rig, const char *p_name, int p_at) {
    body(p_rig, p_name, p_at)->set("motion", Vector2(1.0, 0.0));
}

void delay_every_link(LoopbackRig &p_rig) {
    for (int at = SERVER; at < p_rig.count(); ++at) {
        p_rig.conditions(at, netw_test::polls(4));
    }
}

void shove(LoopbackRig &p_rig, const char *p_name) {
    Node2D *served = body(p_rig, p_name, SERVER);
    served->set_position(served->get_position() + Vector2(60.0, -40.0));
}

LawVerdict horizon_resumes(const World &) {
    LoopbackRig rig(1);
    rig.declare_world(
        WorldDecl().clocked(30, 3).lag_compensated().player(runner("P"), 0)
    );
    run(rig, "P", 0);
    rig.step_ticks(SETTLE_TICKS);

    Ref<netw::NetwLinkConditions> deaf = netw_test::link(1);
    deaf->set_loss_chance(1.0);
    rig.conditions(0, deaf);
    rig.step_ticks(PAST_HORIZON);
    const double stalled = body(rig, "P", 0)->get_position().x;
    const int age = ack_age(rig, "P", 0);
    rig.step_ticks(8);
    if (body(rig, "P", 0)->get_position().x != stalled) {
        return law_broken(
            "a client nobody answered kept predicting past x %g",
            stalled
        );
    }
    if (age != netw::predict::ACK_AGE_MAX) {
        return law_broken("the client stalled at an ack age of %d", age);
    }

    rig.conditions(0, Ref<netw::NetwLinkConditions>());
    rig.step_ticks(SETTLE_TICKS);
    if (body(rig, "P", 0)->get_position().x <= stalled) {
        return law_broken("the client never resumed from x %g", stalled);
    }
    if (ack_age(rig, "P", 0) >= netw::predict::ACK_AGE_MAX) {
        return law_broken("the server answered and the ack age stayed full");
    }
    return law_held();
}

LawVerdict signals_in_order(const World &) {
    LoopbackRig rig(1);
    rig.declare_world(
        WorldDecl().clocked(30, 3).lag_compensated().player(runner("P"), 0)
    );
    delay_every_link(rig);
    run(rig, "P", 0);
    rig.step_ticks(SETTLE_TICKS);

    Vector<StringName> signals;
    signals.push_back(StringName("divergence_detected"));
    signals.push_back(StringName("episode_changed"));
    signals.push_back(StringName("corrected"));
    Vector<StringName> published;
    published.push_back(signals[2]);
    const PredictionReports heard(prediction(rig, "P", 0).ptr(), signals);
    const Recorder corrected(prediction(rig, "P", 0).ptr(), published);
    shove(rig, "P");
    rig.step_ticks(RECOVER_TICKS);

    const Vector<StringName> order = heard.order();
    if (order.size() < 4 || order[0] != signals[0] || order[1] != signals[1]
        || order[2] != signals[2]) {
        return law_broken(
            "the client heard %d signal(s), not a divergence, an episode "
            "and a recovery in that order",
            int(order.size())
        );
    }
    const int64_t diverged = heard.args(signals[0])[0];
    if (corrected.count(signals[2]) < 1) {
        return law_broken("the corrected signal never reached a listener");
    }
    const int64_t moved = corrected.args(signals[2])[0];
    if (diverged != moved) {
        return law_broken(
            "the divergence names tick %d and the recovery tick %d",
            int(diverged),
            int(moved)
        );
    }
    const int changes = heard.count(signals[1]);
    if (int(heard.args(signals[1], 0)[0])
            != NetwPrediction::EPISODE_CHANGE_OPENED
        || int(heard.args(signals[1], changes - 1)[0])
            != NetwPrediction::EPISODE_CHANGE_CLOSED) {
        return law_broken("the episode did not open first and close last");
    }
    if (corrected.count(signals[2]) != 1) {
        return law_broken(
            "one shove moved the client %d time(s)",
            corrected.count(signals[2])
        );
    }
    return law_held();
}

LawVerdict refused_joint_once(bool p_spawned) {
    EntityDecl p = runner("P");
    EntityDecl q = runner("Q");
    if (p_spawned) {
        p.spawned();
        q.spawned();
    }
    LoopbackRig rig(2);
    rig.declare_world(
        WorldDecl().clocked(30, 3).lag_compensated().player(p, 0).player(q, 1)
    );
    run(rig, "P", 0);
    run(rig, "Q", 1);
    rig.step_ticks(SETTLE_TICKS);

    const FaultWatch faults;
    const char *const copies[][2] = {
        {"P", "-1"},
        {"Q", "-1"},
        {"P", "0"},
        {"Q", "1"},
    };
    for (const auto &copy : copies) {
        const int at = String(copy[1]).to_int();
        const Ref<NetwEntity> entity = NetwEntity::of(body(rig, copy[0], at));
        entity->get_prediction()->set_rollback_together(true);
        entity->get_simulation()->set_schedule(
            NetwSimulation::SCHEDULE_STEPPED
        );
    }
    rig.step_ticks(SETTLE_TICKS);

    for (const char *name : {"P", "Q"}) {
        const int64_t named = faults.warnings_containing(
            String("'") + String(name) + String("'")
        );
        if (named != 1) {
            return law_broken(
                "the refused JOINT of %s was reported %d time(s)",
                name,
                int(named)
            );
        }
    }
    if (faults.warnings_containing("[NW1033]") != 2) {
        return law_broken(
            "%d NW1033 warning(s) for two refused groups",
            int(faults.warnings_containing("[NW1033]"))
        );
    }
    return law_held();
}

LawVerdict refused_joint_placed(const World &) {
    return refused_joint_once(false);
}

LawVerdict refused_joint_spawned(const World &) {
    return refused_joint_once(true);
}

template <typename Tick>
double sent_from(
    LoopbackRig &p_rig,
    NetwMultiplayer *p_sender,
    int64_t p_to,
    const char *p_cause,
    int p_ticks,
    Tick p_before_each
) {
    constexpr double WINDOW = 3600.0;
    Internals::traffic_window(p_sender, WINDOW);
    Internals::traffic_advance(p_sender, WINDOW);
    for (int tick = 0; tick < p_ticks; ++tick) {
        p_before_each(tick);
        p_rig.step_ticks(1);
    }
    Internals::traffic_advance(p_sender, WINDOW);
    const Dictionary left = p_sender->session_get_traffic(p_to);
    const Dictionary sent = left[StringName("sent")];
    const double rate = sent[StringName(p_cause)];
    return rate * WINDOW;
}

template <typename Tick>
double sent_bytes(
    LoopbackRig &p_rig,
    int p_client,
    const char *p_cause,
    int p_ticks,
    Tick p_before_each
) {
    return sent_from(
        p_rig,
        p_rig.server(),
        p_rig.peer_id(p_client),
        p_cause,
        p_ticks,
        p_before_each
    );
}

double acked_bytes(LoopbackRig &p_rig, bool p_shoved) {
    return sent_bytes(p_rig, 0, "upkeep", 64, [&](int p_tick) {
        if (p_shoved && p_tick % 8 == 0) {
            shove(p_rig, "P");
        }
    });
}

LawVerdict answers_cost(bool p_physics_driven, double &r_agreed) {
    LoopbackRig rig(1);
    rig.declare_world(
        WorldDecl().clocked(30, 3).lag_compensated().player(runner("P"), 0)
    );
    for (const int at : {SERVER, 0}) {
        NetwEntity::of(body(rig, "P", at))
            ->get_simulation()
            ->set_physics_driven(p_physics_driven);
    }
    run(rig, "P", 0);
    rig.step_ticks(SETTLE_TICKS);
    r_agreed = acked_bytes(rig, false);
    const double shoved = acked_bytes(rig, true);
    if (!p_physics_driven && shoved < r_agreed * 1.5) {
        return law_broken(
            "the server answered in %d bytes while it agreed and %d while it "
            "corrected the client",
            int(r_agreed),
            int(shoved)
        );
    }
    return law_held();
}

LawVerdict answers_grow_when_asked(const World &) {
    double plain = 0.0;
    const LawVerdict asked = answers_cost(false, plain);
    if (!asked.held) {
        return asked;
    }
    double solved = 0.0;
    answers_cost(true, solved);
    if (solved < plain * 1.5) {
        return law_broken(
            "the server answered a body it agrees with in %d bytes and a "
            "physics body it agrees with in %d",
            int(plain),
            int(solved)
        );
    }
    return law_held();
}

constexpr int WATCHER = 1;
constexpr int HELD_TICKS = 20;
constexpr int SEEN_TICKS = 8;

void watch_runner(LoopbackRig &p_rig) {
    NetwEntity::of(body(p_rig, "P", WATCHER))
        ->get_simulation()
        ->set_remote_mode(NetwSimulation::REMOTE_MODE_ACTIVE);
}

Vector2 watched_motion(LoopbackRig &p_rig) {
    return body(p_rig, "P", WATCHER)->get("motion");
}

Ref<NetwInterestLayer> two_runners(
    LoopbackRig &p_rig,
    int p_rate = 30,
    int p_send_rate = 0
) {
    Vector<StringName> watched;
    watched.push_back(StringName("P"));
    p_rig.declare_world(
        WorldDecl()
            .clocked(p_rate, 3)
            .sending(p_send_rate)
            .lag_compensated()
            .player(runner("P"), 0)
            .player(runner("Q"), WATCHER)
            .island(StringName("Q"), watched, WorldDecl::INDEPENDENT)
    );
    const Ref<NetwInterestLayer> sight
        = p_rig.server()->interest_layer(StringName("sight"));
    sight->add_entity(NetwEntity::of(body(p_rig, "P", SERVER)));
    sight->add_viewer(p_rig.peer_id(0));
    sight->add_viewer(p_rig.peer_id(WATCHER));
    p_rig.flush_interest();
    p_rig.step_ticks(SETTLE_TICKS);
    return sight;
}

LawVerdict relays_a_change(const World &) {
    constexpr int WINDOW_TICKS = 60;
    constexpr int REPEATED_TICKS = 8;
    constexpr int BACKOFF_TICKS = 16;
    LoopbackRig rig(2);
    constexpr int PRESSED_TICKS = 4;
    two_runners(rig);
    watch_runner(rig);
    rig.step_ticks(SEEN_TICKS);
    body(rig, "P", 0)->set("bombing", true);
    rig.step_ticks(PRESSED_TICKS);
    if (!bool(body(rig, "P", WATCHER)->get("bombing"))) {
        return law_broken(
            "a button pressed by a standing runner had not reached the "
            "watching client %d ticks later",
            PRESSED_TICKS
        );
    }
    run(rig, "P", 0);
    rig.step_ticks(SEEN_TICKS);
    if (watched_motion(rig) != Vector2(1.0, 0.0)) {
        return law_broken("the watching client never saw the runner's input");
    }

    const auto turn = [&](int p_tick) {
        const double heading = p_tick % 2 == 0 ? -1.0 : 1.0;
        body(rig, "P", 0)->set("motion", Vector2(heading, 0.0));
    };
    const auto hold = [](int) {};
    const double turning
        = sent_bytes(rig, WATCHER, "input", WINDOW_TICKS, turn);
    rig.step_ticks(REPEATED_TICKS);
    if (sent_bytes(rig, WATCHER, "input", BACKOFF_TICKS, hold) <= 0.0) {
        return law_broken(
            "a changed input was not sent again between its first repeats "
            "and its refresh"
        );
    }
    const double held = sent_bytes(rig, WATCHER, "input", WINDOW_TICKS, hold);
    if (held <= 0.0) {
        return law_broken("an input held for two seconds was never refreshed");
    }
    if (held * 4.0 > turning) {
        return law_broken(
            "a held input cost %d bytes and a turning one %d",
            int(held),
            int(turning)
        );
    }
    return law_held();
}

LawVerdict relays_in_four_sends(const World &) {
    constexpr int TICKS = 30;
    constexpr int SENDS = 2;
    constexpr int INTERVAL = TICKS / SENDS;
    constexpr int COPIES = 4;
    LoopbackRig rig(2);
    two_runners(rig, TICKS, SENDS);
    watch_runner(rig);
    rig.step_ticks(SETTLE_TICKS);
    run(rig, "P", 0);
    for (int tick = 0; tick < TICKS * 4; ++tick) {
        if (watched_motion(rig) == Vector2(1.0, 0.0)) {
            break;
        }
        rig.step_ticks(1);
    }
    if (watched_motion(rig) != Vector2(1.0, 0.0)) {
        return law_broken("the watching client never saw the runner's input");
    }
    const auto hold = [](int) {};
    for (int copy = 1; copy < COPIES; ++copy) {
        if (sent_bytes(rig, WATCHER, "input", INTERVAL, hold) <= 0.0) {
            return law_broken(
                "send %d after the change carried no copy of it",
                copy + 1
            );
        }
    }
    return law_held();
}

LawVerdict idles_in_step(const World &) {
    constexpr int IDLE_RATE = 15;
    constexpr int SECONDS = 6;
    constexpr int PING_AFTER_RENEWAL_LEFT = 3;
    constexpr double WINDOW = 3600.0;
    LoopbackRig rig(2);
    two_runners(rig, IDLE_RATE);
    watch_runner(rig);
    rig.step_ticks(4 * IDLE_RATE);
    NetwMultiplayer *client = rig.client(WATCHER);
    NetwMultiplayer *server = rig.server();
    const auto hold = [](int) {};
    for (int tick = 0; tick < 2 * IDLE_RATE; ++tick) {
        if (sent_from(rig, client, 1, "input", 1, hold) > 0.0) {
            break;
        }
    }
    rig.step_ticks(PING_AFTER_RENEWAL_LEFT);
    for (NetwMultiplayer *side : {client, server}) {
        Internals::traffic_window(side, WINDOW);
        Internals::traffic_advance(side, WINDOW);
    }
    for (int tick = 0; tick < SECONDS * IDLE_RATE; ++tick) {
        if (tick % IDLE_RATE == 0) {
            client->clock_owe_ping(false);
        }
        rig.step_ticks(1);
    }
    for (NetwMultiplayer *side : {client, server}) {
        Internals::traffic_advance(side, WINDOW);
    }
    const StringName packets("sent_packets");
    const double up = double(client->session_get_traffic(1)[packets]) * WINDOW;
    const double down
        = double(server->session_get_traffic(rig.peer_id(WATCHER))[packets])
        * WINDOW;
    if (up > 2 * SECONDS + 1) {
        return law_broken(
            "a standing client sent %d packets in %d seconds",
            int(up),
            SECONDS
        );
    }
    if (down > SECONDS + 1) {
        return law_broken(
            "the server sent a standing client %d packets in %d seconds",
            int(down),
            SECONDS
        );
    }
    return law_held();
}

LawVerdict runs_one_input_a_tick(const World &) {
    constexpr int TICKS = 30;
    constexpr int SENDS = 10;
    LoopbackRig rig(1);
    rig.declare_world(
        WorldDecl()
            .clocked(TICKS, 3)
            .sending(SENDS)
            .lag_compensated()
            .player(runner("P"), 0)
    );
    run(rig, "P", 0);
    rig.step_ticks(SETTLE_TICKS);
    Node2D *served = body(rig, "P", SERVER);
    double stride = 0.0;
    for (int tick = 0; tick < TICKS; ++tick) {
        const double before = served->get_position().x;
        rig.step_ticks(1);
        const double moved = served->get_position().x - before;
        if (tick == 0) {
            stride = moved;
        }
        if (moved <= 0.0 || std::abs(moved - stride) > 1e-6) {
            return law_broken(
                "the server moved a running client %g and then %g in one tick",
                stride,
                moved
            );
        }
    }
    return law_held();
}

LawVerdict rests_at_a_wide_interval(const World &) {
    constexpr int TICKS = 30;
    constexpr int SENDS = 2;
    constexpr int WINDOW_TICKS = TICKS * 5;
    LoopbackRig rig(1);
    rig.declare_world(
        WorldDecl()
            .clocked(TICKS, 3)
            .sending(SENDS)
            .lag_compensated()
            .player(runner("P"), 0)
    );
    delay_every_link(rig);
    Node2D *predicted = body(rig, "P", 0);
    run(rig, "P", 0);
    rig.step_ticks(SETTLE_TICKS * 2);
    bool halted = false;
    double last = predicted->get_position().x - 1.0;
    const double running = sent_from(
        rig,
        rig.client(0),
        1,
        "input",
        WINDOW_TICKS,
        [&](int) {
            halted = halted || predicted->get_position().x == last;
            last = predicted->get_position().x;
        }
    );
    if (halted) {
        return law_broken("a running client stopped predicting");
    }
    predicted->set("motion", Vector2());
    rig.step_ticks(WINDOW_TICKS);
    const auto hold = [](int) {};
    const double resting
        = sent_from(rig, rig.client(0), 1, "input", WINDOW_TICKS, hold);
    if (resting * 3.0 > running) {
        return law_broken(
            "a standing client sent %d input bytes and a running one %d",
            int(resting),
            int(running)
        );
    }
    const double stood = predicted->get_position().x;
    run(rig, "P", 0);
    rig.step_ticks(2);
    if (predicted->get_position().x == stood) {
        return law_broken("a client that stood still did not move again");
    }
    return law_held();
}

LawVerdict seeds_a_late_watcher(const World &) {
    LoopbackRig rig(2);
    const Ref<NetwInterestLayer> sight = two_runners(rig);
    run(rig, "P", 0);
    rig.step_ticks(HELD_TICKS);
    watch_runner(rig);
    rig.step_ticks(SEEN_TICKS);
    if (watched_motion(rig) != Vector2(1.0, 0.0)) {
        return law_broken(
            "a client that began watching a runner whose input was held had "
            "not seen that input %d ticks later",
            SEEN_TICKS
        );
    }

    sight->remove_viewer(rig.peer_id(WATCHER));
    rig.flush_interest();
    body(rig, "P", 0)->set("motion", Vector2(-1.0, 0.0));
    rig.step_ticks(HELD_TICKS);
    sight->add_viewer(rig.peer_id(WATCHER));
    rig.flush_interest();
    rig.step_ticks(SEEN_TICKS);
    if (watched_motion(rig) != Vector2(-1.0, 0.0)) {
        return law_broken(
            "a client that saw a runner again had not seen its held input "
            "%d ticks later",
            SEEN_TICKS
        );
    }
    return law_held();
}

LawVerdict answers_a_roster_change(const World &) {
    constexpr int JOINED = 8;
    double answered[2] = {0.0, 0.0};
    for (const int joins : {0, 1}) {
        LoopbackRig rig(2);
        two_runners(rig);
        answered[joins]
            = sent_bytes(rig, WATCHER, "upkeep", 32, [&](int p_tick) {
                  if (joins == 1 && p_tick == JOINED) {
                      NetwEntity::of(body(rig, "P", SERVER))
                          ->get_simulation()
                          ->set_remote_mode(NetwSimulation::REMOTE_MODE_ACTIVE);
                  }
              });
    }
    if (answered[1] <= answered[0]) {
        return law_broken(
            "the server answered in %d bytes on a quiet run and %d on a run "
            "where it changed who the runner is simulated with",
            int(answered[0]),
            int(answered[1])
        );
    }
    return law_held();
}

constexpr int RATE = 30;

int effects(LoopbackRig &p_rig, int p_at) {
    return Object::cast_to<netw_test::Carrier>(body(p_rig, "P", p_at))
        ->fresh_effects();
}

void press(LoopbackRig &p_rig, bool p_pressed) {
    body(p_rig, "P", 0)->set("bombing", p_pressed);
}

void stand(LoopbackRig &p_rig, bool p_pressed) {
    p_rig.declare_world(
        WorldDecl().clocked(RATE, 3).lag_compensated().player(runner("P"), 0)
    );
    press(p_rig, p_pressed);
    p_rig.step_ticks(SETTLE_TICKS);
}

void stand_holding(LoopbackRig &p_rig) {
    stand(p_rig, true);
}

void go_dark(LoopbackRig &p_rig, int p_ticks) {
    Ref<netw::NetwLinkConditions> deaf = netw_test::link(1);
    deaf->set_loss_chance(1.0);
    p_rig.conditions(SERVER, deaf);
    p_rig.step_ticks(p_ticks);
    p_rig.conditions(SERVER, Ref<netw::NetwLinkConditions>());
}

int ticks_to_serve(LoopbackRig &p_rig) {
    constexpr int LIMIT = 12;
    press(p_rig, true);
    for (int tick = 1; tick < LIMIT; ++tick) {
        p_rig.step_ticks(1);
        if (bool(body(p_rig, "P", SERVER)->get("bombing"))) {
            return tick;
        }
    }
    return LIMIT;
}

LawVerdict wakes_without_loss(const World &) {
    LoopbackRig dark(1);
    stand(dark, false);
    press(dark, true);
    go_dark(dark, 1);
    press(dark, false);
    go_dark(dark, SETTLE_TICKS);
    dark.step_ticks(SETTLE_TICKS);
    const int64_t told
        = Internals::predict_declaration(
              NetwEntity::of(body(dark, "P", 0)).ptr()
        )
              .facts.get_int_fact(netw::NetwPredictStats::FACT_SUBSTITUTED);
    if (effects(dark, SERVER) == 0 && told == 0) {
        return law_broken(
            "the server never ran a tap whose every packet was lost, and "
            "never told the client"
        );
    }

    LoopbackRig quiet(1);
    stand(quiet, false);
    const int woken = ticks_to_serve(quiet);
    LoopbackRig talking(1);
    talking.declare_world(
        WorldDecl().clocked(RATE, 3).lag_compensated().player(runner("P"), 0)
    );
    run(talking, "P", 0);
    talking.step_ticks(SETTLE_TICKS);
    const int talked = ticks_to_serve(talking);
    if (woken > talked) {
        return law_broken(
            "a button pressed after standing still reached the server in %d "
            "ticks, and one pressed while running in %d",
            woken,
            talked
        );
    }
    return law_held();
}

LawVerdict holds_a_standing_input(const World &) {
    constexpr int WINDOW_TICKS = 5 * RATE;
    LoopbackRig rig(1);
    stand_holding(rig);
    const auto hold = [](int) {};
    const auto tap = [&](int p_tick) { press(rig, p_tick % 2 == 0); };

    int ran = effects(rig, SERVER);
    int idle = 0;
    const auto count = [&](int) {
        idle += effects(rig, SERVER) == ran ? 1 : 0;
        ran = effects(rig, SERVER);
    };
    const double held
        = sent_from(rig, rig.client(0), 1, "input", WINDOW_TICKS, count);
    if (idle > 2) {
        return law_broken(
            "the server did not run a held button on %d of %d ticks",
            idle,
            WINDOW_TICKS
        );
    }
    const double answered = sent_bytes(rig, 0, "upkeep", WINDOW_TICKS, hold);
    const double tapped
        = sent_from(rig, rig.client(0), 1, "input", WINDOW_TICKS, tap);
    const double answered_taps
        = sent_bytes(rig, 0, "upkeep", WINDOW_TICKS, tap);
    if (held * 4.0 > tapped) {
        return law_broken(
            "a held button cost the client %d bytes and a tapped one %d",
            int(held),
            int(tapped)
        );
    }
    if (answered * 2.0 > answered_taps) {
        return law_broken(
            "the server answered a held button in %d bytes and a tapped one "
            "in %d",
            int(answered),
            int(answered_taps)
        );
    }
    return law_held();
}

LawVerdict drops_a_silent_hold(const World &) {
    LoopbackRig rig(1);
    stand_holding(rig);
    Ref<netw::NetwLinkConditions> deaf = netw_test::link(1);
    deaf->set_loss_chance(1.0);
    rig.conditions(SERVER, deaf);
    const int ran = effects(rig, SERVER);
    rig.step_ticks(5 * RATE);
    const int held = effects(rig, SERVER) - ran;
    if (held > 3 * RATE + 8) {
        return law_broken(
            "the server ran a button for %d ticks after its client fell "
            "silent, at %d ticks a second",
            held,
            RATE
        );
    }
    return law_held();
}

LawVerdict rests_inside_its_threshold(const World &) {
    constexpr int WINDOW_TICKS = 5 * RATE;
    constexpr double DRIFT = 0.005;
    const auto hold = [](int) {};
    LoopbackRig rig(1);
    stand(rig, false);
    const double agreed
        = sent_from(rig, rig.client(0), 1, "input", WINDOW_TICKS, hold);

    Node2D *served = body(rig, "P", SERVER);
    served->set_position(served->get_position() + Vector2(DRIFT, 0.0));
    rig.step_ticks(SETTLE_TICKS);
    const double drifted
        = sent_from(rig, rig.client(0), 1, "input", WINDOW_TICKS, hold);
    if (body(rig, "P", 0)->get_position() == served->get_position()) {
        return law_broken(
            "a client %g from the server's copy was corrected under a "
            "threshold of 0.01",
            DRIFT
        );
    }
    if (drifted > agreed * 1.5) {
        return law_broken(
            "a standing client sent %d bytes while it agreed with the server "
            "and %d while it stood %g away, under its threshold",
            int(agreed),
            int(drifted),
            DRIFT
        );
    }
    return law_held();
}

const Row ROWS[] = {
    {"a client standing closer to the server's copy than its threshold",
     "it is not corrected, and it sends no more than one standing on the "
     "server's copy",
     &rests_inside_its_threshold},
    {"a client holding a button while it stands still",
     "the server runs the button every tick, the client sends under a "
     "quarter of what tapping it costs, and the server answers in under "
     "half",
     &holds_a_standing_input},
    {"a client that changes a button after standing still",
     "the client is told when every packet of the change is lost, and on a "
     "clean link the change is run as soon as a running client's",
     &wakes_without_loss},
    {"a client that falls silent while it holds a button",
     "the server stops running the button within three seconds",
     &drops_a_silent_hold},
    {"a client whose server stops answering",
     "it stops predicting once the server has been silent for ACK_AGE_MAX "
     "ticks, and resumes once the server answers again",
     &horizon_resumes},
    {"a client the server shoves once",
     "it reports a divergence, an episode opening, a correction for the "
     "same tick, and the episode closing, and hears corrected once",
     &signals_in_order},
    {"a client the server agrees with, then one it keeps correcting",
     "the server's answers grow while it corrects the client, and a physics "
     "body is answered in full even while the server agrees with it",
     &answers_grow_when_asked},
    {"a server that changes who a runner is simulated with",
     "it answers that runner's owner in more bytes than on a quiet run",
     &answers_a_roster_change},
    {"a client simulating another player's runner",
     "it sees a standing runner's button within four ticks, sees that "
     "runner's input change, a held input is sent again "
     "before its refresh, costs under a quarter of a changing one, and is "
     "still refreshed",
     &relays_a_change},
    {"a client simulating another player's runner while the session sends "
     "on every fifteenth tick",
     "a changed input reaches it in four sends in a row",
     &relays_in_four_sends},
    {"a client that starts simulating a runner whose input is held",
     "it sees that input within ten ticks",
     &seeds_a_late_watcher},
    {"two clients standing still while one pings the server every second",
     "its ping leaves in the packet that renews its held input, and the "
     "server's answer carries the pong and the other runner's refresh, one "
     "packet a second",
     &idles_in_step},
    {"a client on a slow link while the session sends on every fifteenth "
     "tick",
     "it predicts every tick while running, sends under a third as much "
     "standing still, and moves again within two ticks",
     &rests_at_a_wide_interval},
    {"a client running while the session sends on every third tick",
     "the server moves it the same distance every tick",
     &runs_one_input_a_tick},
    {"rollback_together on placed runners whose schedule cannot replay",
     "each is reported once with NW1033, by the peer that predicts it",
     &refused_joint_placed},
    {"rollback_together on spawned runners whose schedule cannot replay",
     "each is reported once with NW1033, by the peer that predicts it",
     &refused_joint_spawned},
};

TEST_CASE(
    "[Netw][Predict][SceneTree] a predicting client stops at its "
    "horizon and resumes, hears its correction in order, and a joint it "
    "cannot replay is reported once"
) {
    const World world = {String("a server and clients on a loopback")};
    for (const Row &row : ROWS) {
        NETW_CELL(row, world);
        NETW_LAW_HOLDS(row, world);
    }
}

} // namespace TestPrediction

#endif
