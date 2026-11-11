#include "support/link.h"
#include "support/netw_test.h"

#if defined(NETW_TIER_HOSTED)

#include "support/fault_log.h"
#include "support/loopback_rig.h"
#include "support/netw_cells.h"
#include "support/netw_recorder.h"

#include "netw/api/entity.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/prediction.hpp"
#include "netw/api/simulation.hpp"

namespace TestPrediction {

using namespace godot;
using netw::NetwEntity;
using netw::NetwMultiplayer;
using netw::NetwPrediction;
using netw::NetwSimulation;
using netw_test::EntityDecl;
using netw_test::FaultWatch;
using netw_test::law_broken;
using netw_test::law_held;
using netw_test::LawRowFor;
using netw_test::LawVerdict;
using netw_test::LoopbackRig;
using netw_test::Recorder;
using netw_test::WorldDecl;

constexpr int SERVER = -1;
constexpr int SETTLE_TICKS = 30;
constexpr int RECOVER_TICKS = 70;
constexpr int PAST_HORIZON = NetwPrediction::ACK_AGE_MAX + 16;

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
    const int age = prediction(rig, "P", 0)->get_ack_age_ticks();
    rig.step_ticks(8);
    if (body(rig, "P", 0)->get_position().x != stalled) {
        return law_broken(
            "a client nobody answered kept predicting past x %g",
            stalled
        );
    }
    if (age != NetwPrediction::ACK_AGE_MAX) {
        return law_broken("the client stalled at an ack age of %d", age);
    }

    rig.conditions(0, Ref<netw::NetwLinkConditions>());
    rig.step_ticks(SETTLE_TICKS);
    if (body(rig, "P", 0)->get_position().x <= stalled) {
        return law_broken("the client never resumed from x %g", stalled);
    }
    if (prediction(rig, "P", 0)->get_ack_age_ticks()
        >= NetwPrediction::ACK_AGE_MAX) {
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
    signals.push_back(StringName("recovered"));
    const Recorder heard(prediction(rig, "P", 0).ptr(), signals);
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
    const int64_t moved = heard.args(signals[2])[0];
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
    if (heard.count(signals[2]) != 1) {
        return law_broken(
            "one shove moved the client %d time(s)",
            heard.count(signals[2])
        );
    }
    return law_held();
}

Error replace_state(NetwMultiplayer *p_api, const RID &p_entity) {
    const RID schema = p_api->schema_create(StringName("RunnerPositionOnly"));
    p_api->schema_add_column(
        schema,
        StringName("position"),
        NetwMultiplayer::COLUMN_VECTOR2,
        1
    );
    p_api->schema_seal(schema);
    const RID set = p_api->property_set_create(
        schema,
        NetwMultiplayer::RECORD_KIND_STATE
    );
    p_api->property_set_add_column(set, 0);
    p_api->property_set_seal(set);
    return p_api->entity_add_property_set(p_entity, set, 0);
}

LawVerdict replaced_state_keeps_client(const World &) {
    const FaultWatch faults;
    LoopbackRig rig(1);
    rig.declare_world(
        WorldDecl().clocked(30, 3).lag_compensated().player(
            runner("P").synced("lean").spawned(),
            0
        )
    );
    run(rig, "P", 0);
    rig.step_ticks(SETTLE_TICKS);
    const Dictionary before = prediction(rig, "P", 0)->reachability();
    if (!Dictionary(before["fields"]).has(StringName("lean"))) {
        return law_broken("the declared state never carried lean");
    }

    for (int at = SERVER; at < 1; ++at) {
        NetwMultiplayer *api = at < 0 ? rig.server() : rig.client(at);
        if (replace_state(api, rig.entity_of(StringName("P"), at)) != OK) {
            return law_broken("the replacement did not attach");
        }
    }
    rig.step_ticks(SETTLE_TICKS);
    if (faults.errors_containing("NW0025")
            + faults.warnings_containing("NW0025")
        != 0) {
        return law_broken("the server refused the client after the swap");
    }
    const Dictionary fields = prediction(rig, "P", 0)->reachability()["fields"];
    if (!fields.has(StringName("position")) || fields.has(StringName("lean"))) {
        return law_broken("the prediction still reads the replaced set");
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
        entity->get_prediction()->set_reconcile_mode(
            NetwPrediction::RECONCILE_JOINT
        );
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
    if (faults.warnings_containing("[NW1014]") != 2) {
        return law_broken(
            "%d NW1014 warning(s) for two refused groups",
            int(faults.warnings_containing("[NW1014]"))
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

const Row ROWS[] = {
    {"a client whose server stops answering",
     "it stops predicting when ack_age_ticks reaches ACK_AGE_MAX, and "
     "resumes once the server answers again",
     &horizon_resumes},
    {"a client the server shoves once",
     "it hears divergence_detected, episode_changed opening, recovered for "
     "the same tick, and episode_changed closing, and is moved once",
     &signals_in_order},
    {"a state set replaced on both copies of a spawned predicted runner",
     "the client stays in the session and the prediction reads the new set",
     &replaced_state_keeps_client},
    {"RECONCILE_JOINT on placed runners whose schedule cannot replay",
     "each is reported once with NW1014, by the peer that predicts it",
     &refused_joint_placed},
    {"RECONCILE_JOINT on spawned runners whose schedule cannot replay",
     "each is reported once with NW1014, by the peer that predicts it",
     &refused_joint_spawned},
};

TEST_CASE(
    "[Networked][Predict][SceneTree] a predicting client stops at its "
    "horizon and resumes, hears its correction in order, and keeps its "
    "seat through a changed declaration"
) {
    const World world = {String("a server and clients on a loopback")};
    for (const Row &row : ROWS) {
        NETW_CELL(row, world);
        NETW_LAW_HOLDS(row, world);
    }
}

} // namespace TestPrediction

#endif
