#include "support/link.h"
#include "support/netw_test.h"

#include "support/minted_script.h"

#include "support/declared_nodes.h"

#if defined(NETW_TIER_HOSTED)

#include "support/fault_log.h"
#include "support/netw_cells.h"
#include "support/value_flow_stand.h"

#include "netw/api/entity.hpp"
#include "netw/api/participant.hpp"

namespace TestValueFlow {

using namespace godot;
using netw_test::FlowPair;
using netw_test::law_broken;
using netw_test::law_held;
using netw_test::LawRowFor;
using netw_test::LawVerdict;
using netw_test::LoopbackRig;

constexpr int TICKRATE = 30;
constexpr double POLL_PERIOD_MS = 1000.0 / 60.0;
constexpr int PHASE = 20;
constexpr int CLEAN_TICKS = 40;
constexpr int LOSSY_TICKS = 80;
constexpr int HEAL_TICKS = 6;
constexpr int QUIET_TICKS = 6;
constexpr int BEAT_TICKS = 10;
constexpr int NOBODY = -2;
constexpr int SERVER = -1;

enum Flow {
    STATE_AND_INPUT,
    RETAINED,
    BROADCAST,
    HEARTBEAT,
};

struct FlowScenario {
    String label;
    Flow flow = STATE_AND_INPUT;
    const char *body = netw_test::gdsrc::STATE_AND_INPUT;
    int clients = 1;
    int controller = 0;
    int author = SERVER;
    int holder = NOBODY;
    bool late_holder = false;
    double delay_ticks = 0.0;
    int64_t gap_bound = 0;
    bool lossy = false;
    bool dark = false;
    bool flips = false;
    bool keeps_authoring = false;
    const char *breaks = nullptr;
};

Vector2 authored(int64_t p_step) {
    return Vector2(double(p_step), -double(p_step));
}

struct Evidence {
    double authority_state = 0.0;
    double mirror_state = 0.0;
    double observer_state = 0.0;
    double authority_input = 0.0;
    double controller_input = 0.0;
    double observer_input = 0.0;
    int64_t widest_gap = 0;
    int64_t retained_settled = 0;
    int64_t retained_quiet = 0;
    int64_t retained_flipped = 0;
    bool mirror_holds_flip = false;
    Vector2 last_authored;
    Vector<Vector2> reached;
    Vector<Vector2> healed;
    int64_t rows_before_quiet = 0;
    int64_t rows_after_quiet = 0;
    int64_t beats = 0;
    Vector2 beaten;
};

constexpr netw::NetwMultiplayer::Stat RETAINED_OUT
    = netw::NetwMultiplayer::STAT_RETAINED_FRAMES_OUT;
constexpr netw::NetwMultiplayer::Stat ROWS_OUT
    = netw::NetwMultiplayer::STAT_ROW_FRAMES_OUT;

int64_t stat_of(Object *p_core, netw::NetwMultiplayer::Stat p_stat) {
    return netw_test::flow_core(p_core)->stats_get(p_stat);
}

class FlowRun {
    FlowScenario declared;
    Evidence seen;
    int64_t step = 0;

    Node2D *copy(const FlowPair &p_pair, int p_side) const {
        return p_side < 0 ? p_pair.authored : p_pair.mirror(p_side);
    }

    Object *core(LoopbackRig &p_rig, int p_side) const {
        return p_side < 0 ? p_rig.server() : p_rig.client(p_side);
    }

    void steer(LoopbackRig &p_rig, const FlowPair &p_pair) {
        if (declared.controller == NOBODY) {
            return;
        }
        const Ref<netw::NetwPlayer> player = p_rig.player(declared.controller);
        REQUIRE(player.is_valid());
        netw::NetwEntity::of(p_pair.authored)
            ->set(StringName("controller"), player);
        p_rig.pump(4);
    }

    void hand_authority(LoopbackRig &p_rig, const FlowPair &p_pair) {
        const int holder = p_rig.peer_id(declared.holder);
        p_pair.authored->set_multiplayer_authority(holder);
        for (int side = 0; side < declared.clients; ++side) {
            p_pair.mirror(side)->set_multiplayer_authority(holder);
        }
    }

    void drive_pair(LoopbackRig &p_rig, const FlowPair &p_pair) {
        step += 1;
        p_pair.authored->set_position(authored(step));
        p_pair.mirror(0)->set_rotation(double(step) * 0.01);
        if (declared.flips) {
            p_pair.authored->set(StringName("stunned"), (step % 2) == 0);
        }
        p_rig.step_ticks(1);
    }

    void author(LoopbackRig &p_rig, const FlowPair &p_pair, int p_ticks) {
        for (int tick = 0; tick < p_ticks; ++tick) {
            step += 1;
            seen.last_authored = authored(step);
            copy(p_pair, declared.author)
                ->set(StringName("aim_dir"), seen.last_authored);
            p_rig.step_ticks(1);
        }
    }

    Vector<Vector2> others(const FlowPair &p_pair) const {
        Vector<Vector2> out;
        for (int side = SERVER; side < declared.clients; ++side) {
            if (side != declared.author) {
                out.push_back(copy(p_pair, side)->get(StringName("aim_dir")));
            }
        }
        return out;
    }

    void impair(LoopbackRig &p_rig, bool p_on) {
        const int from = p_rig.peer_id(declared.author);
        for (int side = SERVER; side < declared.clients; ++side) {
            if (side == declared.author) {
                continue;
            }
            Ref<netw::NetwLinkConditions> conditions = netw_test::link();
            if (p_on && declared.dark) {
                conditions->set_loss_chance(1.0);
            } else if (p_on) {
                conditions = netw_test::impairment(53, POLL_PERIOD_MS);
            }
            p_rig.conditions(side, conditions, from);
        }
    }

    void run_pair(LoopbackRig &p_rig, const FlowPair &p_pair) {
        if (declared.delay_ticks > 0.0) {
            const Ref<netw::NetwLinkConditions> delayed = netw_test::link(1);
            delayed->set_latency_ms(
                declared.delay_ticks * 1000.0 / double(TICKRATE)
            );
            p_rig.conditions(0, delayed, 1);
        }
        for (int tick = 0; tick < 60; ++tick) {
            drive_pair(p_rig, p_pair);
            if (tick > 15) {
                const int64_t gap = int64_t(
                    p_pair.authored->get_position().x
                    - p_pair.mirror(0)->get_position().x
                );
                seen.widest_gap = MAX(seen.widest_gap, gap);
            }
        }
        seen.authority_input = p_pair.authored->get_rotation();
        seen.controller_input = p_pair.mirror(0)->get_rotation();
        if (declared.clients > 1) {
            seen.observer_state = p_pair.mirror(1)->get_position().x;
            seen.observer_input = p_pair.mirror(1)->get_rotation();
        }
    }

    void run_retained(LoopbackRig &p_rig, const FlowPair &p_pair) {
        for (int tick = 0; tick < PHASE; ++tick) {
            drive_pair(p_rig, p_pair);
        }
        seen.retained_settled = stat_of(p_rig.server(), RETAINED_OUT);
        for (int tick = 0; tick < PHASE; ++tick) {
            drive_pair(p_rig, p_pair);
        }
        seen.retained_quiet = stat_of(p_rig.server(), RETAINED_OUT);
        p_pair.authored->set(StringName("stunned"), true);
        p_rig.step_ticks(BEAT_TICKS);
        seen.retained_flipped = stat_of(p_rig.server(), RETAINED_OUT);
        seen.mirror_holds_flip
            = bool(p_pair.mirror(0)->get(StringName("stunned")));
    }

    void run_broadcast(LoopbackRig &p_rig, const FlowPair &p_pair) {
        if (declared.late_holder) {
            p_rig.step_ticks(CLEAN_TICKS);
            hand_authority(p_rig, p_pair);
            p_rig.step_ticks(2);
        }
        Object *author_core = core(p_rig, declared.author);
        author(p_rig, p_pair, CLEAN_TICKS);
        seen.reached = others(p_pair);
        if (declared.lossy) {
            impair(p_rig, true);
            author(p_rig, p_pair, LOSSY_TICKS);
            if (!declared.dark) {
                impair(p_rig, false);
            }
        }
        p_rig.step_ticks(HEAL_TICKS);
        seen.healed = others(p_pair);
        seen.rows_before_quiet = stat_of(author_core, ROWS_OUT);
        if (declared.keeps_authoring) {
            author(p_rig, p_pair, QUIET_TICKS);
            seen.last_authored = authored(step - QUIET_TICKS);
        } else {
            p_rig.step_ticks(QUIET_TICKS);
        }
        seen.rows_after_quiet = stat_of(author_core, ROWS_OUT);
    }

    void run_heartbeat(LoopbackRig &p_rig, const FlowPair &p_pair) {
        author(p_rig, p_pair, 1);
        p_rig.step_ticks(BEAT_TICKS * 2);
        p_pair.mirror(0)->set(StringName("aim_dir"), Vector2(-7.0, -7.0));
        const int64_t before = stat_of(p_rig.server(), ROWS_OUT);
        p_rig.step_ticks(BEAT_TICKS * 2 + HEAL_TICKS);
        seen.beats = stat_of(p_rig.server(), ROWS_OUT) - before;
        seen.beaten = p_pair.mirror(0)->get(StringName("aim_dir"));
    }

public:
    explicit FlowRun(const FlowScenario &p_scenario) : declared(p_scenario) {
        netw_test::FaultWatch faults;
        if (p_scenario.body == netw_test::gdsrc::STATE_AND_INPUT) {
            faults.expect_warning("*[NW1038]*", p_scenario.clients + 1);
        }
        LoopbackRig rig(p_scenario.clients);
        rig.mount();
        netw_test::flow_clocks(rig, TICKRATE);
        const FlowPair pair
            = netw_test::stand_flow_pair(rig, p_scenario.body, "FlowBody");
        steer(rig, pair);
        if (p_scenario.holder != NOBODY && !p_scenario.late_holder) {
            hand_authority(rig, pair);
        }
        switch (p_scenario.flow) {
            case STATE_AND_INPUT:
                run_pair(rig, pair);
                break;
            case RETAINED:
                run_retained(rig, pair);
                break;
            case BROADCAST:
                run_broadcast(rig, pair);
                break;
            case HEARTBEAT:
                run_heartbeat(rig, pair);
                break;
        }
        seen.authority_state = pair.authored->get_position().x;
        seen.mirror_state = pair.mirror(0)->get_position().x;
    }

    const FlowScenario &scenario() const {
        return declared;
    }

    const Evidence &evidence() const {
        return seen;
    }
};

typedef LawRowFor<FlowRun> FlowLaw;

bool flows(const FlowRun &p_run, Flow p_flow) {
    return p_run.scenario().flow == p_flow;
}

LawVerdict law_state(const FlowRun &p_run) {
    const Evidence &seen = p_run.evidence();
    if (!flows(p_run, STATE_AND_INPUT)) {
        return law_held();
    }
    if (seen.mirror_state <= 0.0) {
        return law_broken("the mirror never received the state");
    }
    if (seen.mirror_state > seen.authority_state) {
        return law_broken(
            "the mirror reads %d against the server's %d",
            int(seen.mirror_state),
            int(seen.authority_state)
        );
    }
    if (seen.widest_gap > p_run.scenario().gap_bound) {
        return law_broken(
            "the mirror trailed by %d ticks, past the link's %d",
            int(seen.widest_gap),
            int(p_run.scenario().gap_bound)
        );
    }
    return law_held();
}

const FlowLaw L_STATE = {
    "state-reaches",
    "the server's state reaches every client, which never leads it and "
    "trails it by no more than the link's delay",
    &law_state,
};

LawVerdict law_input(const FlowRun &p_run) {
    const Evidence &seen = p_run.evidence();
    if (!flows(p_run, STATE_AND_INPUT)) {
        return law_held();
    }
    if (seen.authority_input <= 0.0) {
        return law_broken("the server never read the controller's input");
    }
    if (seen.authority_input > seen.controller_input) {
        return law_broken(
            "the server reads %d against the controller's %d",
            int(seen.authority_input * 1000.0),
            int(seen.controller_input * 1000.0)
        );
    }
    return law_held();
}

const FlowLaw L_INPUT = {
    "input-reaches",
    "the controlling client's input reaches the server, which never leads it",
    &law_input,
};

LawVerdict law_audience(const FlowRun &p_run) {
    const Evidence &seen = p_run.evidence();
    if (!flows(p_run, STATE_AND_INPUT) || p_run.scenario().clients < 2) {
        return law_held();
    }
    if (seen.observer_state <= 0.0) {
        return law_broken("a watching client never received the state");
    }
    if (seen.observer_input != 0.0) {
        return law_broken(
            "a watching client read %d of another client's input",
            int(seen.observer_input * 1000.0)
        );
    }
    return law_held();
}

const FlowLaw L_AUDIENCE = {
    "audience",
    "a client that does not control the body receives its state and none of "
    "the controller's input",
    &law_audience,
};

LawVerdict law_retained(const FlowRun &p_run) {
    const Evidence &seen = p_run.evidence();
    if (!flows(p_run, RETAINED)) {
        return law_held();
    }
    if (seen.retained_settled <= 0) {
        return law_broken("the retained property was never sent");
    }
    if (seen.retained_quiet != seen.retained_settled) {
        return law_broken(
            "an unchanged retained property was sent %d more times",
            int(seen.retained_quiet - seen.retained_settled)
        );
    }
    if (seen.retained_flipped <= seen.retained_quiet) {
        return law_broken("a changed retained property was not sent");
    }
    if (!seen.mirror_holds_flip) {
        return law_broken("the client never took the changed property");
    }
    if (seen.mirror_state <= 0.0) {
        return law_broken("the moving property stopped with the retained one");
    }
    return law_held();
}

const FlowLaw L_RETAINED = {
    "retained",
    "a retained property is sent only when it changes, while the moving "
    "property beside it keeps sending",
    &law_retained,
};

LawVerdict law_broadcast(const FlowRun &p_run) {
    const Evidence &seen = p_run.evidence();
    if (!flows(p_run, BROADCAST)) {
        return law_held();
    }
    for (int at = 0; at < seen.reached.size(); ++at) {
        if (seen.reached[at].x <= 0.0) {
            return law_broken("peer %d never received the broadcast", at);
        }
    }
    return law_held();
}

const FlowLaw L_BROADCAST = {
    "broadcast-reaches",
    "a broadcast reaches every peer but its author, whether the author is "
    "the server, the controller or the node's authority",
    &law_broadcast,
};

LawVerdict law_converges(const FlowRun &p_run) {
    const Evidence &seen = p_run.evidence();
    if (!flows(p_run, BROADCAST)) {
        return law_held();
    }
    for (int at = 0; at < seen.healed.size(); ++at) {
        if (!seen.healed[at].is_equal_approx(seen.last_authored)) {
            return law_broken(
                "peer %d holds %d against the author's last %d",
                at,
                int(seen.healed[at].x),
                int(seen.last_authored.x)
            );
        }
    }
    return law_held();
}

const FlowLaw L_CONVERGES = {
    "converges",
    "once the link is clean every peer holds the author's last broadcast "
    "value",
    &law_converges,
};

LawVerdict law_quiet(const FlowRun &p_run) {
    const Evidence &seen = p_run.evidence();
    if (!flows(p_run, BROADCAST) || p_run.scenario().dark) {
        return law_held();
    }
    if (seen.rows_after_quiet != seen.rows_before_quiet) {
        return law_broken(
            "a settled broadcast sent %d more rows",
            int(seen.rows_after_quiet - seen.rows_before_quiet)
        );
    }
    return law_held();
}

const FlowLaw L_QUIET = {
    "quiet",
    "a broadcast that stopped changing and reached every peer stops sending",
    &law_quiet,
};

LawVerdict law_heartbeat(const FlowRun &p_run) {
    const Evidence &seen = p_run.evidence();
    if (!flows(p_run, HEARTBEAT)) {
        return law_held();
    }
    if (seen.beats < 2) {
        return law_broken("an unchanged heartbeat sent %d rows", seen.beats);
    }
    if (!seen.beaten.is_equal_approx(seen.last_authored)) {
        return law_broken(
            "a drifted client copy holds %d against %d",
            int(seen.beaten.x),
            int(seen.last_authored.x)
        );
    }
    return law_held();
}

const FlowLaw L_HEARTBEAT = {
    "heartbeat",
    "a broadcast with a heartbeat resends its unchanged value every beat, "
    "and a client copy that drifted takes it back",
    &law_heartbeat,
};

const FlowLaw LAWS[] = {
    L_STATE,
    L_INPUT,
    L_AUDIENCE,
    L_RETAINED,
    L_BROADCAST,
    L_CONVERGES,
    L_QUIET,
    L_HEARTBEAT,
};

FlowScenario scenario(const char *p_label) {
    FlowScenario out;
    out.label = p_label;
    return out;
}

FlowScenario delayed() {
    FlowScenario out = scenario("a client steering over a link 4 ticks long");
    out.delay_ticks = 4.0;
    out.gap_bound = 4;
    return out;
}

FlowScenario observed() {
    FlowScenario out = scenario("a second client watching the body");
    out.clients = 2;
    return out;
}

FlowScenario unsteered() {
    FlowScenario out = scenario("a body nobody steers");
    out.controller = NOBODY;
    out.breaks = "input-reaches";
    return out;
}

FlowScenario retained(const char *p_label, bool p_flips) {
    FlowScenario out = scenario(p_label);
    out.flow = RETAINED;
    out.body = netw_test::gdsrc::STATE_VOLATILE_AND_RETAINED;
    out.flips = p_flips;
    out.breaks = p_flips ? "retained" : nullptr;
    return out;
}

FlowScenario broadcast(const char *p_label) {
    FlowScenario out = scenario(p_label);
    out.flow = BROADCAST;
    out.body = netw_test::gdsrc::BROADCAST_AIM;
    out.clients = 2;
    out.author = 0;
    out.lossy = true;
    return out;
}

FlowScenario dark() {
    FlowScenario out = broadcast("a client's broadcast over a link gone dark");
    out.dark = true;
    out.breaks = "converges";
    return out;
}

FlowScenario restless() {
    FlowScenario out = broadcast("a client that never stops broadcasting");
    out.keeps_authoring = true;
    out.breaks = "quiet";
    return out;
}

FlowScenario from_server() {
    FlowScenario out = broadcast(
        "the server broadcasting for a body nobody "
        "steers"
    );
    out.clients = 1;
    out.controller = NOBODY;
    out.author = SERVER;
    out.lossy = false;
    return out;
}

FlowScenario from_holder(const char *p_label, bool p_late) {
    FlowScenario out = broadcast(p_label);
    out.body = netw_test::gdsrc::BROADCAST_AUTHORITY_AIM;
    out.clients = p_late ? 1 : 2;
    out.controller = p_late ? NOBODY : 1;
    out.holder = 0;
    out.late_holder = p_late;
    out.lossy = false;
    return out;
}

FlowScenario heartbeat() {
    FlowScenario out = scenario("a server broadcast with a heartbeat of 10");
    out.flow = HEARTBEAT;
    out.body = netw_test::gdsrc::BROADCAST_HEARTBEAT_AIM;
    out.controller = NOBODY;
    return out;
}

TEST_CASE(
    "[Netw][Sync][SceneTree] state, input and broadcasts reach the "
    "peers they are meant for, and stop when they stop changing"
) {
    const FlowScenario CORPUS[] = {
        scenario("a client steering a body the server owns"),
        delayed(),
        observed(),
        unsteered(),
        retained("a body with a retained flag", false),
        retained("a retained flag flipped every tick", true),
        broadcast("a client's broadcast over a lossy link that heals"),
        dark(),
        restless(),
        from_server(),
        from_holder("a broadcast by the client holding authority", false),
        from_holder("a broadcast after authority moves to a client", true),
        heartbeat(),
    };
    for (const FlowScenario &row : CORPUS) {
        const FlowRun run(row);
        for (const FlowLaw &law : LAWS) {
            NETW_CELL(law, row);
            const bool breaks
                = row.breaks != nullptr && String(row.breaks) == law.name;
            NETW_LAW_VERDICT(law, run, !breaks);
        }
    }
}

} // namespace TestValueFlow

#endif
