#include "support/fault_log.h"
#include "support/netw_test.h"

#if defined(NETW_TIER_HOSTED)

#include "support/netw_cells.h"

#include "godot/multiplayer.hpp"
#include "godot/scene_tree.hpp"
#include "godot/script.hpp"
#include "godot/spatial_node.hpp"
#include "godot/variant.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/wire/registry.hpp"

namespace TestNetwSessionExtensionDoorLaws {

using namespace godot;
using netw::NetwEntity;
using netw::NetwMultiplayer;
using netw_test::law_broken;
using netw_test::law_held;
using netw_test::LawRowFor;
using netw_test::LawVerdict;

constexpr int64_t ROUTE = 7;
constexpr int64_t ABSENT_ROUTE = 99;
constexpr int64_t CLIENT_PEER = 2;

enum Plant {
    PLANT_NONE,
    PLANT_AN_INNER_NOBODY_ADOPTED,
};

struct DoorScenario {
    String label;
    const char *channel = "SYNC";
    bool binds_the_route = false;
    Error answers = OK;
    NetwMultiplayer::Stat lands_on = NetwMultiplayer::STAT_VERDICT_SKIP;
};

DoorScenario no_implementation_at_all() {
    DoorScenario scenario;
    scenario.label = "no-implementation-at-all";
    scenario.answers = ERR_DOES_NOT_EXIST;
    scenario.lands_on = NetwMultiplayer::STAT_VERDICT_DOES_NOT_EXIST;
    return scenario;
}

DoorScenario a_command_for_an_entity_it_does_not_steer() {
    DoorScenario scenario;
    scenario.label = "a-command-for-an-entity-it-does-not-steer";
    scenario.channel = "PREDICT_COMMAND";
    scenario.binds_the_route = true;
    scenario.answers = ERR_UNAUTHORIZED;
    scenario.lands_on = NetwMultiplayer::STAT_VERDICT_UNAUTHORIZED;
    return scenario;
}

const NetwMultiplayer::Stat VERDICT_STATS[] = {
    NetwMultiplayer::STAT_VERDICT_DOES_NOT_EXIST,
    NetwMultiplayer::STAT_VERDICT_SKIP,
    NetwMultiplayer::STAT_VERDICT_UNAVAILABLE,
    NetwMultiplayer::STAT_VERDICT_UNAUTHORIZED,
    NetwMultiplayer::STAT_VERDICT_INVALID_DATA,
    NetwMultiplayer::STAT_VERDICT_BUSY,
};

class DoorRun {
    DoorScenario declared;
    Plant planted = PLANT_NONE;
    bool inner_is_the_one_adopted = false;
    Error answered = OK;
    int64_t stats[6] = {0, 0, 0, 0, 0, 0};
    int entity_state = -1;

public:
    DoorRun(const DoorScenario &p_scenario, Plant p_plant = PLANT_NONE)
        : declared(p_scenario), planted(p_plant) {
        Ref<NetwMultiplayer> session = NetwMultiplayer::create();
        REQUIRE(session.is_valid());
        session->session_set_role(NetwMultiplayer::ROLE_LISTEN_SERVER);

        Node3D *body = memnew(Node3D);
        netw::gd::scene_root()->add_child(body);
        const Ref<NetwEntity> wrapper = NetwEntity::ensure(body);
        const RID entity = session->entity_of(body);
        if (declared.binds_the_route) {
            session->liveness_bind_route(ROUTE, wrapper.ptr());
        }

        PackedByteArray payload;
        payload.push_back(0);
        payload.push_back(0);
        answered = session->receive_carrier(
            netw::NetwMultiplayer::frame_pack(
                declared.binds_the_route ? ROUTE : ABSENT_ROUTE,
                0,
                netw::wire::builtin_channel(declared.channel),
                payload,
                String()
            ),
            CLIENT_PEER,
            true,
            -1,
            -1
        );
        for (int index = 0; index < 6; index++) {
            stats[index] = session->stats_get(VERDICT_STATS[index]);
        }
        entity_state = session->entity_get_state(entity);

        Ref<SceneMultiplayer> second;
        second.instantiate();
        if (planted != PLANT_AN_INNER_NOBODY_ADOPTED) {
            session->embed_adopt_inner(second);
        }
        inner_is_the_one_adopted = session->session_get_inner() == second;

        session->embed_dispose();
        body->queue_free();
    }

    const DoorScenario &scenario() const {
        return declared;
    }

    bool holds_what_was_adopted() const {
        return inner_is_the_one_adopted;
    }

    Error verdict() const {
        return answered;
    }

    int64_t stat_at(int p_index) const {
        return stats[p_index];
    }

    int state() const {
        return entity_state;
    }
};

typedef LawRowFor<DoorRun> DoorLaw;

LawVerdict law_counted(const DoorRun &p_run) {
    if (p_run.verdict() != p_run.scenario().answers) {
        return law_broken(
            "the carrier answered %d where the door names %d",
            int(p_run.verdict()),
            int(p_run.scenario().answers)
        );
    }
    for (int index = 0; index < 6; index++) {
        const int64_t expected = p_run.scenario().answers != OK
                && VERDICT_STATS[index] == p_run.scenario().lands_on
            ? 1
            : 0;
        if (p_run.stat_at(index) != expected) {
            return law_broken(
                "verdict stat %d reads %d against %d",
                index,
                int(p_run.stat_at(index)),
                int(expected)
            );
        }
    }
    return law_held();
}

LawVerdict law_unmutated(const DoorRun &p_run) {
    if (!p_run.scenario().binds_the_route) {
        return law_held();
    }
    if (p_run.state() != int(NetwMultiplayer::ENTITY_STATE_LIVE)) {
        return law_broken(
            "a refused frame left the entity in state %d",
            p_run.state()
        );
    }
    return law_held();
}

LawVerdict law_reseated(const DoorRun &p_run) {
    if (!p_run.holds_what_was_adopted()) {
        return law_broken("adopting a transport left the session on the old");
    }
    return law_held();
}

const DoorLaw L_COUNTED = {
    "counted",
    "a refused frame answers its verdict and is counted under it once",
    &law_counted,
};

const DoorLaw L_UNMUTATED = {
    "unmutated",
    "a refused frame stops before anything it names changes",
    &law_unmutated,
};

const DoorLaw L_RESEATED = {
    "reseated",
    "adopting a transport re-seats the core, so nothing reads a stale one",
    &law_reseated,
};

const DoorLaw LAWS[] = {L_COUNTED, L_UNMUTATED, L_RESEATED};

TEST_CASE("[Networked][Session] the extension door laws hold") {
    netw_test::FaultWatch expected_faults;
    expected_faults.expect_warning("*[NW0007]*", 1);

    const DoorScenario CORPUS[] = {
        no_implementation_at_all(),
        a_command_for_an_entity_it_does_not_steer(),
    };
    for (const DoorScenario &scenario : CORPUS) {
        const DoorRun run(scenario);
        for (const DoorLaw &law : LAWS) {
            NETW_CELL(law, scenario);
            NETW_LAW_HOLDS(law, run);
        }
    }
}

TEST_CASE("[Networked][Session] a transport nobody adopted reds reseated") {
    const DoorScenario scenario = no_implementation_at_all();
    const DoorRun run(scenario, PLANT_AN_INNER_NOBODY_ADOPTED);
    NETW_CELL(L_RESEATED, scenario);
    NETW_LAW_BREAKS(L_RESEATED, run);
}

} // namespace TestNetwSessionExtensionDoorLaws

#endif
