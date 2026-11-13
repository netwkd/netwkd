#include "support/netw_test.h"

#if defined(NETW_TIER_HOSTED)

#include "support/entity_decl.h"
#include "support/fault_log.h"
#include "support/loopback_rig.h"
#include "support/netw_call_log.h"
#include "support/netw_cells.h"

#include <godot_cpp/classes/audio_stream_player.hpp>
#include <godot_cpp/classes/node2d.hpp>

#include <cmath>
#include <initializer_list>

#include "godot/local_vector.hpp"
#include "godot/node.hpp"
#include "godot/templates.hpp"
#include "godot/utility.hpp"
#include "netw/api/context.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/entity_options.hpp"
#include "netw/api/interest.hpp"
#include "netw/api/interest_layer.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/interest/engine.hpp"
#include "netw/repl/spawn_plan.hpp"

namespace TestInterest {

using namespace godot;
using netw::Netw;
using netw::NetwEntity;
using netw::NetwInterestLayer;
using netw::NetwMultiplayer;
using netw::interest::BitSet;
using netw::interest::Delta;
using netw::interest::Engine;
using netw::repl::LeaveDecision;
using netw::repl::reconcile;
using netw::repl::SpawnAction;
using netw::repl::SpawnOp;
using netw::repl::SpawnRow;
using netw_test::CallLog;
using netw_test::EntityDecl;
using netw_test::FaultWatch;
using netw_test::law_broken;
using netw_test::law_held;
using netw_test::LawVerdict;
using netw_test::LoopbackRig;

struct Stand {
    String label;
};

typedef netw_test::LawRowFor<Stand> Row;

constexpr int64_t ROOT = 1;
constexpr int64_t CHILD = 2;
constexpr int64_t LEAF = 3;
constexpr int64_t HOST_PEER = 1;
constexpr int64_t PLANNED_PEER = 2;

constexpr Engine::Policy OUTSIDERS = Engine::HIDE_FROM_OUTSIDERS;
constexpr Engine::Policy INSIDERS = Engine::HIDE_FROM_INSIDERS;

PackedInt64Array bits(std::initializer_list<int> p_bits) {
    PackedInt64Array out;
    for (const int bit : p_bits) {
        out = BitSet::with_bit(out, bit, true);
    }
    return out;
}

Array names(std::initializer_list<const char *> p_ids) {
    Array out;
    for (const char *id : p_ids) {
        out.push_back(StringName(id));
    }
    return out;
}

StringName layer_id(int p_index) {
    return StringName(p_index == 0 ? "a" : "b");
}

void commit(Engine &p_engine) {
    p_engine.commit(p_engine.recompute());
}

bool same(const Variant &p_left, const Variant &p_right) {
    if (p_left.get_type() != p_right.get_type()) {
        return false;
    }
    if (p_left.get_type() != Variant::ARRAY) {
        return bool(p_left == p_right);
    }
    const Array left = p_left;
    const Array right = p_right;
    if (left.size() != right.size()) {
        return false;
    }
    for (int index = 0; index < left.size(); ++index) {
        if (!same(left[index], right[index])) {
            return false;
        }
    }
    return true;
}

int missing_bits(
    const PackedInt64Array &p_left,
    const PackedInt64Array &p_right
) {
    return BitSet::popcount(BitSet::subtract(p_left, p_right));
}

Array edges(std::initializer_list<int64_t> p_keys) {
    Array out;
    for (const int64_t key : p_keys) {
        Array entry;
        entry.push_back(key);
        entry.push_back(0);
        out.push_back(entry);
    }
    return out;
}

Engine chain_engine() {
    Engine engine;
    engine.set_live_peers(bits({0, 1, 2}));
    engine.set_order_key(ROOT, 0, 1);
    engine.set_order_key(CHILD, 1, 2);
    engine.set_order_key(LEAF, 2, 3);
    engine.set_parent(CHILD, ROOT);
    engine.set_parent(LEAF, CHILD);
    return engine;
}

Engine configured_engine() {
    Engine engine = chain_engine();
    engine.set_layer(StringName("near"), bits({0, 2}), OUTSIDERS);
    engine.set_layer(StringName("blind"), bits({2}), INSIDERS);
    engine.set_membership(ROOT, names({"near"}));
    engine.set_membership(CHILD, names({"blind"}));
    engine.set_membership(LEAF, Array());
    engine.set_intent(CHILD, bits({0, 1}));
    return engine;
}

Engine single_entity_engine(Engine::Policy p_policy) {
    Engine engine;
    engine.set_live_peers(bits({0, 1, 2}));
    engine.set_layer(StringName("test"), PackedInt64Array(), p_policy);
    engine.set_membership(ROOT, names({"test"}));
    engine.set_order_key(ROOT, 0, 1);
    return engine;
}

PackedInt64Array random_bits() {
    PackedInt64Array out;
    for (int bit = 0; bit < 3; ++bit) {
        if (netw::gd::randi_range(0, 1) == 1) {
            out = BitSet::with_bit(out, bit, true);
        }
    }
    return out;
}

Engine random_state_engine(int p_seed, bool p_reverse) {
    netw::gd::seed(p_seed);
    PackedInt64Array layer_viewers[2];
    Engine::Policy policies[2];
    for (int index = 0; index < 2; ++index) {
        layer_viewers[index] = random_bits();
        policies[index]
            = netw::gd::randi_range(0, 1) == 1 ? INSIDERS : OUTSIDERS;
    }
    Array memberships[3];
    PackedInt64Array intents[3];
    for (int entity = 0; entity < 3; ++entity) {
        for (int layer = 0; layer < 2; ++layer) {
            if (netw::gd::randi_range(0, 1) == 1) {
                memberships[entity].push_back(layer_id(layer));
            }
        }
        intents[entity] = random_bits();
    }

    Engine engine;
    engine.set_live_peers(bits({0, 1, 2}));
    for (int step = 0; step < 2; ++step) {
        const int at = p_reverse ? 1 - step : step;
        engine.set_layer(layer_id(at), layer_viewers[at], policies[at]);
    }
    const int64_t keys[3] = {ROOT, CHILD, LEAF};
    for (int step = 0; step < 3; ++step) {
        const int at = p_reverse ? 2 - step : step;
        engine.set_order_key(keys[at], at, at + 1);
        engine.set_membership(keys[at], memberships[at]);
        engine.set_intent(keys[at], intents[at]);
    }
    engine.set_parent(CHILD, ROOT);
    engine.set_parent(LEAF, CHILD);
    return engine;
}

bool oracle_grant(
    int p_entity,
    int p_peer_bit,
    int p_policy_bits,
    int p_viewer_bits,
    int p_membership_bits
) {
    bool has_membership = false;
    bool admitted = false;
    for (int layer = 0; layer < 2; ++layer) {
        if ((p_membership_bits & (1 << (p_entity * 2 + layer))) == 0) {
            continue;
        }
        has_membership = true;
        const bool viewer
            = (p_viewer_bits & (1 << (layer * 2 + p_peer_bit))) != 0;
        const bool outsiders = ((p_policy_bits >> layer) & 1) == 0;
        admitted = admitted || (outsiders ? viewer : !viewer);
    }
    return has_membership ? admitted : true;
}

Engine small_model_engine(
    int p_policy_bits,
    int p_viewer_bits,
    int p_membership_bits
) {
    Engine engine;
    engine.set_live_peers(bits({0, 1}));
    engine.set_order_key(ROOT, 0, 1);
    engine.set_order_key(CHILD, 1, 2);
    engine.set_parent(CHILD, ROOT);
    for (int layer = 0; layer < 2; ++layer) {
        PackedInt64Array viewers;
        for (int peer_bit = 0; peer_bit < 2; ++peer_bit) {
            if ((p_viewer_bits & (1 << (layer * 2 + peer_bit))) != 0) {
                viewers = BitSet::with_bit(viewers, peer_bit, true);
            }
        }
        engine.set_layer(
            layer_id(layer),
            viewers,
            ((p_policy_bits >> layer) & 1) == 0 ? OUTSIDERS : INSIDERS
        );
    }
    for (int entity = 0; entity < 2; ++entity) {
        Array memberships;
        for (int layer = 0; layer < 2; ++layer) {
            if ((p_membership_bits & (1 << (entity * 2 + layer))) != 0) {
                memberships.push_back(layer_id(layer));
            }
        }
        engine.set_membership(entity == 0 ? ROOT : CHILD, memberships);
    }
    return engine;
}

LawVerdict oracle(const Stand &) {
    int failures = 0;
    for (int policy_bits = 0; policy_bits < 4; ++policy_bits) {
        for (int viewer_bits = 0; viewer_bits < 16; ++viewer_bits) {
            for (int membership_bits = 0; membership_bits < 16;
                 ++membership_bits) {
                Engine engine = small_model_engine(
                    policy_bits,
                    viewer_bits,
                    membership_bits
                );
                commit(engine);
                for (int entity = 0; entity < 2; ++entity) {
                    for (int peer_bit = 0; peer_bit < 2; ++peer_bit) {
                        bool expected = oracle_grant(
                            entity,
                            peer_bit,
                            policy_bits,
                            viewer_bits,
                            membership_bits
                        );
                        if (entity == 1) {
                            expected = expected && engine.test(ROOT, peer_bit);
                        }
                        const int64_t key = entity == 0 ? ROOT : CHILD;
                        if (engine.test(key, peer_bit) != expected) {
                            failures += 1;
                        }
                    }
                }
            }
        }
    }
    if (failures != 0) {
        return law_broken("%d grants disagree with the oracle", failures);
    }
    return law_held();
}

LawVerdict mutation_order(const Stand &) {
    for (int seed = 0; seed < 64; ++seed) {
        Engine forward = random_state_engine(seed, false);
        Engine reverse = random_state_engine(seed, true);
        if (!same(
                forward.recompute().to_array(),
                reverse.recompute().to_array()
            )) {
            return law_broken("seed %d committed two deltas", seed);
        }
    }
    return law_held();
}

LawVerdict monotonic(const Stand &) {
    Engine granting = single_entity_engine(OUTSIDERS);
    granting.set_layer(StringName("test"), bits({0}), OUTSIDERS);
    commit(granting);
    const PackedInt64Array fewer = granting.row_of(ROOT);
    granting.set_layer(StringName("test"), bits({0, 1}), OUTSIDERS);
    commit(granting);
    const PackedInt64Array more = granting.row_of(ROOT);
    if (missing_bits(fewer, more) != 0 || !BitSet::test(more, 1)) {
        return law_broken("a new viewer did not only add a peer");
    }

    Engine hiding = single_entity_engine(INSIDERS);
    hiding.set_layer(StringName("test"), bits({0}), INSIDERS);
    commit(hiding);
    const PackedInt64Array wider = hiding.row_of(ROOT);
    hiding.set_layer(StringName("test"), bits({0, 1}), INSIDERS);
    commit(hiding);
    const PackedInt64Array narrower = hiding.row_of(ROOT);
    if (missing_bits(narrower, wider) != 0 || BitSet::test(narrower, 1)) {
        return law_broken("a new insider did not only hide from a peer");
    }
    return law_held();
}

LawVerdict clamp(const Stand &) {
    Engine engine = configured_engine();
    commit(engine);
    if (missing_bits(engine.row_of(CHILD), engine.row_of(ROOT)) != 0) {
        return law_broken("the child reaches a peer its parent does not");
    }
    if (missing_bits(engine.row_of(LEAF), engine.row_of(CHILD)) != 0) {
        return law_broken("the leaf reaches a peer its parent does not");
    }
    return law_held();
}

LawVerdict edge_order(const Stand &) {
    Engine engine = chain_engine();
    engine.set_membership(ROOT, names({"test"}));
    engine.set_membership(CHILD, names({"test"}));
    engine.set_membership(LEAF, names({"test"}));
    engine.set_layer(StringName("test"), bits({0}), OUTSIDERS);
    commit(engine);

    engine.set_layer(StringName("test"), PackedInt64Array(), OUTSIDERS);
    Delta hiding = engine.recompute();
    if (!same(hiding.hides, edges({LEAF, CHILD, ROOT}))) {
        return law_broken("the hides did not run leaf to root");
    }
    engine.commit(hiding);

    engine.set_layer(StringName("test"), bits({0}), OUTSIDERS);
    if (!same(engine.recompute().shows, edges({ROOT, CHILD, LEAF}))) {
        return law_broken("the shows did not run root to leaf");
    }
    return law_held();
}

LawVerdict replay(const Stand &) {
    Engine first = configured_engine();
    Engine second = configured_engine();
    const Delta first_delta = first.recompute();
    const Delta second_delta = second.recompute();
    if (!same(first_delta.to_array(), second_delta.to_array())) {
        return law_broken("one state gave two deltas");
    }
    first.commit(first_delta);
    second.commit(second_delta);
    if (!same(first.rows(), second.rows())) {
        return law_broken("one state gave two matrices");
    }

    first.set_membership(ROOT, names({"near"}));
    first.set_layer(StringName("near"), bits({0, 2}), OUTSIDERS);
    if (!first.recompute().is_empty()) {
        return law_broken("restating the state owed a change");
    }
    return law_held();
}

SpawnRow plan_row(
    int64_t p_route,
    int64_t p_parent_route,
    bool p_held,
    bool p_desired
) {
    SpawnRow out;
    out.route = p_route;
    out.parent_route = p_parent_route;
    if (p_held) {
        out.recipients.push_back(int32_t(PLANNED_PEER));
    }
    out.local_desired[PLANNED_PEER] = p_desired;
    return out;
}

LocalVector<SpawnRow> nested(
    bool p_held,
    bool p_parent_desired,
    bool p_child_desired
) {
    LocalVector<SpawnRow> rows;
    rows.push_back(plan_row(1, 0, p_held, p_parent_desired));
    rows.push_back(plan_row(2, 1, p_held, p_child_desired));
    return rows;
}

LocalVector<SpawnOp> plan(const LocalVector<SpawnRow> &p_rows) {
    PackedInt32Array peers;
    peers.push_back(int32_t(PLANNED_PEER));
    return reconcile(p_rows, peers);
}

String planned(const LocalVector<SpawnOp> &p_plan, SpawnAction p_action) {
    String out;
    for (const SpawnOp &op : p_plan) {
        if (op.action == p_action) {
            out += itos(op.route);
        }
    }
    return out;
}

LawVerdict gain_order(const Stand &) {
    const String spawns
        = planned(plan(nested(false, true, true)), SpawnAction::SPAWN);
    if (spawns != "12") {
        return law_broken("spawned %s", spawns.utf8().get_data());
    }
    return law_held();
}

LawVerdict loss_order(const Stand &) {
    const String hides
        = planned(plan(nested(true, false, false)), SpawnAction::HIDE);
    if (hides != "21") {
        return law_broken("hid %s", hides.utf8().get_data());
    }
    return law_held();
}

LawVerdict orphan_child(const Stand &) {
    const String spawns
        = planned(plan(nested(false, false, true)), SpawnAction::SPAWN);
    if (!spawns.is_empty()) {
        return law_broken("spawned %s", spawns.utf8().get_data());
    }
    return law_held();
}

LawVerdict kept_child(const Stand &) {
    LocalVector<SpawnRow> rows = nested(true, false, true);
    LeaveDecision keep;
    keep.hide = false;
    rows[1].leave.insert(PLANNED_PEER, keep);
    const LocalVector<SpawnOp> ops = plan(rows);
    const String hides = planned(ops, SpawnAction::HIDE);
    if (hides != "21") {
        return law_broken("hid %s", hides.utf8().get_data());
    }
    if (!ops[0].forced) {
        return law_broken("the kept child's hide was not forced");
    }
    return law_held();
}

const Row ENGINE_ROWS[] = {
    {"every state of two layers, two peers and a parent with its child",
     "each peer is granted exactly what the reference rule grants",
     &oracle},
    {"sixty four seeded states, each stated in two orders",
     "both orders commit the same delta",
     &mutation_order},
    {"a layer gaining a viewer, hiding from outsiders and then from "
     "insiders",
     "the first only grants that viewer and the second only hides from it",
     &monotonic},
    {"a chain of three entities in layers of their own",
     "no child reaches a peer its parent does not",
     &clamp},
    {"a chain of three in one layer losing its only viewer, then gaining it",
     "the hides run leaf to root and the shows root to leaf",
     &edge_order},
    {"one state built twice, then stated again",
     "both builds commit the same delta and matrix, and the restatement owes "
     "nothing",
     &replay},
    {"a parent and its child a peer gains",
     "the spawn plan sends the parent first",
     &gain_order},
    {"a parent and its child a peer loses",
     "the spawn plan hides the child first",
     &loss_order},
    {"a child a peer is owed under a parent it is not",
     "the spawn plan sends nothing",
     &orphan_child},
    {"a parent that despawns over a child that asked to be kept",
     "both hide child first and the child's hide is forced",
     &kept_child},
};

TEST_CASE(
    "[Networked][Interest] the interest engine grants each peer what its "
    "layers say, in any order, parents before children"
) {
    const Stand stand = {String("the interest engine and spawn plan")};
    for (const Row &row : ENGINE_ROWS) {
        NETW_CELL(row, stand);
        NETW_LAW_HOLDS(row, stand);
    }
}

LawVerdict host_declares(const Stand &) {
    LoopbackRig rig(0);
    rig.mount();
    NetwMultiplayer *server = rig.server();
    server->session_set_role(NetwMultiplayer::ROLE_LISTEN_SERVER);

    Node *body = memnew(Node);
    body->set_name("Watcher");
    const Ref<NetwEntity> entity = Netw::configure_entity(body);
    entity->get_interest()->join(StringName("zone"));
    entity->get_interest()->join(StringName("ward"));
    entity->get_interest()->join(StringName("zone"));
    entity->get_interest()->leave(StringName("ward"));
    rig.branch()->add_child(body);
    body->set_owner(rig.branch());
    if (entity->get_interest()->layer_ids().size() != 1) {
        return law_broken("the declaration does not name one layer");
    }
    server->session_flush_deferred();
    const RID handle = entity->get_rid_handle();
    if (server->interest_get_layers(handle).size() != 1) {
        return law_broken("the host did not join the declared layer");
    }
    rig.branch()->remove_child(body);
    rig.branch()->add_child(body);
    body->set_owner(rig.branch());
    server->session_flush_deferred();
    if (server->interest_get_layers(entity->get_rid_handle()).size() != 1) {
        return law_broken("the layer was not joined again on re-entry");
    }
    return law_held();
}

struct Stealthed {
    LoopbackRig rig;
    NetwMultiplayer *server = nullptr;
    Ref<NetwEntity> entity;
    Node2D *body = nullptr;
    AudioStreamPlayer *speaker = nullptr;
    Ref<NetwInterestLayer> layer;

    explicit Stealthed(int p_route) : rig(1) {
        server = rig.server();
        server->session_set_role(NetwMultiplayer::ROLE_LISTEN_SERVER);
        const RID handle = rig.declare_entity(
            EntityDecl().named("Stealthed").on_route(p_route)
        );
        entity = server->entity_get_view(handle);
        body = Object::cast_to<Node2D>(rig.node_of(handle));
        REQUIRE(body != nullptr);
        speaker = memnew(AudioStreamPlayer);
        speaker->set_name("Speaker");
        speaker->set_volume_db(-12.0);
        body->add_child(speaker);
        layer = server->interest_layer(StringName("stealth"));
        REQUIRE(layer.is_valid());
    }

    bool host_sees() const {
        return server->interest_player_sees(HOST_PEER, entity);
    }

    void hide() {
        layer->add_entity(entity);
        rig.flush_interest();
    }

    void show() {
        layer->add_viewer(HOST_PEER);
        rig.flush_interest();
    }
};

LawVerdict muted_body(const Stand &) {
    Stealthed stand(66);
    const RID handle = stand.entity->get_rid_handle();
    const int64_t guest = stand.rig.peer_id(0);
    if (!stand.server->interest_is_visible(handle, guest)) {
        return law_broken("an entity on no layer is hidden from a peer");
    }
    stand.hide();
    if (stand.host_sees() || stand.body->is_visible()) {
        return law_broken("the host still draws a body it cannot see");
    }
    if (stand.server->interest_is_visible(handle, guest)
        || !stand.server->interest_is_visible(handle, HOST_PEER)) {
        return law_broken("visibility differs from what replication sends");
    }
    if (std::abs(stand.speaker->get_volume_db() + 80.0) > 0.001) {
        return law_broken("the hidden body is still heard");
    }
    stand.show();
    if (!stand.host_sees() || !stand.body->is_visible()) {
        return law_broken("the shown body is not drawn");
    }
    if (std::abs(stand.speaker->get_volume_db() + 12.0) > 0.001) {
        return law_broken("the shown body lost its volume");
    }
    return law_held();
}

LawVerdict perception_changed(const Stand &) {
    Stealthed stand(67);
    stand.hide();
    if (stand.body->is_visible()) {
        return law_broken("the hidden body is drawn");
    }
    stand.layer->set_default_perception_policy(
        NetwMultiplayer::PERCEPTION_POLICY_SHOW
    );
    if (stand.host_sees() || !stand.body->is_visible()) {
        return law_broken("the show policy did not draw the held member");
    }
    stand.layer->set_default_perception_policy(
        NetwMultiplayer::PERCEPTION_POLICY_HIDE
    );
    if (stand.body->is_visible()) {
        return law_broken("the hide policy did not hide the held member");
    }
    return law_held();
}

bool perceived(const Array &p_call, bool p_visible) {
    return p_call.size() == 3 && bool(p_call[0]) == p_visible
        && int64_t(p_call[1]) == HOST_PEER
        && StringName(p_call[2]) == StringName("stealth");
}

LawVerdict custom_perception(const Stand &) {
    Stealthed stand(68);
    CallLog heard;
    stand.entity->get_interest()->on_perception_policy(
        StringName("stealth"),
        NetwMultiplayer::PERCEPTION_POLICY_CUSTOM,
        heard.callable("perceive")
    );
    stand.hide();
    if (heard.count("perceive") != 1 || !stand.body->is_visible()) {
        return law_broken("the action did not replace hiding");
    }
    if (!perceived(heard.args("perceive"), false)) {
        return law_broken("the hiding call names the wrong edge");
    }
    stand.show();
    if (heard.count("perceive") != 2) {
        return law_broken("the action did not run on the show");
    }
    if (!perceived(heard.args("perceive", 1), true)) {
        return law_broken("the showing call names the wrong edge");
    }
    return law_held();
}

Array named_ids(const TypedArray<NetwEntity> &p_entities) {
    Array out;
    for (int at = 0; at < p_entities.size(); ++at) {
        const NetwEntity *row = Object::cast_to<NetwEntity>(p_entities[at]);
        if (row != nullptr) {
            out.push_back(row->get_name());
        }
    }
    return out;
}

struct Cars {
    LoopbackRig rig;
    NetwMultiplayer *server = nullptr;
    NetwMultiplayer *owner = nullptr;
    Ref<NetwEntity> mine;
    Ref<NetwEntity> theirs;
    Ref<NetwEntity> local_mine;
    Ref<NetwEntity> local_theirs;

    explicit Cars(int p_route) : rig(1) {
        server = rig.server();
        owner = rig.client(0);
        mine = car("OwnCar", p_route, "arena");
        theirs = car("OtherCar", p_route + 1, "arena");
        rig.flush_interest();
        local_mine = mirror("OwnCar", p_route, "arena");
        local_theirs = mirror("OtherCar", p_route + 1, "arena");
    }

    static EntityDecl car_decl(const char *p_name, int p_route) {
        return EntityDecl().named(p_name).on_route(p_route);
    }

    Ref<NetwEntity> car(const char *p_name, int p_route, const char *p_layer) {
        const Ref<NetwEntity> made = server->entity_get_view(
            rig.declare_entity(car_decl(p_name, p_route))
        );
        REQUIRE(made.is_valid());
        made->set_name(StringName(p_name));
        server->interest_layer(StringName(p_layer))->add_entity(made);
        return made;
    }

    Ref<NetwEntity> mirror(
        const char *p_name,
        int p_route,
        const char *p_layer
    ) {
        const Ref<NetwEntity> made = owner->entity_get_view(
            rig.declare_mirror(0, car_decl(p_name, p_route))
        );
        REQUIRE(made.is_valid());
        made->set_name(StringName(p_name));
        made->get_interest()->join(StringName(p_layer));
        return made;
    }

    Array shared_on_owner() const {
        return named_ids(
            owner->interest_shared_entities(local_mine, StringName("arena"))
        );
    }
};

LawVerdict owner_shares(const Stand &) {
    Cars stand(73);
    stand.car("Spectator", 75, "lobby");
    stand.rig.flush_interest();
    stand.mirror("Spectator", 75, "lobby");

    const Array committed = named_ids(stand.server->interest_shared_entities(
        stand.mine,
        StringName("arena")
    ));
    const Array derived = stand.shared_on_owner();
    if (!same(committed, derived)) {
        return law_broken("the owner and the server name different rosters");
    }
    if (derived.size() != 1
        || StringName(derived[0]) != StringName("OtherCar")) {
        return law_broken("the owner's roster is not the other car");
    }
    const Variant theirs = stand.local_theirs;
    if (!stand.owner->liveness_get_entities().has(theirs)) {
        return law_broken("the other car is not live on the owner");
    }
    const StringName arena("arena");
    stand.owner->interest_layer_free(stand.owner->interest_layer_find(arena));
    const RID handle = stand.local_mine->get_rid_handle();
    if (!stand.owner->interest_get_layers(handle).is_empty()
        || stand.owner->interest_layer_find(arena).is_valid()) {
        return law_broken("asking for an entity's layers made a layer");
    }
    return law_held();
}

LawVerdict owner_in_scene(const Stand &) {
    Cars stand(76);
    stand.owner->interest_client_admit(
        stand.owner->interest_layer_create(StringName("scene:Arena")),
        stand.local_mine
    );
    const Array resolved
        = stand.owner->interest_resolved_layer_ids(stand.local_mine);
    if (!resolved.has(StringName("scene:Arena"))) {
        return law_broken("the owner's scene layer did not admit its car");
    }
    if (!resolved.has(StringName("arena"))) {
        return law_broken("the scene layer hid the layer the car joined");
    }
    const Array shared = stand.shared_on_owner();
    if (shared.size() != 1
        || StringName(shared[0]) != StringName("OtherCar")) {
        return law_broken("the owner does not find the other car");
    }
    return law_held();
}

struct Freeing {
    NetwMultiplayer *server = nullptr;
    RID earlier;
};

Freeing *freeing = nullptr;

Array free_earlier(const RID &p_layer) {
    freeing->server->interest_layer_free(freeing->earlier);
    freeing->server->interest_layer_free(p_layer);
    return Array();
}

LawVerdict driver_answers(const Stand &) {
    FaultWatch faults;
    faults.expect_error("*not an Array*[NW1007]*", 1);
    faults.expect_error("*not an entity of this session*[NW1007]*", 1);
    LoopbackRig rig(1);
    NetwMultiplayer *server = rig.server();
    const Ref<NetwEntity> own = server->entity_get_view(
        rig.declare_entity(EntityDecl().named("Own").on_route(81))
    );
    const Ref<NetwEntity> foreign = rig.client(0)->entity_get_view(
        rig.declare_mirror(0, EntityDecl().named("Own").on_route(81))
    );
    REQUIRE(own.is_valid());
    REQUIRE(foreign.is_valid());
    const Ref<NetwInterestLayer> counted
        = server->interest_layer(StringName("count"));
    const Ref<NetwInterestLayer> layer
        = server->interest_layer(StringName("sight"));
    REQUIRE(counted.is_valid());
    REQUIRE(layer.is_valid());
    const CallLog driver;

    counted->set_driver(driver.answering("number", 7));
    if (server->interest_flush_now() != ERR_INVALID_DATA) {
        return law_broken("a driver answering a number was taken");
    }
    counted->set_driver(Callable());
    Array foreign_only;
    foreign_only.push_back(foreign);
    layer->set_driver(driver.answering("foreign", foreign_only));
    if (server->interest_flush_now() != ERR_DOES_NOT_EXIST
        || layer->has_entity(foreign)) {
        return law_broken("another session's entity joined the layer");
    }
    Array own_only;
    own_only.push_back(own);
    layer->set_driver(driver.answering("own", own_only));
    if (server->interest_flush_now() != OK || !layer->has_entity(own)) {
        return law_broken("the driver's own entity did not join the layer");
    }
    layer->set_driver(Callable());
    if (layer->add_entity(foreign) || layer->has_entity(foreign)) {
        return law_broken("the layer took another session's entity");
    }
    layer->connect("interest_entered", driver.callable("entered"));
    server->interest_layer_report_edge(
        server->interest_layer_find(StringName("sight")),
        foreign,
        rig.peer_id(0),
        true
    );
    if (driver.count("entered") != 0) {
        return law_broken("the layer reported another session's entity");
    }
    const RID first = server->interest_layer_create(StringName("first"));
    const RID second = server->interest_layer_create(StringName("second"));
    const RID third = server->interest_layer_create(StringName("third"));
    server->interest_layer_set_driver_callback(
        first,
        driver.answering("first", Array())
    );
    server->interest_layer_set_driver_callback(
        second,
        callable_mp_static(&free_earlier)
    );
    server->interest_layer_set_driver_callback(
        third,
        driver.answering("third", Array())
    );
    Freeing held;
    held.server = server;
    held.earlier = first;
    freeing = &held;
    const Error drove = server->interest_flush_now();
    freeing = nullptr;
    if (drove != OK || driver.count("third") != 1) {
        return law_broken("a driver after two freed layers did not run");
    }
    return law_held();
}

LawVerdict kept_view(const Stand &) {
    Ref<NetwInterestLayer> ended;
    int64_t viewer = 0;
    {
        LoopbackRig rig(1);
        NetwMultiplayer *server = rig.server();
        viewer = rig.peer_id(0);
        const StringName name("kept");
        const Ref<NetwInterestLayer> freed = server->interest_layer(name);
        REQUIRE(freed.is_valid());
        const RID gone = server->interest_layer_find(name);
        server->interest_layer_free(gone);
        if (freed->add_viewer(viewer)) {
            return law_broken("a freed layer took a viewer");
        }
        if (server->interest_layer_find(name).is_valid()
            || server->interest_has_layer(name)) {
            return law_broken("a freed layer came back");
        }
        const RID held = rig.declare_entity(
            EntityDecl().named("Held").on_route(85)
        );
        const RID late = rig.declare_entity(
            EntityDecl().named("Late").on_route(86)
        );
        const Error answers[] = {
            server->interest_layer_remove_viewer(gone, viewer),
            server->interest_join(held, gone),
            server->interest_leave(held, gone),
            server->interest_on_enter(held, gone, Callable()),
            server->interest_on_leave(held, gone, Callable()),
            server->interest_on_leave_policy(
                held,
                gone,
                NetwMultiplayer::LEAVE_POLICY_HIDE,
                Callable()
            ),
            server->interest_on_perception_policy(
                held,
                gone,
                NetwMultiplayer::PERCEPTION_POLICY_HIDE,
                Callable()
            ),
        };
        for (const Error answer : answers) {
            if (answer != ERR_DOES_NOT_EXIST) {
                return law_broken("a freed layer took a change by its RID");
            }
        }
        const StringName doomed("doomed");
        const RID freed_on_add = server->interest_layer_create(doomed);
        server->interest_layer(doomed)->connect(
            "entity_added",
            Callable(server, "interest_layer_free")
                .bind(freed_on_add)
                .unbind(1)
        );
        server->interest_layer_add_entity(freed_on_add, held);
        const StringName refilled("refilled");
        const RID filled_on_free = server->interest_layer_create(refilled);
        server->interest_layer_add_entity(filled_on_free, held);
        server->interest_layer(refilled)->connect(
            "entity_removed",
            Callable(server, "interest_layer_add_entity")
                .bind(filled_on_free, late)
                .unbind(1)
        );
        server->interest_layer_free(filled_on_free);
        if (server->interest_has_layer(doomed)
            || server->interest_has_layer(refilled)
            || !server->interest_membership_ids(held).is_empty()
            || !server->interest_membership_ids(late).is_empty()) {
            return law_broken("a layer freed in its own handler came back");
        }
        ended = server->interest_layer(StringName("ended"));
        REQUIRE(ended.is_valid());
        REQUIRE(ended->add_viewer(viewer));

        FaultWatch faults;
        faults.expect_error("*[NW0032]*", 1);
        if (server->interest_layer_set_param(
                server->interest_layer_find(StringName("ended")),
                NetwMultiplayer::LAYER_PARAM_POLICY,
                9
            )
            != ERR_INVALID_PARAMETER) {
            return law_broken("a layer took a policy that does not exist");
        }
        faults.expect_error("*belongs to a multiplayer scene*[NW1020]*", 1);
        rig.declare_scene(StringName("Arena"));
        rig.pump(4);
        rig.enter_scene(StringName("Arena"));
        const RID scene = rig.entity_of(StringName("Arena"));
        REQUIRE(server->scene_watch_peer(scene, viewer) == OK);
        const RID owned = server->scene_get_layer(scene);
        REQUIRE(owned.is_valid());
        server->interest_layer_free(owned);
        const Error removed
            = server->interest_layer_remove_viewer(owned, viewer);
        const Error added = server->interest_layer_add_viewer(owned, viewer);
        if (removed != ERR_LOCKED || added != ERR_LOCKED
            || !server->scene_subscribes(scene, viewer)
            || server->interest_layer_find(server->scene_layer_id(scene))
                   != owned) {
            return law_broken("a scene's layer changed by its RID");
        }
    }
    Node *loose = memnew(Node);
    Ref<NetwEntity> stray = NetwEntity::ensure(loose);
    const bool kept = ended->has_viewer(viewer) || ended->add_entity(stray);
    stray.unref();
    memdelete(loose);
    if (kept) {
        return law_broken("a layer of an ended session changed");
    }
    return law_held();
}

struct Dropping {
    Ref<NetwMultiplayer> session;
    ObjectID id;
    bool alive = false;
};

Dropping *dropping = nullptr;

void drop_session(int64_t) {
    dropping->session.unref();
    dropping->alive = netw::gd::instance_from_id(dropping->id) != nullptr;
}

LawVerdict dropped_in_handler(const Stand &) {
    Dropping held;
    held.session.instantiate();
    held.id = netw::gd::instance_id(held.session.ptr());
    const Ref<NetwInterestLayer> layer
        = held.session->interest_layer(StringName("dropped"));
    REQUIRE(layer.is_valid());
    layer->connect("viewer_added", callable_mp_static(&drop_session));
    dropping = &held;
    layer->add_viewer(HOST_PEER);
    dropping = nullptr;
    if (!held.alive) {
        return law_broken("the session was freed inside the layer's call");
    }
    if (netw::gd::instance_from_id(held.id) != nullptr) {
        return law_broken("the dropped session outlived the layer's call");
    }
    if (layer->has_viewer(HOST_PEER)) {
        return law_broken("the layer of a dropped session changed");
    }
    return law_held();
}

LawVerdict client_edits_layer(const Stand &) {
    FaultWatch faults;
    faults.expect_error("*interest_layer_add_entity*[NW0012]*", 1);
    faults.expect_error("*interest_layer_remove_entity*[NW0012]*", 1);
    LoopbackRig rig(1);
    NetwMultiplayer *server = rig.server();
    NetwMultiplayer *client = rig.client(0);
    const Ref<NetwEntity> entity = server->entity_get_view(
        rig.declare_entity(EntityDecl().named("Guarded").on_route(82))
    );
    const Ref<NetwInterestLayer> layer
        = server->interest_layer(StringName("sight"));
    layer->add_entity(entity);
    rig.flush_interest();
    const RID copy
        = rig.declare_mirror(0, EntityDecl().named("Guarded").on_route(82));
    const RID seen = client->interest_layer_create(StringName("sight"));

    if (client->interest_layer_add_entity(seen, copy) != ERR_UNAUTHORIZED) {
        return law_broken("a client added an entity to a layer");
    }
    if (client->interest_layer_remove_entity(seen, copy)
        != ERR_UNAUTHORIZED) {
        return law_broken("a client removed an entity from a layer");
    }
    rig.pump(4);
    if (!layer->has_entity(entity)) {
        return law_broken("the server's layer lost the entity");
    }
    return law_held();
}

int heard_from(const CallLog &p_log, const char *p_tag, int64_t p_peer) {
    int heard = 0;
    for (int at = 0; at < p_log.count(p_tag); ++at) {
        const Array call = p_log.args(p_tag, at);
        if (call.size() == 2 && StringName(call[0]) == StringName("sight")
            && int64_t(call[1]) == p_peer) {
            heard += 1;
        }
    }
    return heard;
}

enum StopSeeing {
    VIEWER_REMOVED,
    PEER_LEFT,
    LAYER_FREED,
    DESPAWNED,
};

LawVerdict observers_heard(StopSeeing p_stop) {
    LoopbackRig rig(1);
    rig.mount();
    NetwMultiplayer *server = rig.server();
    server->session_set_role(NetwMultiplayer::ROLE_LISTEN_SERVER);
    const int64_t guest = rig.peer_id(0);
    const EntityDecl decl
        = EntityDecl().named("Watched").on_route(83).mounted();
    const Ref<NetwEntity> entity
        = server->entity_get_view(rig.declare_entity(decl));
    const Ref<NetwEntity> copy
        = rig.client(0)->entity_get_view(rig.declare_mirror(0, decl));
    REQUIRE(entity.is_valid());
    REQUIRE(copy.is_valid());
    Node *body = entity->get_owner();
    Node *mirror = copy->get_owner();
    const CallLog heard;
    entity->get_interest()->connect(
        "observer_entered",
        heard.callable("server_in")
    );
    entity->get_interest()->connect(
        "observer_exited",
        heard.callable("server_out")
    );
    copy->get_interest()->connect("observer_entered", heard.callable("own_in"));
    copy->get_interest()->connect("observer_exited", heard.callable("own_out"));
    const Ref<NetwInterestLayer> layer
        = server->interest_layer(StringName("sight"));
    layer->connect("interest_exited", heard.callable("layer_out"));
    layer->add_viewer(HOST_PEER);
    layer->add_viewer(guest);
    layer->add_entity(entity);
    rig.flush_interest();
    rig.pump(4);

    if (heard.count("server_in") != 2
        || heard_from(heard, "server_in", HOST_PEER) != 1
        || heard_from(heard, "server_in", guest) != 1) {
        return law_broken(
            "the server heard %d starts, not one for each peer",
            heard.count("server_in")
        );
    }
    if (heard.count("own_in") != 1 || heard_from(heard, "own_in", guest) != 1) {
        return law_broken("the client did not hear itself start seeing once");
    }

    switch (p_stop) {
        case VIEWER_REMOVED:
            layer->remove_viewer(guest);
            break;
        case PEER_LEFT:
            rig.drop_client(0);
            rig.pump(4);
            break;
        case LAYER_FREED:
            server->interest_layer_free(
                server->interest_layer_find(StringName("sight"))
            );
            server->interest_layer(StringName("sight"))
                ->connect("interest_exited", heard.callable("reopened_out"));
            break;
        case DESPAWNED:
            entity->despawn(Ref<netw::NetwDespawnOpts>());
            body->get_parent()->remove_child(body);
            mirror->get_parent()->remove_child(mirror);
            mirror->queue_free();
            break;
    }
    rig.flush_interest();
    rig.pump(4);

    const int host_saw = p_stop == VIEWER_REMOVED || p_stop == PEER_LEFT
        ? 0
        : 1;
    if (heard.count("server_out") != host_saw + 1
        || heard_from(heard, "server_out", HOST_PEER) != host_saw
        || heard_from(heard, "server_out", guest) != 1) {
        return law_broken(
            "the server heard %d stops, %d from the host's player and %d "
            "from the client",
            heard.count("server_out"),
            heard_from(heard, "server_out", HOST_PEER),
            heard_from(heard, "server_out", guest)
        );
    }
    if (heard.count("layer_out") != host_saw + 1
        || heard.count("reopened_out") != 0) {
        return law_broken(
            "the layer reported %d stops and its reopened name %d",
            heard.count("layer_out"),
            heard.count("reopened_out")
        );
    }
    const int own_saw = p_stop == PEER_LEFT ? 0 : 1;
    if (heard.count("own_out") != own_saw
        || heard_from(heard, "own_out", guest) != own_saw) {
        return law_broken(
            "the client heard itself stop seeing %d times",
            heard.count("own_out")
        );
    }
    return law_held();
}

LawVerdict observers_when_unviewed(const Stand &) {
    return observers_heard(VIEWER_REMOVED);
}

LawVerdict observers_when_peer_left(const Stand &) {
    return observers_heard(PEER_LEFT);
}

LawVerdict observers_when_layer_freed(const Stand &) {
    return observers_heard(LAYER_FREED);
}

LawVerdict observers_when_despawned(const Stand &) {
    return observers_heard(DESPAWNED);
}

struct Nesting {
    NetwMultiplayer *server = nullptr;
    Ref<NetwInterestLayer> near;
    int64_t guest = 0;
    int heard = 0;
};

Nesting *nesting = nullptr;

void enter_near(const StringName &, int64_t p_peer) {
    if (p_peer != HOST_PEER) {
        return;
    }
    nesting->heard += 1;
    if (nesting->heard > 1) {
        return;
    }
    nesting->near->add_viewer(nesting->guest);
    nesting->server->interest_flush_now();
}

int heard_host(const CallLog &p_log, const char *p_tag) {
    int heard = 0;
    for (int at = 0; at < p_log.count(p_tag); ++at) {
        const Array call = p_log.args(p_tag, at);
        heard += call.size() == 2 && int64_t(call[1]) == HOST_PEER ? 1 : 0;
    }
    return heard;
}

LawVerdict nested_flush(const Stand &) {
    LoopbackRig rig(1);
    Nesting held;
    held.server = rig.server();
    held.server->session_set_role(NetwMultiplayer::ROLE_LISTEN_SERVER);
    held.guest = rig.peer_id(0);
    const Ref<NetwEntity> entity = held.server->entity_get_view(
        rig.declare_entity(EntityDecl().named("Nested").on_route(84))
    );
    REQUIRE(entity.is_valid());
    held.near = held.server->interest_layer(StringName("near"));
    const Ref<NetwInterestLayer> far
        = held.server->interest_layer(StringName("far"));
    const CallLog heard;
    entity->get_interest()->on_enter(
        callable_mp_static(&enter_near),
        StringName("near")
    );
    entity->get_interest()->on_enter(heard.callable("far"), StringName("far"));
    held.near->add_entity(entity);
    far->add_entity(entity);
    far->add_viewer(held.guest);
    rig.flush_interest();
    held.near->add_viewer(HOST_PEER);
    far->add_viewer(HOST_PEER);

    nesting = &held;
    rig.flush_interest();
    rig.flush_interest();
    nesting = nullptr;
    if (held.heard != 1 || heard_host(heard, "far") != 1) {
        return law_broken("a layer heard its enter for peer 1 more than once");
    }
    if (!held.server->interest_player_sees(HOST_PEER, entity)
        || !held.server->interest_player_sees(held.guest, entity)) {
        return law_broken("the flush after the handler left a peer blind");
    }
    return law_held();
}

const Row WORLD_ROWS[] = {
    {"a listen host's node that joins a layer in _init, leaves the tree and "
     "comes back",
     "the host's session holds it in that one layer each time it enters",
     &host_declares},
    {"a host's body with a speaker, hidden from the host and then shown",
     "it is not drawn or heard while hidden, and comes back at the volume "
     "it had",
     &muted_body},
    {"a hidden body whose layer's perception policy turns to show, then "
     "back",
     "it is drawn and then hidden again with no change to who sees it",
     &perception_changed},
    {"a hidden body with its own perception action, hidden and then shown",
     "the action runs on both edges with the peer and the layer, and the "
     "body is not hidden for it",
     &custom_perception},
    {"an owner asking who shares its arena, beside a car in the lobby",
     "it names the other car, as the server does",
     &owner_shares},
    {"an owner whose own scene layer admits its car, asking who shares the "
     "arena",
     "the car still resolves the arena and finds the other car",
     &owner_in_scene},
    {"a layer driver answering a number, then another session's entity, "
     "then its own, then one freeing an earlier layer and its own",
     "the first two stop the flush and leave the layer as it was, the "
     "third joins, the layer neither takes nor reports another session's "
     "entity, and the driver after the freed layers still runs",
     &driver_answers},
    {"a layer kept after its free, layers freed or refilled by their own "
     "handlers, a policy that does not exist, and a layer kept after its "
     "session ends",
     "none of them changes anything and no freed layer comes back",
     &kept_view},
    {"a layer whose viewer_added handler drops the last reference to its "
     "session",
     "the session lives until the call returns and then ends",
     &dropped_in_handler},
    {"a client adding and removing an entity in its copy of a layer",
     "both are refused and reported, and the server's layer keeps it",
     &client_edits_layer},
    {"a listen host's entity a layer shows to the host's player and a "
     "client, then the layer losing the client as a viewer",
     "the server hears each peer start seeing it once, the host's player as "
     "peer 1, the client hears only itself, and both hear the client stop "
     "seeing it once",
     &observers_when_unviewed},
    {"the same entity's client disconnecting",
     "the server hears the client stop seeing it once, and the client's "
     "ended session reports nothing",
     &observers_when_peer_left},
    {"the same entity's layer freed and its name opened again",
     "the server hears both peers stop seeing it once, the client hears "
     "itself, and the new layer reports none of it",
     &observers_when_layer_freed},
    {"the same entity despawned",
     "the server hears both peers stop seeing it once, and the client hears "
     "itself",
     &observers_when_despawned},
    {"a listen host's entity in two layers whose enter handler adds a viewer "
     "and flushes",
     "each layer tells the host's player once, and both peers see it after "
     "the next flush",
     &nested_flush},
};

TEST_CASE(
    "[Networked][Interest][SceneTree] a peer sees what its layers grant, a "
    "hidden body comes back as it was, and only the server changes a layer"
) {
    const Stand stand = {String("a server and a client on the loopback rig")};
    for (const Row &row : WORLD_ROWS) {
        NETW_CELL(row, stand);
        NETW_LAW_HOLDS(row, stand);
    }
}

} // namespace TestInterest

#endif
