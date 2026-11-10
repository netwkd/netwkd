#include "support/netw_test.h"

#include "support/event_ring.h"
#include "support/netw_cells.h"

#include "godot/variant.hpp"
#include "netw/api/event_plane.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/schema_core.hpp"
#include "netw/carrier_frame.hpp"

namespace TestNetwEventSessionLaws {

using namespace godot;
using netw::EventPlane;
using netw::NetwCarrierFrame;
using netw::NetwMultiplayer;
using netw::SchemaCore;
using netw_test::EventRing;
using netw_test::law_broken;
using netw_test::law_held;
using netw_test::LawRowFor;
using netw_test::LawVerdict;

constexpr int64_t GATE_ROUTE = 5;
constexpr int64_t ACK_PEER = 4;
constexpr int64_t INTAKE_PEER = 6;
constexpr int64_t COMMIT_TICK = 41;

enum Plant {
    PLANT_NONE,
    PLANT_COMMIT_BEHIND_THE_SESSION,
    PLANT_GATE_BEHIND_THE_SESSION,
    PLANT_MALFORMED_READS_AS_FOREIGN,
};

struct SessionScenario {
    String label;
    int verdicts = 0;
    int acks = 0;
    int commits = 0;
    int admissions = 0;
    int refusals = 0;
    int intakes = 0;
    int truncations = 0;
};

SessionScenario gate_verdicts() {
    SessionScenario scenario;
    scenario.label = "gate-verdicts";
    scenario.verdicts = 3;
    return scenario;
}

SessionScenario carrier_acks() {
    SessionScenario scenario;
    scenario.label = "carrier-acks";
    scenario.acks = 2;
    return scenario;
}

SessionScenario table_commits() {
    SessionScenario scenario;
    scenario.label = "table-commits";
    scenario.commits = 2;
    return scenario;
}

SessionScenario gate_admissions() {
    SessionScenario scenario;
    scenario.label = "gate-admissions";
    scenario.admissions = 3;
    return scenario;
}

SessionScenario gate_refusals() {
    SessionScenario scenario;
    scenario.label = "gate-refusals";
    scenario.refusals = 2;
    return scenario;
}

SessionScenario datagram_intake() {
    SessionScenario scenario;
    scenario.label = "datagram-intake";
    scenario.intakes = 2;
    scenario.truncations = 1;
    return scenario;
}

SessionScenario mixed_session() {
    SessionScenario scenario;
    scenario.label = "mixed-session";
    scenario.verdicts = 2;
    scenario.acks = 1;
    scenario.commits = 1;
    scenario.admissions = 1;
    scenario.refusals = 1;
    scenario.intakes = 1;
    scenario.truncations = 1;
    return scenario;
}

struct Evidence {
    int64_t verdict_total = 0;
    int64_t refused_total = 0;
    int64_t received_bytes = 0;
    int64_t commit_rows = 0;
    int64_t commit_tick = 0;
    int64_t acks_advanced = 0;
    PackedInt64Array ring;
};

class SessionRun {
    SessionScenario declared;
    Plant planted = PLANT_NONE;
    Evidence quiet;
    Evidence watched;

    bool plant_is(Plant p_plant) const {
        return planted == p_plant;
    }

    void drive(bool p_armed, Evidence &r_evidence) {
        Ref<NetwMultiplayer> core;
        core.instantiate();
        core->clock_engine().set_tick(COMMIT_TICK);
        if (p_armed) {
            core->event_arm(
                NetwMultiplayer::EVENT_GROUP_GATE
                | NetwMultiplayer::EVENT_GROUP_STAGE
                | NetwMultiplayer::EVENT_GROUP_VERDICT
                | NetwMultiplayer::EVENT_GROUP_CARRIER
            );
        }

        for (int index = 0; index < declared.verdicts; index++) {
            core->count_verdict(ERR_INVALID_DATA, GATE_ROUTE);
        }
        r_evidence.verdict_total
            = core->stats_get_verdict_count(ERR_INVALID_DATA);

        for (int index = 0; index < declared.admissions; index++) {
            core->finish_stage_verdict(EventPlane::GATE_SYNC, OK, GATE_ROUTE);
        }
        for (int index = 0; index < declared.refusals; index++) {
            if (plant_is(PLANT_GATE_BEHIND_THE_SESSION)) {
                core->sink_verdict(ERR_SKIP, GATE_ROUTE);
            } else {
                core->finish_stage_verdict(
                    EventPlane::GATE_SYNC,
                    ERR_SKIP,
                    GATE_ROUTE
                );
            }
        }
        r_evidence.refused_total = core->stats_get_verdict_count(ERR_SKIP);

        PackedByteArray payload;
        payload.push_back(9);
        for (int index = 0; index < declared.intakes; index++) {
            core->receive_header(
                INTAKE_PEER,
                NetwCarrierFrame::build(payload, false, index + 1, -1, 0, -1)
            );
        }
        for (int index = 0; index < declared.truncations; index++) {
            PackedByteArray truncated;
            truncated.push_back(
                plant_is(PLANT_MALFORMED_READS_AS_FOREIGN)
                    ? uint8_t(0)
                    : uint8_t(NetwCarrierFrame::MAGIC_UNRELIABLE)
            );
            core->receive_header(INTAKE_PEER, truncated);
        }
        r_evidence.received_bytes = core->get_received_bytes();

        for (int index = 0; index < declared.acks; index++) {
            r_evidence.acks_advanced
                += core->note_peer_ack(ACK_PEER, index + 1, 0) ? 1 : 0;
        }

        if (declared.commits > 0) {
            const RID schema = core->schema_create("pose");
            core->schema_add_column(
                schema,
                "hp",
                NetwMultiplayer::COLUMN_F32,
                1
            );
            core->schema_seal(schema);
            const RID table = core->table_create(schema);
            PackedInt64Array routes;
            routes.push_back(GATE_ROUTE);
            PackedFloat32Array hp;
            hp.push_back(3.5f);
            core->table_write_routes(table, routes);
            core->table_write_column(table, 0, hp);
            for (int index = 0; index < declared.commits; index++) {
                if (plant_is(PLANT_COMMIT_BEHIND_THE_SESSION)) {
                    core->store_arrays().commit(table, COMMIT_TICK);
                } else {
                    core->table_commit(table);
                }
            }
            r_evidence.commit_rows = core->table_read_routes(table).size();
            r_evidence.commit_tick = core->table_get_tick(table);
        }

        r_evidence.ring = core->event_drain();
    }

public:
    explicit SessionRun(
        const SessionScenario &p_scenario,
        Plant p_plant = PLANT_NONE
    )
        : declared(p_scenario), planted(p_plant) {
        drive(false, quiet);
        drive(true, watched);
    }

    const SessionScenario &scenario() const {
        return declared;
    }

    const Evidence &unarmed() const {
        return quiet;
    }

    const Evidence &armed() const {
        return watched;
    }

    EventRing ring() const {
        return EventRing(watched.ring);
    }

    int64_t expected(int64_t p_event) const {
        if (p_event == EventPlane::VERDICT) {
            return declared.verdicts + declared.refusals;
        }
        if (p_event == EventPlane::GATE_SYNC) {
            return declared.admissions + declared.refusals;
        }
        if (p_event == EventPlane::DATAGRAM_RECEIVED) {
            return declared.intakes;
        }
        if (p_event == EventPlane::DATAGRAM_MALFORMED) {
            return declared.truncations;
        }
        if (p_event == EventPlane::ACK_ADVANCED) {
            return declared.acks;
        }
        if (p_event == EventPlane::TABLE_COMMIT) {
            return declared.commits;
        }
        return 0;
    }
};

typedef LawRowFor<SessionRun> SessionLaw;

LawVerdict law_neutral(const SessionRun &p_run) {
    const Evidence &quiet = p_run.unarmed();
    const Evidence &watched = p_run.armed();
    if (quiet.received_bytes != watched.received_bytes) {
        return law_broken(
            "%d bytes received armed, %d unarmed",
            int(watched.received_bytes),
            int(quiet.received_bytes)
        );
    }
    if (quiet.refused_total != watched.refused_total) {
        return law_broken(
            "%d refusals armed, %d unarmed",
            int(watched.refused_total),
            int(quiet.refused_total)
        );
    }
    if (quiet.verdict_total != watched.verdict_total) {
        return law_broken(
            "verdict total %d armed, %d unarmed",
            int(watched.verdict_total),
            int(quiet.verdict_total)
        );
    }
    if (quiet.acks_advanced != watched.acks_advanced) {
        return law_broken(
            "%d acks advanced armed, %d unarmed",
            int(watched.acks_advanced),
            int(quiet.acks_advanced)
        );
    }
    if (quiet.commit_rows != watched.commit_rows
        || quiet.commit_tick != watched.commit_tick) {
        return law_broken(
            "the table reads %d rows at tick %d armed, %d at %d unarmed",
            int(watched.commit_rows),
            int(watched.commit_tick),
            int(quiet.commit_rows),
            int(quiet.commit_tick)
        );
    }
    if (!quiet.ring.is_empty()) {
        return law_broken(
            "an unarmed session recorded %d rows",
            EventRing(quiet.ring).size()
        );
    }
    return law_held();
}

LawVerdict law_complete(const SessionRun &p_run) {
    const EventRing ring = p_run.ring();
    const int64_t families[] = {
        EventPlane::VERDICT,
        EventPlane::ACK_ADVANCED,
        EventPlane::TABLE_COMMIT,
        EventPlane::GATE_SYNC,
        EventPlane::DATAGRAM_RECEIVED,
        EventPlane::DATAGRAM_MALFORMED,
    };
    int64_t total = 0;
    for (const int64_t event : families) {
        const int64_t expected = p_run.expected(event);
        total += expected;
        const int64_t seen = ring.count_of(event);
        if (seen != expected) {
            return law_broken(
                "%d rows of %s, %d acts",
                int(seen),
                EventPlane::name_of(event),
                int(expected)
            );
        }
    }
    if (int64_t(ring.size()) != total) {
        return law_broken(
            "the ring holds %d rows for %d acts",
            ring.size(),
            int(total)
        );
    }
    for (int index = 0; index < ring.size(); index++) {
        if (ring.tick_at(index) != COMMIT_TICK) {
            return law_broken(
                "row %d is stamped %d, not the session's %d",
                index,
                int(ring.tick_at(index)),
                int(COMMIT_TICK)
            );
        }
    }
    return law_held();
}

const SessionLaw L_NEUTRAL = {
    "neutral",
    "watching a session changes nothing the session does",
    &law_neutral,
};

const SessionLaw L_COMPLETE = {
    "complete",
    "every act the core owns leaves one row of its own kind",
    &law_complete,
};

const SessionLaw LAWS[] = {L_NEUTRAL, L_COMPLETE};

TEST_CASE("[Networked][Event][Hosted] the session's event laws hold") {
    const SessionScenario CORPUS[] = {
        gate_verdicts(),
        carrier_acks(),
        table_commits(),
        gate_admissions(),
        gate_refusals(),
        datagram_intake(),
        mixed_session(),
    };
    for (const SessionScenario &scenario : CORPUS) {
        const SessionRun run(scenario);
        for (const SessionLaw &law : LAWS) {
            NETW_CELL(law, scenario);
            NETW_LAW_HOLDS(law, run);
        }
    }
}

TEST_CASE(
    "[Networked][Event][Hosted] a commit behind the session's back reds "
    "complete"
) {
    const SessionScenario scenario = table_commits();
    const SessionRun run(scenario, PLANT_COMMIT_BEHIND_THE_SESSION);
    NETW_CELL(L_COMPLETE, scenario);
    NETW_LAW_BREAKS(L_COMPLETE, run);
}

TEST_CASE(
    "[Networked][Event][Hosted] a gate that sinks its verdict without "
    "naming itself reds complete"
) {
    const SessionScenario scenario = gate_refusals();
    const SessionRun run(scenario, PLANT_GATE_BEHIND_THE_SESSION);
    NETW_CELL(L_COMPLETE, scenario);
    NETW_LAW_BREAKS(L_COMPLETE, run);
}

TEST_CASE(
    "[Networked][Event][Hosted] a truncated datagram handed to the "
    "application reds complete"
) {
    const SessionScenario scenario = datagram_intake();
    const SessionRun run(scenario, PLANT_MALFORMED_READS_AS_FOREIGN);
    NETW_CELL(L_COMPLETE, scenario);
    NETW_LAW_BREAKS(L_COMPLETE, run);
}

TEST_CASE(
    "[Networked][Event][Hosted] EV1 event_drain answers the newest 4096 "
    "rows oldest first, and a second drain answers nothing"
) {
    Ref<NetwMultiplayer> core;
    core.instantiate();
    core->event_arm(NetwMultiplayer::EVENT_GROUP_VERDICT);
    const int overflow = 5;
    const int recorded = int(EventPlane::ROW_CAPACITY) + overflow;
    for (int tick = 0; tick < recorded; tick++) {
        core->clock_engine().set_tick(tick);
        core->count_verdict(ERR_INVALID_DATA, GATE_ROUTE);
    }

    const EventRing ring(core->event_drain());
    NETW_REQUIRE_EQ(ring.size(), int(EventPlane::ROW_CAPACITY));
    int out_of_order = 0;
    for (int index = 0; index < ring.size(); index++) {
        out_of_order += ring.tick_at(index) != index + overflow ? 1 : 0;
    }
    NETW_CHECK_EQ(out_of_order, 0);
    NETW_CHECK_EQ(int(core->event_drain().size()), 0);
}

TEST_CASE(
    "[Networked][Event][Hosted] EV2 a session records only the groups it "
    "was armed with, and nothing at all unarmed"
) {
    struct Row {
        const char *label;
        int64_t groups;
        bool disarm;
        int verdicts;
        int gates;
    };
    const Row rows[] = {
        {"unarmed", 0, false, 0, 0},
        {"another group", NetwMultiplayer::EVENT_GROUP_CARRIER, false, 0, 0},
        {"verdicts", NetwMultiplayer::EVENT_GROUP_VERDICT, false, 3, 0},
        {"verdicts and gates",
         NetwMultiplayer::EVENT_GROUP_VERDICT
             | NetwMultiplayer::EVENT_GROUP_GATE,
         false,
         3,
         2},
        {"disarmed", NetwMultiplayer::EVENT_GROUP_VERDICT, true, 0, 0},
    };
    for (const Row &row : rows) {
        INFO(row.label);
        Ref<NetwMultiplayer> core;
        core.instantiate();
        core->event_arm(row.groups);
        if (row.disarm) {
            core->event_arm(0);
        }
        for (int index = 0; index < 3; index++) {
            core->count_verdict(ERR_INVALID_DATA, GATE_ROUTE);
        }
        for (int index = 0; index < 2; index++) {
            core->finish_stage_verdict(EventPlane::GATE_SYNC, OK, GATE_ROUTE);
        }

        const EventRing ring(core->event_drain());
        NETW_CHECK_EQ(ring.count_of(EventPlane::VERDICT), row.verdicts);
        NETW_CHECK_EQ(ring.count_of(EventPlane::GATE_SYNC), row.gates);
        NETW_CHECK_EQ(ring.size(), row.verdicts + row.gates);
    }
}

TEST_CASE(
    "[Networked][Event][Hosted] EV3 a row carries its act's tick, route, "
    "peer, verdict and the two values its kind names"
) {
    Ref<NetwMultiplayer> core;
    core.instantiate();
    core->clock_engine().set_tick(COMMIT_TICK);
    core->event_arm(
        NetwMultiplayer::EVENT_GROUP_GATE | NetwMultiplayer::EVENT_GROUP_CARRIER
    );
    PackedByteArray payload;
    payload.push_back(9);
    core->receive_header(
        INTAKE_PEER,
        NetwCarrierFrame::build(payload, false, 3, -1, 0, -1)
    );
    core->note_peer_ack(ACK_PEER, 1, 0);
    core->finish_stage_verdict(EventPlane::GATE_SYNC, ERR_SKIP, GATE_ROUTE);

    const EventRing ring(core->event_drain());
    NETW_REQUIRE_EQ(ring.size(), 3);
    NETW_CHECK_EQ(ring.event_at(0), int64_t(EventPlane::DATAGRAM_RECEIVED));
    NETW_CHECK_EQ(ring.tick_at(0), COMMIT_TICK);
    NETW_CHECK_EQ(ring.peer_at(0), INTAKE_PEER);
    NETW_CHECK_EQ(ring.a_at(0), int64_t(payload.size()));
    NETW_CHECK_EQ(ring.b_at(0), int64_t(3));
    NETW_CHECK_EQ(ring.event_at(1), int64_t(EventPlane::ACK_ADVANCED));
    NETW_CHECK_EQ(ring.peer_at(1), ACK_PEER);
    NETW_CHECK_EQ(ring.a_at(1), int64_t(1));
    NETW_CHECK_EQ(ring.event_at(2), int64_t(EventPlane::GATE_SYNC));
    NETW_CHECK_EQ(ring.route_at(2), GATE_ROUTE);
    NETW_CHECK_EQ(ring.verdict_at(2), int64_t(ERR_SKIP));
    NETW_CHECK_EQ(ring.entity_at(2), int64_t(0));
}

} // namespace TestNetwEventSessionLaws
