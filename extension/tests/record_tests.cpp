#include "support/netw_test.h"

#include "support/netw_cells.h"

#include "godot/object.hpp"
#include "netw/api/record.hpp"

#if defined(NETW_TIER_HOSTED)
#include "support/internals.h"
#include "support/loopback_rig.h"
#include "support/minted_script.h"

#include "godot/node.hpp"
#include "netw/api/context.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/netw_multiplayer.hpp"
#endif

namespace TestRecord {

using namespace godot;
using netw::DictionaryRecord;
using netw_test::law_broken;
using netw_test::law_held;
using netw_test::LawRowFor;
using netw_test::LawVerdict;

struct World {
    String label;
};

typedef LawRowFor<World> Row;

Ref<DictionaryRecord> fresh() {
    Ref<DictionaryRecord> record;
    record.instantiate();
    return record;
}

LawVerdict stored_names_only(const World &) {
    const Ref<DictionaryRecord> record = fresh();
    record->set("health", 75);
    if (!record->has_value("health") || int(record->get("health")) != 75) {
        return law_broken("health did not read back as 75");
    }
    record->set("resource_name", "named");
    if (record->has_value("resource_name")
        || String(record->get_name()) != String("named")) {
        return law_broken("resource_name entered the record");
    }
    Dictionary values;
    values["a"] = 1;
    record->set("data", values);
    if (record->has_value("data") || !record->has_value("a")
        || record->get_data().size() != 1) {
        return law_broken("data did not replace what the record stores");
    }
    return law_held();
}

LawVerdict fallback_and_null(const World &) {
    const Ref<DictionaryRecord> record = fresh();
    if (record->has_value("missing")
        || int(record->get_value("missing", 42)) != 42) {
        return law_broken("an absent name did not answer its fallback");
    }
    record->set_value("missing", Variant());
    if (!record->has_value("missing")
        || record->get_value("missing", 42).get_type() != Variant::NIL) {
        return law_broken("the fallback masked a stored null");
    }
    return law_held();
}

LawVerdict bytes_round_trip(const World &) {
    const Ref<DictionaryRecord> record = fresh();
    record->set_value("health", 75);
    record->set_value("place", Vector2(1, 2));
    const Ref<DictionaryRecord> copy = fresh();
    copy->deserialize(record->serialize());
    if (copy->to_dict().size() != 2
        || copy->get_value("health").get_type() != Variant::INT
        || int(copy->get_value("health")) != 75
        || copy->get_value("place").get_type() != Variant::VECTOR2
        || !Vector2(copy->get_value("place")).is_equal_approx(Vector2(1, 2))) {
        return law_broken("the rebuilt record is %s", String(Variant(
            copy->to_dict()
        )).utf8().get_data());
    }
    return law_held();
}

const Row RECORD_ROWS[] = {
    {"a record set health, then resource_name, then data",
     "health is stored, resource_name names the Resource, and data replaces "
     "what is stored",
     &stored_names_only},
    {"a record asked for a name it lacks, then for a stored null",
     "the first answers the fallback and the second answers null",
     &fallback_and_null},
    {"a record rebuilt from the bytes of another",
     "it holds the same values with the same types",
     &bytes_round_trip},
};

TEST_CASE(
    "[Networked][Record][Hosted] a DictionaryRecord stores what a game sets, "
    "tells a stored null from an absent name, and survives its own bytes"
) {
    const World world = {String("a bare DictionaryRecord")};
    for (const Row &row : RECORD_ROWS) {
        NETW_CELL(row, world);
        NETW_LAW_HOLDS(row, world);
    }
}

#if defined(NETW_TIER_HOSTED)

using netw::Netw;
using netw::NetwEntity;
using netw_test::Internals;
using netw_test::LoopbackRig;
using netw_test::WorldDecl;

constexpr const char *MOVER = R"(extends Node

var offset := 0.0

func _init() -> void:
	Netw.configure_property(self, &"offset").state()

func move(tick: int) -> void:
	offset = float(tick)
)";

constexpr int TICKS = 10;
constexpr int64_t AGO = 5;
constexpr int OUTLIVED = 100;

Script *minted = nullptr;

Node *build_minted(const Variant &p_name) {
    Node *made = Object::cast_to<Node>(minted->call("new"));
    made->set_name(String(p_name));
    return made;
}

struct Range {
    Ref<Script> script;
    LoopbackRig rig;
    int route = -1;
    Node *body = nullptr;
    Ref<NetwEntity> entity;
    int64_t born = -1;

    explicit Range(int p_ticks)
        : script(netw_test::minted_script(MOVER)), rig(1) {
        REQUIRE(script.is_valid());
        rig.mount();
        rig.declare_world(WorldDecl().clocked(30, 0).lag_compensated());
        minted = script.ptr();
        Array args;
        args.push_back(String("Body"));
        Array types;
        types.push_back(int(Variant::STRING));
        route = rig.spawn_registered(
            StringName("recorded"),
            callable_mp_static(&build_minted),
            args,
            types
        );
        minted = nullptr;
        body = rig.route_node(route);
        REQUIRE(body != nullptr);
        entity = NetwEntity::of(body);
        REQUIRE(entity.is_valid());
        born = rig.server()->clock_get_tick();
        for (int tick = 1; tick <= p_ticks; ++tick) {
            body->call(StringName("move"), tick);
            rig.step_ticks(1);
        }
    }

    int64_t now() const {
        return rig.server()->clock_get_tick();
    }

    int timelines() const {
        const Dictionary metrics = rig.server()->lagcomp_metrics();
        return int(metrics.get(StringName("timelines"), -1));
    }
};

LawVerdict reads_the_past(const World &) {
    Range range(TICKS);
    const Ref<DictionaryRecord> sampled
        = Netw::sample(range.entity, range.now() - AGO);
    const Ref<DictionaryRecord> next
        = Netw::sample(range.entity, range.now() - AGO + 1);
    if (sampled.is_null() || !sampled->has_value(StringName("offset"))
        || next.is_null() || !next->has_value(StringName("offset"))) {
        return law_broken("the samples hold no offset");
    }
    const double offset = double(sampled->get_value(StringName("offset")));
    const double later = double(next->get_value(StringName("offset")));
    if (offset <= 0.0 || offset >= double(TICKS)
        || Math::abs(later - offset - 1.0) > 0.0001) {
        return law_broken(
            "the samples read %f and %f where the body is at %d",
            offset,
            later,
            TICKS
        );
    }
    return law_held();
}

LawVerdict client_answers_nothing(const World &) {
    Range range(TICKS);
    Node *mirror = range.rig.route_node(range.route, 0);
    if (mirror == nullptr) {
        return law_broken("the client holds no copy of the body");
    }
    const Ref<NetwEntity> seen = NetwEntity::of(mirror);
    if (seen.is_null()) {
        return law_broken("the client's copy has no entity");
    }
    const Ref<DictionaryRecord> sampled
        = Netw::sample(seen, range.now() - AGO);
    if (sampled.is_valid() && !sampled->is_empty()) {
        return law_broken(
            "the client answered %s",
            String(Variant(sampled->to_dict())).utf8().get_data()
        );
    }
    return law_held();
}

LawVerdict outlived_tick_answers_nothing(const World &) {
    Range range(OUTLIVED);
    const Ref<DictionaryRecord> sampled
        = Netw::sample(range.entity, range.born + 1);
    if (sampled.is_valid() && !sampled->is_empty()) {
        return law_broken(
            "a dropped tick answered %s",
            String(Variant(sampled->to_dict())).utf8().get_data()
        );
    }
    const Ref<DictionaryRecord> recent
        = Netw::sample(range.entity, range.now() - AGO);
    if (recent.is_null() || recent->is_empty()) {
        return law_broken("the recent past answered nothing either");
    }
    return law_held();
}

LawVerdict undeclared_answers_nothing(const World &) {
    Range range(TICKS);
    if (range.timelines() != 1) {
        return law_broken("%d timeline(s) before", range.timelines());
    }
    range.rig.server()->lagcomp_timeline_undeclare(
        Internals::entity_of(range.rig.server(), range.body)
    );
    range.rig.step_ticks(1);
    const Ref<DictionaryRecord> sampled
        = Netw::sample(range.entity, range.now() - AGO);
    if (sampled.is_valid() && !sampled->is_empty()) {
        return law_broken("an undeclared body still answered a sample");
    }
    if (range.timelines() != 0) {
        return law_broken("%d timeline(s) after", range.timelines());
    }
    return law_held();
}

const Row SAMPLE_ROWS[] = {
    {"Netw.sample of a tick five ticks ago, on the server",
     "it holds the state property as it was, one tick apart from the next",
     &reads_the_past},
    {"Netw.sample on a client",
     "it answers an empty record, since only the server records history",
     &client_answers_nothing},
    {"Netw.sample of a tick a hundred ticks ago",
     "it answers an empty record rather than the oldest one kept",
     &outlived_tick_answers_nothing},
    {"Netw.sample after lagcomp_timeline_undeclare",
     "it answers an empty record, and lagcomp_metrics counts no timeline",
     &undeclared_answers_nothing},
};

TEST_CASE(
    "[Networked][Record][SceneTree] Netw.sample answers a state property as "
    "it was on the server, and an empty record where no history is kept"
) {
    const World world = {String("a server with lag compensation and a client")};
    for (const Row &row : SAMPLE_ROWS) {
        NETW_CELL(row, world);
        NETW_LAW_HOLDS(row, world);
    }
}

#endif

} // namespace TestRecord
