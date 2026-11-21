#include "support/netw_test.h"

#include "support/netw_cells.h"
#include "support/send_drive.h"
#include "support/stream_seat.h"

#include <cstdint>

#include "godot/local_vector.hpp"
#include "godot/variant.hpp"
#include "netw/api/link_conditions.hpp"
#include "netw/api/schema_core.hpp"
#include "netw/repl/link_governor.hpp"
#include "netw/repl/session_send.hpp"
#include "netw/store/schedule.hpp"
#include "netw/wire/registry.hpp"

#if defined(NETW_TIER_HOSTED)

#include "support/internals.h"
#include "support/loopback_rig.h"
#include "support/minted_script.h"

#include "godot/node.hpp"
#include "godot/script.hpp"
#include "godot/spatial_node.hpp"
#include "netw/api/context.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/session.hpp"

#endif

namespace TestBandwidth {

using namespace godot;
using netw::NetwLinkConditions;
using netw::SchemaCore;
using netw::repl::LinkGovernor;
using netw::repl::SessionResult;
using netw::table::SchemaRecord;
using netw::wire::WireRegistry;
using netw_test::law_broken;
using netw_test::law_held;
using netw_test::LawVerdict;
using netw_test::RowOffer;
using netw_test::SessionSend;

struct Row {
    String label;
    LawVerdict (*check)();
};

const int PEER = 5;
const double CALM_RTT_MS = 30.0;
const double EXACT = 1e-6;

void drive(
    LinkGovernor &r_link,
    const Ref<NetwLinkConditions> &p_link,
    int p_datagrams
) {
    const double rtt_ms = p_link->get_latency_ms() * 2.0;
    const double jitter_ms = p_link->get_jitter_ms();
    const double loss = p_link->get_loss_chance();
    double owed = 0.0;
    for (int at = 0; at < p_datagrams; ++at) {
        owed += loss;
        const bool lost = owed >= 1.0;
        if (lost) {
            owed -= 1.0;
        }
        r_link
            .note_ack(PEER, lost ? 0 : 1, lost ? 1 : 0, rtt_ms, jitter_ms, at);
    }
}

void lose_a_round(LinkGovernor &r_link, int64_t &r_now_ms) {
    r_now_ms += int64_t(CALM_RTT_MS) + 1;
    r_link.note_ack(
        PEER,
        0,
        LinkGovernor::MIN_SAMPLES,
        CALM_RTT_MS,
        0.0,
        r_now_ms
    );
}

void climb(LinkGovernor &r_link, int p_rounds, int64_t &r_now_ms) {
    for (int round = 0; round < p_rounds; ++round) {
        r_now_ms += int64_t(CALM_RTT_MS) + 1;
        r_link.note_ack(PEER, 1, 0, CALM_RTT_MS, 0.0, r_now_ms);
    }
}

bool near(double p_value, double p_expected) {
    return p_value > p_expected - EXACT && p_value < p_expected + EXACT;
}

LawVerdict whole_and_good(const LinkGovernor &p_link) {
    if (p_link.mode(PEER) != LinkGovernor::GOOD) {
        return law_broken("the link stayed bad");
    }
    if (!near(double(p_link.share(PEER)), 1.0)) {
        return law_broken(
            "the link kept %d per mille of its rate",
            int(double(p_link.share(PEER)) * 1000.0)
        );
    }
    return law_held();
}

LawVerdict healthy_link() {
    LinkGovernor link;
    drive(link, NetwLinkConditions::wifi(), int(LinkGovernor::WINDOW) * 2);
    if (link.budget_bits(PEER, 8000) != 8000) {
        return law_broken("a healthy link lost part of its budget");
    }
    return whole_and_good(link);
}

LawVerdict poor_link() {
    LinkGovernor link;
    drive(link, NetwLinkConditions::poor_3g(), int(LinkGovernor::WINDOW));
    if (link.mode(PEER) != LinkGovernor::BAD) {
        return law_broken("a poor 3g link stayed good for a whole window");
    }
    if (link.budget_bits(PEER, 8000) != 4000) {
        return law_broken(
            "the budget is %d bits, not half of 8000",
            int(link.budget_bits(PEER, 8000))
        );
    }
    const int64_t floor = LinkGovernor::FLOOR_BITS;
    if (link.budget_bits(PEER, floor) != floor) {
        return law_broken("the budget halved below the floor");
    }
    return law_held();
}

LawVerdict recovering_link() {
    LinkGovernor link;
    int64_t now_ms = 0;
    lose_a_round(link, now_ms);
    const double expected[] = {0.5, 0.625, 0.75, 0.875, 1.0};
    for (int round = 0; round < 5; ++round) {
        if (!near(double(link.share(PEER)), expected[round])) {
            return law_broken(
                "after %d clean round trips the link holds %d per mille",
                round,
                int(double(link.share(PEER)) * 1000.0)
            );
        }
        climb(link, 1, now_ms);
    }
    return whole_and_good(link);
}

LawVerdict burst_link() {
    LinkGovernor link;
    int64_t now_ms = 0;
    lose_a_round(link, now_ms);
    lose_a_round(link, now_ms);
    lose_a_round(link, now_ms);
    if (!near(double(link.share(PEER)), 0.125)) {
        return law_broken("three lossy round trips did not halve three times");
    }
    climb(link, 16, now_ms);
    return whole_and_good(link);
}

LawVerdict relapsing_link() {
    LinkGovernor link;
    int64_t now_ms = 0;
    climb(link, int(LinkGovernor::WINDOW), now_ms);
    lose_a_round(link, now_ms);
    climb(link, 4, now_ms);
    lose_a_round(link, now_ms);
    climb(link, 16, now_ms);
    if (!near(double(link.share(PEER)), 0.875)) {
        return law_broken(
            "a relapse climbed to %d per mille, not one step under the rate",
            int(double(link.share(PEER)) * 1000.0)
        );
    }
    now_ms += LinkGovernor::CEILING_HOLD_MS;
    climb(link, 1, now_ms);
    return whole_and_good(link);
}

LawVerdict capped_link() {
    const double capacity = 0.6;
    LinkGovernor link;
    double owed = 0.0;
    int64_t now_ms = 0;
    double least = 1.0;
    double most = 0.0;
    for (int tick = 0; tick < 600 + 30 * 60; ++tick) {
        now_ms += 16;
        const double share = double(link.share(PEER));
        owed += share > capacity ? (share - capacity) / share : 0.0;
        const bool lost = owed >= 1.0;
        if (lost) {
            owed -= 1.0;
        }
        link.note_ack(PEER, lost ? 0 : 1, lost ? 1 : 0, 50.0, 0.0, now_ms);
        if (tick >= 600) {
            least = share < least ? share : least;
            most = share > most ? share : most;
        }
    }
    const double step = double(LinkGovernor::STEP);
    if (least < capacity - step || most > capacity + step) {
        return law_broken(
            "the rate wandered from %d to %d per mille around 600",
            int(least * 1000.0),
            int(most * 1000.0)
        );
    }
    return law_held();
}

TEST_CASE(
    "[Netw][Repl] a link that loses packets halves its budget, climbs "
    "back an eighth a round trip, and is never throttled for good"
) {
    const Row ROWS[] = {
        {"a wifi link keeps its whole budget", &healthy_link},
        {"a poor 3g link goes bad within a window and halves to the floor",
         &poor_link},
        {"one lossy round trip adds back an eighth a clean round trip",
         &recovering_link},
        {"a burst of lossy round trips leaves no ceiling", &burst_link},
        {"a relapse stops a step under the rate until the hold expires",
         &relapsing_link},
        {"a link capped at 0.6 of the rate settles within a step of it",
         &capped_link},
    };
    for (const Row &row : ROWS) {
        NETW_FORMAT_TEXT(label_text, row.label.utf8().get_data());
        CAPTURE(label_text);
        const LawVerdict verdict = row.check();
        NETW_FORMAT_TEXT(detail_text, verdict.detail);
        CAPTURE(detail_text);
        CHECK(verdict.held);
    }
}

const uint8_t CHANNEL = 44;
const int ROUTES = 8;
const int PASSES = 24;
const int64_t WIDE = int64_t(1) << 22;

const WireRegistry &registry() {
    static const WireRegistry made = [] {
        WireRegistry out;
        netw::wire::ChannelDecl decl;
        decl.id = CHANNEL;
        decl.name = StringName("bandwidth_probe");
        decl.delivery = netw::wire::Delivery::TICK;
        out.register_channel(decl);
        return out;
    }();
    return made;
}

const SchemaRecord &row_schema() {
    static const SchemaRecord record = [] {
        SchemaRecord made;
        made.name = StringName("BandwidthRow");
        SchemaCore::append_column(&made, StringName("x"), SchemaCore::I32, 1);
        SchemaCore::append_column(&made, StringName("y"), SchemaCore::I32, 1);
        SchemaCore::fix(&made);
        return made;
    }();
    return record;
}

RowOffer offer(
    int64_t p_route,
    int64_t p_value,
    float p_priority = 1.0f,
    uint8_t p_marks = 0
) {
    RowOffer out;
    out.route = p_route;
    out.channel = CHANNEL;
    out.schema = &row_schema();
    Array values;
    values.push_back(p_value);
    values.push_back(p_value * 2);
    netw_test::offer_values(out, values);
    out.recipients.push_back(PEER);
    out.priority = p_priority;
    out.marks = p_marks;
    out.masked = true;
    out.tick = p_value;
    return out;
}

bool sent(const SessionResult &p_result, int64_t p_route) {
    for (uint32_t at = 0; at < p_result.sends.size(); ++at) {
        if (p_result.sends[at].route == p_route) {
            return true;
        }
    }
    return false;
}

int64_t frame_bytes(const LocalVector<RowOffer> &p_rows) {
    SessionSend probe;
    netw_test::seat_streams(probe, p_rows);
    const SessionResult out = probe.run(registry(), p_rows, WIDE, 0);
    int64_t most = 0;
    for (uint32_t at = 0; at < out.sends.size(); ++at) {
        most = out.sends[at].framed > most ? out.sends[at].framed : most;
    }
    return most;
}

struct Saturation {
    int sends[ROUTES] = {0};
    int longest_gap[ROUTES] = {0};
};

Saturation saturate(bool p_ranked) {
    LocalVector<RowOffer> first;
    first.push_back(offer(1, 1));
    const int64_t budget_bits = frame_bytes(first) * 8 * ROUTES / 3;
    SessionSend session;
    Saturation out;
    int last_seen[ROUTES];
    for (int route = 0; route < ROUTES; ++route) {
        last_seen[route] = -1;
    }
    for (int pass = 0; pass < PASSES; ++pass) {
        LocalVector<RowOffer> offers;
        for (int route = 0; route < ROUTES; ++route) {
            const float priority = p_ranked ? float(route + 1) : 1.0f;
            offers.push_back(offer(route + 1, pass + 1, priority));
        }
        const SessionResult result = netw_test::drive_send(
            session,
            registry(),
            offers,
            budget_bits,
            uint16_t(pass + 1)
        );
        for (uint32_t at = 0; at < result.sends.size(); ++at) {
            const int route = int(result.sends[at].route) - 1;
            out.sends[route] += 1;
            const int gap = pass - last_seen[route];
            out.longest_gap[route] = MAX(out.longest_gap[route], gap);
            last_seen[route] = pass;
        }
    }
    for (int route = 0; route < ROUTES; ++route) {
        const int gap = PASSES - last_seen[route];
        out.longest_gap[route] = MAX(out.longest_gap[route], gap);
    }
    return out;
}

LawVerdict ranked_routes() {
    const Saturation out = saturate(true);
    for (int route = 0; route < ROUTES; ++route) {
        if (out.sends[route] == 0) {
            return law_broken("route %d was never sent", route + 1);
        }
    }
    if (out.sends[ROUTES - 1] <= out.sends[0]) {
        return law_broken(
            "the top route was sent %d times and the bottom %d",
            out.sends[ROUTES - 1],
            out.sends[0]
        );
    }
    return law_held();
}

LawVerdict equal_routes() {
    const Saturation out = saturate(false);
    for (int route = 0; route < ROUTES; ++route) {
        if (out.longest_gap[route] > ROUTES) {
            return law_broken(
                "route %d waited %d passes",
                route + 1,
                out.longest_gap[route]
            );
        }
    }
    return law_held();
}

LawVerdict player_row() {
    const int BEATS = 30;
    SessionSend session;
    LocalVector<RowOffer> rows;
    rows.push_back(offer(1, 0, 1.0f, netw::store::MARK_PLAYER));
    for (int at = 0; at < BEATS; ++at) {
        RowOffer beat = offer(100 + at, at);
        beat.heartbeat = 3;
        rows.push_back(beat);
    }
    netw_test::seat_streams(session, rows);
    session.run(registry(), rows, WIDE, 1);
    netw_test::accept_streams(session, rows, PEER);
    const int64_t fill = frame_bytes(rows) * 4;
    int64_t resting_ticks = 0;
    int64_t player_skipped = 0;
    for (int64_t tick = 2; tick < 80; ++tick) {
        rows[0] = offer(1, tick, 1.0f, netw::store::MARK_PLAYER);
        const SessionResult result
            = session.run(registry(), rows, fill * 8, tick);
        bool resting = false;
        for (int at = 0; at < BEATS; ++at) {
            resting = resting || sent(result, 100 + at);
        }
        resting_ticks += resting ? 1 : 0;
        if (resting && !sent(result, 1)) {
            player_skipped += 1;
        }
        netw_test::accept_streams(session, rows, PEER);
    }
    if (resting_ticks <= 10) {
        return law_broken("only %d ticks sent a resting row", resting_ticks);
    }
    if (player_skipped != 0) {
        return law_broken(
            "the player's row waited in %d ticks",
            int(player_skipped)
        );
    }
    return law_held();
}

LocalVector<RowOffer> movers(int64_t p_tick, bool p_with_resting) {
    LocalVector<RowOffer> out;
    for (int at = 0; at < 10; ++at) {
        out.push_back(offer(100 + at, p_tick * 1000 + at));
    }
    if (p_with_resting) {
        out.push_back(offer(1, 77));
    }
    return out;
}

RowOffer wide_player(int64_t p_tick) {
    constexpr int COLUMNS = 9;
    static const SchemaRecord schema = [] {
        SchemaRecord made;
        made.name = StringName("BandwidthPlayer");
        for (int at = 0; at < COLUMNS; ++at) {
            SchemaCore::append_column(
                &made,
                StringName(String::num_int64(at)),
                SchemaCore::I64,
                1
            );
        }
        SchemaCore::fix(&made);
        return made;
    }();
    RowOffer row;
    row.route = 1;
    row.channel = CHANNEL;
    row.schema = &schema;
    row.recipients.push_back(PEER);
    row.marks = netw::store::MARK_PLAYER;
    row.tick = p_tick;
    Array values;
    for (int at = 0; at < COLUMNS; ++at) {
        values.push_back(p_tick + at);
    }
    netw_test::offer_values(row, values);
    return row;
}

LawVerdict wide_player_row() {
    constexpr int64_t DATAGRAM = 128;
    constexpr double BYTES_PER_SECOND = 2000.0;
    constexpr int64_t TICKS_PER_SECOND = 60;
    SessionSend session;
    session.set_rate(BYTES_PER_SECOND);
    session.set_tickrate(TICKS_PER_SECOND);
    LocalVector<RowOffer> rows = movers(1, false);
    rows.push_back(wide_player(1));
    const int64_t size = frame_bytes(rows);
    if (size < 75 || size > DATAGRAM) {
        return law_broken("the full player row measures %d bytes", int(size));
    }
    netw_test::seat_streams(session, rows);
    bool sent_player = false;
    bool sent_small_after = false;
    for (int64_t tick = 1; tick <= 80; ++tick) {
        const RowOffer player = wide_player(tick);
        rows = movers(tick, false);
        rows.push_back(player);
        const SessionResult result
            = session.run(registry(), rows, DATAGRAM * 8, tick);
        for (const netw::repl::RowSend &send : result.sends) {
            if (send.route != player.route) {
                sent_small_after = sent_small_after || sent_player;
                continue;
            }
            if (!sent_player) {
                const netw::wire::CodeRow image
                    = netw_test::sent_row(session.world, send);
                if (send.masked || image.is_empty()
                    || image.read_bits(0, 64) != uint64_t(tick)) {
                    return law_broken("the first player row is not current");
                }
                sent_player = true;
            }
        }
        netw_test::accept_streams(session, rows, PEER);
    }
    if (!sent_player) {
        return law_broken("the first player row never fit");
    }
    if (!sent_small_after) {
        return law_broken("smaller rows stopped after the player row");
    }
    return law_held();
}

LawVerdict lost_final_player_row() {
    constexpr int64_t PLAYER = 1;
    constexpr int64_t MOVING = 2;
    constexpr int64_t DATAGRAM = 128;
    LocalVector<RowOffer> rows;
    rows.push_back(offer(PLAYER, 1, 1.0f, netw::store::MARK_PLAYER));
    rows.push_back(offer(MOVING, 1));
    const int64_t fill = frame_bytes(rows);
    SessionSend session;
    session.set_tickrate(60);
    session.set_rate(double(fill * 60));
    netw_test::seat_streams(session, rows);
    const SessionResult lost = session.run(registry(), rows, DATAGRAM * 8, 1);
    if (lost.sends.size() != 1 || lost.sends[0].route != PLAYER
        || lost.sends[0].framed != fill || lost.sends[0].masked) {
        return law_broken("the lost player row was not one full refill");
    }
    int64_t moving_sends = 0;
    bool player_recovered = false;
    for (int64_t tick = 2; tick <= 60; ++tick) {
        rows[0].tick = tick;
        rows[1] = offer(MOVING, tick);
        const SessionResult result
            = session.run(registry(), rows, DATAGRAM * 8, tick, tick * 20);
        for (const netw::repl::RowSend &send : result.sends) {
            if (send.route == MOVING) {
                if (send.framed != fill) {
                    return law_broken(
                        "the moving row spent %d bytes against a %d byte fill",
                        int(send.framed),
                        int(fill)
                    );
                }
                ++moving_sends;
            } else if (send.route == PLAYER && !send.masked) {
                const netw::wire::CodeRow image
                    = netw_test::sent_row(session.world, send);
                if (image.is_empty() || image.read_bits(0, 32) != 1) {
                    return law_broken("the repair does not hold the final row");
                }
                player_recovered = true;
            }
        }
        if (player_recovered && moving_sends > 0) {
            return law_held();
        }
    }
    return moving_sends == 0
        ? law_broken("the competing row never sent")
        : law_broken("the final player row never recovered");
}

LawVerdict resting_row() {
    const int PER_TICK = 2;
    SessionSend session;
    netw_test::seat_streams(session, movers(0, false));
    const int64_t fill = frame_bytes(movers(1, false)) * PER_TICK;
    session.run(registry(), movers(2, true), 0, 2);
    netw_test::seat_streams(session, movers(3, true));
    const int64_t bound = int64_t(
        netw::store::class_weight(netw::store::RowClass::MOVING) * 10 / PER_TICK
        + 2
    );
    for (int64_t tick = 20; tick < 20 + bound + 40; ++tick) {
        const SessionResult result
            = session.run(registry(), movers(tick, true), fill * 8, tick);
        if (sent(result, 1)) {
            if (tick - 20 > bound) {
                return law_broken(
                    "the resting row waited %d ticks past a bound of %d",
                    int(tick - 20),
                    int(bound)
                );
            }
            return law_held();
        }
    }
    return law_broken("the resting row was never sent");
}

TEST_CASE(
    "[Netw][Repl] under a tight budget every entity still updates, "
    "and priority, a player's own entity and waiting decide the share"
) {
    const Row ROWS[] = {
        {"eight routes under a third of their budget, ranked by priority",
         &ranked_routes},
        {"eight equal routes under a third of their budget take turns",
         &equal_routes},
        {"a player's own row beside thirty heartbeat rows", &player_row},
        {"a full player row waits for its bytes beside smaller moving rows",
         &wide_player_row},
        {"a lost final player row recovers beside a moving row",
         &lost_final_player_row},
        {"a resting row beside ten moving rows", &resting_row},
    };
    for (const Row &row : ROWS) {
        NETW_FORMAT_TEXT(label_text, row.label.utf8().get_data());
        CAPTURE(label_text);
        const LawVerdict verdict = row.check();
        NETW_FORMAT_TEXT(detail_text, verdict.detail);
        CAPTURE(detail_text);
        CHECK(verdict.held);
    }
}

#if defined(NETW_TIER_HOSTED)

using netw::Netw;
using netw::NetwMultiplayer;
using netw::NetwSession;
using netw_test::EntityDecl;
using netw_test::Internals;
using netw_test::LoopbackRig;
using netw_test::WorldDecl;

constexpr int TICKRATE = 60;
constexpr double SLOW_KILOBYTES_PER_SECOND = 8.0;
constexpr int64_t FRAME_SLACK = 64;

constexpr const char *SINK = R"(extends Node

var calls := 0

@rpc("any_peer", "call_remote", "reliable")
func note_call(tag: int) -> void:
	calls += 1
)";

struct Wake {
    LoopbackRig rig;
    LocalVector<RID> entities;
    LocalVector<Node2D *> bodies;
    LocalVector<Node *> sinks;
    int64_t step = 0;

    Wake(int p_bodies, int p_send_rate) : rig(2) {
        rig.mount();
        rig.declare_world(
            WorldDecl().clocked(TICKRATE, 0).sending(p_send_rate)
        );
        for (int at = 0; at < p_bodies; ++at) {
            const EntityDecl body
                = EntityDecl()
                      .named(StringName(vformat("Wake%d", at)))
                      .on_schema("WakePose")
                      .synced(StringName("position"))
                      .placed_at(Vector2(double(at), 0.0));
            const RID entity = rig.declare_entity(EntityDecl(body).spawned());
            const int route = int(rig.server()->entity_get_route(entity));
            for (int client = 0; client < 2; ++client) {
                rig.declare_mirror(client, EntityDecl(body).on_route(route));
            }
            entities.push_back(entity);
            bodies.push_back(Object::cast_to<Node2D>(rig.node_of(entity)));
        }
        move(1);
    }

    ~Wake() {
        for (Node *sink : sinks) {
            if (sink->get_parent() != nullptr) {
                sink->get_parent()->remove_child(sink);
            }
            memdelete(sink);
        }
    }

    void seat_sinks() {
        NetwMultiplayer *server = rig.server();
        const RID made = server->entity_create();
        const int64_t route = server->entity_admit(made);
        REQUIRE(route > 0);
        for (int side = -1; side < 2; ++side) {
            const Ref<Script> script = netw_test::script_from(SINK);
            REQUIRE(script.is_valid());
            Node *sink = Object::cast_to<Node>(script->call("new"));
            REQUIRE(sink != nullptr);
            sink->set_name("Sink");
            sinks.push_back(sink);
            rig.branch(side)->add_child(sink);
        }
        REQUIRE(server->entity_bind_node(made, sinks[0]) == OK);
        for (int index = 0; index < 2; ++index) {
            NetwMultiplayer *client = rig.client(index);
            const RID here = client->entity_create();
            REQUIRE(client->entity_bind_route(here, route) == OK);
            REQUIRE(client->entity_bind_node(here, sinks[index + 1]) == OK);
        }
        rig.pump(4);
    }

    void move(int p_ticks) {
        for (int tick = 0; tick < p_ticks; ++tick) {
            step += 1;
            for (uint32_t at = 0; at < bodies.size(); ++at) {
                bodies[at]->set_position(
                    Vector2(double(at), double(step) * 0.5)
                );
            }
            if (!sinks.is_empty()) {
                sinks[1]->rpc_id(1, StringName("note_call"), int(step));
                sinks[0]->rpc(StringName("note_call"), int(step));
            }
            rig.step_ticks(1);
        }
    }

    double link(
        int p_client,
        int64_t p_peer,
        NetwMultiplayer::LinkStat p_stat
    ) const {
        NetwMultiplayer *api
            = p_client < 0 ? rig.server() : rig.client(p_client);
        return api->peer_get_link_stat(p_peer, p_stat);
    }

    int64_t sent_bytes(int64_t p_peer) const {
        return int64_t(link(-1, p_peer, NetwMultiplayer::LINK_STAT_SENT_BYTES));
    }

    LocalVector<double> mirrored_heights(int p_client) const {
        LocalVector<double> out;
        NetwMultiplayer *client = rig.client(p_client);
        for (const RID &entity : entities) {
            const int64_t route = rig.server()->entity_get_route(entity);
            const Node2D *mirror = Object::cast_to<Node2D>(
                client->entity_get_node(Internals::entity_at(client, route))
            );
            REQUIRE(mirror != nullptr);
            out.push_back(mirror->get_position().y);
        }
        return out;
    }
};

struct Spent {
    int64_t most = 0;
    int64_t total = 0;
    int64_t ticks = 0;
};

Spent watch(Wake &r_wake, int64_t p_peer, int p_ticks) {
    Spent out;
    int64_t before = r_wake.sent_bytes(p_peer);
    for (int tick = 0; tick < p_ticks; ++tick) {
        r_wake.move(1);
        const int64_t now = r_wake.sent_bytes(p_peer);
        out.most = MAX(out.most, now - before);
        out.total += now - before;
        out.ticks += 1;
        before = now;
    }
    return out;
}

LawVerdict peer_rate(Wake &r_wake) {
    const int64_t slow = r_wake.rig.peer_id(0);
    const int64_t fast = r_wake.rig.peer_id(1);
    Netw::session(r_wake.rig.branch(-1))
        ->set_peer_bandwidth(slow, SLOW_KILOBYTES_PER_SECOND);
    r_wake.move(90);
    const LocalVector<double> before = r_wake.mirrored_heights(0);
    const Spent held = watch(r_wake, slow, 60);
    const LocalVector<double> after = r_wake.mirrored_heights(0);
    const Spent kept = watch(r_wake, fast, 60);
    const int64_t fill = int64_t(SLOW_KILOBYTES_PER_SECOND * 1000.0 / TICKRATE);
    for (uint32_t at = 0; at < before.size(); ++at) {
        if (after[at] <= before[at]) {
            return law_broken("body %d never updated on the held peer", at);
        }
    }
    if (held.total <= fill * held.ticks / 2) {
        return law_broken(
            "the held peer was sent %d bytes in %d ticks",
            int(held.total),
            int(held.ticks)
        );
    }
    if (held.most > fill + FRAME_SLACK) {
        return law_broken(
            "one tick sent the held peer %d bytes against %d",
            int(held.most),
            int(fill)
        );
    }
    if (held.total > fill * (held.ticks + 2)) {
        return law_broken("the held peer was sent past its rate");
    }
    if (kept.total <= fill * (kept.ticks + 2)) {
        return law_broken(
            "the other peer was held to the same rate, %d bytes",
            int(kept.total)
        );
    }
    return law_held();
}

LawVerdict reliable_alone(Wake &r_wake) {
    r_wake.seat_sinks();
    r_wake.move(240);
    constexpr NetwMultiplayer::LinkStat MODE = NetwMultiplayer::LINK_STAT_MODE;
    for (int client = 0; client < 2; ++client) {
        const int server_side
            = int(r_wake.link(-1, r_wake.rig.peer_id(client), MODE));
        const int client_side = int(r_wake.link(client, 1, MODE));
        if (server_side != NetwMultiplayer::LINK_MODE_GOOD
            || client_side != NetwMultiplayer::LINK_MODE_GOOD) {
            return law_broken(
                "client %d reads mode %d and the server reads mode %d",
                client,
                client_side,
                server_side
            );
        }
    }
    return law_held();
}

double spent(
    const Ref<NetwSession> &p_session,
    int64_t p_peer,
    NetwMultiplayer::Traffic p_first
) {
    constexpr int CAUSES = NetwMultiplayer::TRAFFIC_RECEIVED_STATE
        - NetwMultiplayer::TRAFFIC_SENT_STATE;
    double total = 0.0;
    for (int at = p_first; at < p_first + CAUSES; ++at) {
        total += p_session->traffic(p_peer, NetwMultiplayer::Traffic(at));
    }
    return total;
}

bool within_a_twentieth(double p_value, double p_expected) {
    return Math::abs(p_value - p_expected) <= p_expected * 0.05;
}

LawVerdict traffic_by_cause(Wake &r_wake) {
    constexpr double WINDOW = 3600.0;
    constexpr double UDP_HEADERS = 28.0;
    r_wake.seat_sinks();
    NetwMultiplayer *server = r_wake.rig.server();
    NetwMultiplayer *client = r_wake.rig.client(0);
    Internals::traffic_window(server, WINDOW);
    Internals::traffic_window(client, WINDOW);
    Internals::traffic_advance(server, WINDOW);
    Internals::traffic_advance(client, WINDOW);
    r_wake.move(60);
    Internals::traffic_advance(server, WINDOW);
    Internals::traffic_advance(client, WINDOW);

    const Ref<NetwSession> host = Netw::session(r_wake.rig.branch(-1));
    const Ref<NetwSession> guest = Netw::session(r_wake.rig.branch(0));
    const int64_t to = r_wake.rig.peer_id(0);
    const double state = host->traffic(to, NetwMultiplayer::TRAFFIC_SENT_STATE);
    const double calls = host->traffic(to, NetwMultiplayer::TRAFFIC_SENT_CALLS);
    if (state <= 0.0 || calls <= 0.0) {
        return law_broken(
            "moving bodies and an RPC read %d state and %d call bytes",
            int(state),
            int(calls)
        );
    }
    if (host->traffic(to, NetwMultiplayer::TRAFFIC_SENT_INPUT) != 0.0) {
        return law_broken("a session with no input read input bytes");
    }
    const double arrived
        = guest->traffic(0, NetwMultiplayer::TRAFFIC_RECEIVED_STATE);
    if (!within_a_twentieth(arrived, state)) {
        return law_broken(
            "%d state bytes left and %d arrived",
            int(state),
            int(arrived)
        );
    }
    const double left_total
        = spent(host, to, NetwMultiplayer::TRAFFIC_SENT_STATE);
    const double arrived_total
        = spent(guest, 0, NetwMultiplayer::TRAFFIC_RECEIVED_STATE);
    if (!within_a_twentieth(arrived_total, left_total)) {
        return law_broken(
            "%d bytes left and %d arrived",
            int(left_total),
            int(arrived_total)
        );
    }
    const Ref<NetwSession> other = Netw::session(r_wake.rig.branch(1));
    const double sent_bytes = host->stat(NetwMultiplayer::STAT_SENT_BYTES);
    const double received_bytes
        = guest->stat(NetwMultiplayer::STAT_RECEIVED_BYTES)
        + other->stat(NetwMultiplayer::STAT_RECEIVED_BYTES);
    if (!within_a_twentieth(received_bytes, sent_bytes)) {
        return law_broken(
            "the server counted %d bytes sent and its clients %d received",
            int(sent_bytes),
            int(received_bytes)
        );
    }
    const double packets
        = host->traffic(to, NetwMultiplayer::TRAFFIC_SENT_PACKETS);
    const double headers
        = host->traffic(to, NetwMultiplayer::TRAFFIC_SENT_HEADERS);
    if (headers <= 0.0 || headers >= packets * UDP_HEADERS) {
        return law_broken(
            "%d loopback packets cost %d header bytes",
            int(packets * WINDOW),
            int(headers * WINDOW)
        );
    }
    return law_held();
}

LawVerdict sent_on(Wake &r_wake, int p_expected) {
    const int64_t peer = r_wake.rig.peer_id(0);
    r_wake.move(8);
    const LocalVector<double> before = r_wake.mirrored_heights(0);
    int64_t left = r_wake.sent_bytes(peer);
    int sends = 0;
    for (int tick = 0; tick < TICKRATE; ++tick) {
        r_wake.move(1);
        const int64_t now = r_wake.sent_bytes(peer);
        sends += now > left ? 1 : 0;
        left = now;
    }
    const LocalVector<double> after = r_wake.mirrored_heights(0);
    for (uint32_t at = 0; at < before.size(); ++at) {
        if (after[at] < before[at] + double(TICKRATE - 4) * 0.5) {
            return law_broken("body %d fell behind on the client", at);
        }
    }
    if (sends != p_expected) {
        return law_broken("the server sent on %d of %d ticks", sends, TICKRATE);
    }
    return law_held();
}

LawVerdict sent_every_second_tick(Wake &r_wake) {
    return sent_on(r_wake, TICKRATE / 2);
}

LawVerdict sent_every_tick(Wake &r_wake) {
    return sent_on(r_wake, TICKRATE);
}

LawVerdict priority_reaches_scheduler(Wake &r_wake) {
    NetwMultiplayer *server = r_wake.rig.server();
    const RID weighted = r_wake.entities[0];
    const RID owned = r_wake.entities[1];
    const RID plain = r_wake.entities[2];
    server->entity_get_view(weighted)->set_replication_priority(4.0);
    server->entity_get_view(owned)->set_peer_id(r_wake.rig.peer_id(0));
    server->store_stale_entity(owned);
    r_wake.move(4);
    const netw::store::Store &store = Internals::store(server);
    const float weight = store.weight_of(server->store_slot_of(weighted));
    const float unset = store.weight_of(server->store_slot_of(plain));
    if (weight != 4.0f || unset != 1.0f) {
        return law_broken(
            "priorities of 4 and 1 reached the scheduler as %f and %f",
            weight,
            unset
        );
    }
    const uint8_t mine = store.marks_of(server->store_slot_of(owned));
    const uint8_t theirs = store.marks_of(server->store_slot_of(plain));
    if ((mine & netw::store::MARK_PLAYER) == 0
        || (theirs & netw::store::MARK_PLAYER) != 0) {
        return law_broken(
            "a player's entity reads marks %d and a prop %d",
            int(mine),
            int(theirs)
        );
    }
    return law_held();
}

struct WakeRow {
    String label;
    int bodies;
    LawVerdict (*check)(Wake &);
    int send_rate = 0;
    const char *refusal = nullptr;
};

TEST_CASE(
    "[Netw][Repl][SceneTree] a peer is sent no more than its "
    "bandwidth and still sees every body move, reliable traffic alone "
    "leaves a link its whole budget, traffic reads what left by cause, "
    "and a send rate sends on fewer ticks or is refused"
) {
    const WakeRow ROWS[] = {
        {"240 moving bodies, one peer given 8 KB/s", 240, &peer_rate},
        {"eight moving bodies, one of priority 4 and one a player's own",
         8,
         &priority_reaches_scheduler},
        {"eight moving bodies and a reliable RPC each way every tick",
         8,
         &reliable_alone},
        {"eight moving bodies and an RPC each way, read as traffic",
         8,
         &traffic_by_cause},
        {"eight moving bodies sent 30 times a second at 60 ticks",
         8,
         &sent_every_second_tick,
         30},
        {"eight moving bodies asked for 45 sends a second at 60 ticks",
         8,
         &sent_every_tick,
         45,
         "*sends_per_second got 45*such as 30 or 60*[NW0032]*"},
        {"eight moving bodies asked for 3 sends a second at 60 ticks",
         8,
         &sent_every_tick,
         3,
         "*sends_per_second got 3*at least every 16 ticks, such as 4*"
         "[NW0032]*"},
    };
    for (const WakeRow &row : ROWS) {
        NETW_FORMAT_TEXT(label_text, row.label.utf8().get_data());
        CAPTURE(label_text);
        netw_test::FaultWatch faults;
        if (row.refusal != nullptr) {
            faults.expect_error(row.refusal, 3);
        }
        Wake wake(row.bodies, row.send_rate);
        const LawVerdict verdict = row.check(wake);
        NETW_FORMAT_TEXT(detail_text, verdict.detail);
        CAPTURE(detail_text);
        CHECK(verdict.held);
    }
}

#endif

} // namespace TestBandwidth
