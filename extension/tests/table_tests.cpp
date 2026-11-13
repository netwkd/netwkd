#include "support/netw_test.h"

#if defined(NETW_TIER_HOSTED)

#include "support/internals.h"
#include "support/link.h"
#include "support/loopback_rig.h"
#include "support/netw_cells.h"
#include "support/netw_recorder.h"

#include "godot/node.hpp"
#include "netw/api/context.hpp"
#include "netw/api/database.hpp"
#include "netw/api/database_backend.hpp"
#include "netw/api/database_config.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/promise.hpp"
#include "netw/api/schema_model.hpp"
#include "netw/api/table.hpp"
#include "netw/persist/memory_store.hpp"
#include "netw/schema_model.hpp"

namespace TestTable {

using namespace godot;
using netw::Netw;
using netw::NetwDatabase;
using netw::NetwDatabaseBackend;
using netw::NetwLinkConditions;
using netw::NetwMultiplayer;
using netw::NetwPromise;
using netw::NetwSchema;
using netw::NetwSession;
using netw::NetwTable;
using netw::persist::MemoryConnection;
using netw_test::Internals;
using netw_test::law_broken;
using netw_test::law_held;
using netw_test::LawVerdict;
using netw_test::LoopbackRig;
using netw_test::Recorder;

class HeldBackend : public NetwDatabaseBackend {
public:
    Ref<MemoryConnection> connection;

    explicit HeldBackend(const char *p_store) {
        connection = MemoryConnection::opened(p_store, "slot1");
    }

    Ref<NetwPromise> open_builtin(
        const Ref<NetwSession> &,
        const StringName &
    ) override {
        return NetwPromise::resolved(connection);
    }
};

struct Declared {
    Declared() {
        netw::schema_model::clear();
        netw::persist::forget_stores();
        const Ref<netw::NetwQuantize> none;
        const Ref<NetwSchema> schema = NetwSchema::declare("TableMob");
        schema->vector3("pos", none, 1);
        schema->i32("hp", 1);
    }

    ~Declared() {
        netw::schema_model::clear();
        netw::persist::forget_stores();
    }
};

struct Stage {
    Declared declared;
    LoopbackRig rig;
    Ref<NetwTable> host;
    Ref<NetwTable> guest;

    explicit Stage(int p_clients = 1) : rig(p_clients) {
        rig.mount();
        host = NetwTable::of(rig.branch(-1), "TableMob");
        guest = NetwTable::of(rig.branch(0), "TableMob");
        REQUIRE(host.is_valid());
        REQUIRE(guest.is_valid());
    }

    Ref<NetwTable> at(int p_client) const {
        return NetwTable::of(rig.branch(p_client), "TableMob");
    }

    PackedInt64Array claim(int p_count) const {
        return rig.server()->table_claim_routes(host->get_rid(), p_count);
    }

    Ref<NetwDatabase> database(int p_client, const Ref<HeldBackend> &p_with) {
        Node *author = memnew(Node);
        author->set_name(StringName("Saves"));
        Netw::configure_database(author, "saves")->backend(p_with);
        rig.branch(p_client)->add_child(author);
        const Ref<NetwDatabase> db = NetwDatabase::of(author, "saves");
        REQUIRE(db.is_valid());
        db->open("slot1");
        rig.pump(2);
        return db;
    }
};

struct World {
    String label;
};

typedef netw_test::LawRowFor<World> Row;

PackedInt64Array int64s(const std::initializer_list<int64_t> &p_values) {
    PackedInt64Array out;
    for (const int64_t value : p_values) {
        out.push_back(value);
    }
    return out;
}

PackedInt32Array ranged(int p_count, int p_first) {
    PackedInt32Array out;
    for (int at = 0; at < p_count; ++at) {
        out.push_back(p_first + at);
    }
    return out;
}

PackedVector3Array along_x(int p_count, float p_offset) {
    PackedVector3Array out;
    for (int at = 0; at < p_count; ++at) {
        out.push_back(Vector3(float(at) + p_offset, 0.0f, 0.0f));
    }
    return out;
}

Error publish(
    const Ref<NetwTable> &p_table,
    const PackedInt64Array &p_routes,
    int p_hp,
    float p_offset = 0.0f
) {
    p_table->write_routes(p_routes);
    p_table->write_column(0, along_x(p_routes.size(), p_offset));
    p_table->write_column(1, ranged(p_routes.size(), p_hp));
    return p_table->commit();
}

int hp_at(const Ref<NetwTable> &p_table, int64_t p_route) {
    const int row = p_table->row_of(p_route);
    const PackedInt32Array hp = p_table->read_column(1);
    return row >= 0 && row < hp.size() ? hp[row] : -1;
}

LawVerdict holds_wave(
    const Ref<NetwTable> &p_table,
    const PackedInt64Array &p_routes,
    int p_hp
) {
    if (p_table->read_routes().size() != p_routes.size()) {
        return law_broken(
            "it holds %d rows of %d",
            int(p_table->read_routes().size()),
            int(p_routes.size())
        );
    }
    for (int at = 0; at < p_routes.size(); ++at) {
        if (hp_at(p_table, p_routes[at]) != p_hp + at) {
            return law_broken(
                "route %d reads hp %d, not %d",
                int(p_routes[at]),
                hp_at(p_table, p_routes[at]),
                p_hp + at
            );
        }
    }
    return law_held();
}

int settled(const Ref<NetwPromise> &p_promise) {
    if (p_promise.is_null() || !p_promise->get_is_settled()) {
        return -1;
    }
    if (p_promise->get_is_failed()) {
        return int(p_promise->get_code());
    }
    return int(p_promise->get_result());
}

Ref<NetwLinkConditions> shuffling() {
    Ref<NetwLinkConditions> conditions = netw_test::link(11);
    conditions->set_latency_ms(40.0);
    conditions->set_jitter_ms(40.0);
    conditions->set_reorder_chance(0.5);
    return conditions;
}

LawVerdict wave_without_nodes(const World &) {
    Stage stage;
    const PackedInt64Array routes = stage.claim(3);
    if (publish(stage.host, routes, 100) != OK) {
        return law_broken("the server's commit was refused");
    }
    stage.rig.step_ticks(3);

    const LawVerdict seen = holds_wave(stage.guest, routes, 100);
    if (!seen.held) {
        return seen;
    }
    NetwMultiplayer *guest = stage.rig.client(0);
    const RID entity = Internals::entity_at(guest, routes[0]);
    if (!entity.is_valid() || guest->entity_get_node(entity) != nullptr) {
        return law_broken("the client did not hold a nodeless entity");
    }
    return law_held();
}

LawVerdict received_per_wave(const World &) {
    Stage stage;
    const Recorder host_heard(stage.host.ptr(), {"received"});
    const Recorder guest_heard(stage.guest.ptr(), {"received"});
    if (stage.at(0) != stage.guest) {
        return law_broken("a second lookup answered another table");
    }

    const PackedInt64Array first = stage.claim(2);
    publish(stage.host, first, 1);
    stage.rig.step_ticks(3);
    if (host_heard.count("received") != 1
        || guest_heard.count("received") != 1) {
        return law_broken(
            "the first wave was heard %d times by the server, %d by the client",
            host_heard.count("received"),
            guest_heard.count("received")
        );
    }
    if (stage.host->read_births().size() != 2
        || stage.guest->read_births().size() != 2) {
        return law_broken("the first wave did not name two births");
    }

    const PackedInt64Array second = stage.claim(1);
    publish(stage.host, int64s({first[0], second[0]}), 3);
    stage.rig.step_ticks(3);
    if (guest_heard.count("received") != 2) {
        return law_broken(
            "the client heard %d emissions over two waves",
            guest_heard.count("received")
        );
    }
    const PackedInt64Array born = stage.guest->read_births();
    const PackedInt64Array died = stage.guest->read_deaths();
    if (born.size() != 1 || born[0] != second[0] || died.size() != 1
        || died[0] != first[1]) {
        return law_broken("the second wave named the wrong births or deaths");
    }
    if (hp_at(stage.host, first[0]) != 3) {
        return law_broken("the server did not read its own wave");
    }
    return law_held();
}

LawVerdict second_wave_moves(const World &) {
    Stage stage;
    const PackedInt64Array routes = stage.claim(2);
    publish(stage.host, routes, 100, 1.0f);
    stage.rig.step_ticks(3);
    publish(stage.host, routes, 80, 9.0f);
    stage.rig.step_ticks(3);

    const LawVerdict seen = holds_wave(stage.guest, routes, 80);
    if (!seen.held) {
        return seen;
    }
    const PackedVector3Array pos = stage.guest->read_column(0);
    const int row = stage.guest->row_of(routes[1]);
    if (row < 0 || pos[row] != Vector3(10.0f, 0.0f, 0.0f)) {
        return law_broken("the second wave did not move the rows");
    }
    return law_held();
}

LawVerdict large_wave_shuffled(const World &) {
    Stage stage;
    stage.rig.conditions(0, shuffling());
    const PackedInt64Array routes = stage.claim(300);
    publish(stage.host, routes, 0);
    stage.rig.step_ticks(30);
    return holds_wave(stage.guest, routes, 0);
}

LawVerdict large_table_late_joiner(const World &) {
    Stage stage;
    const PackedInt64Array routes = stage.claim(300);
    publish(stage.host, routes, 7);
    stage.rig.step_ticks(3);

    const int late = stage.rig.late_client("Late");
    stage.rig.conditions(late, shuffling());
    stage.rig.pump(4);
    stage.rig.step_ticks(30);
    return holds_wave(stage.at(late), routes, 7);
}

LawVerdict client_publishes_nothing(const World &) {
    Stage stage(2);
    if (publish(stage.guest, int64s({901, 902}), 7) != OK) {
        return law_held();
    }
    stage.rig.step_ticks(6);
    if (stage.host->read_routes().size() != 0) {
        return law_broken("the server took a client's rows");
    }
    if (stage.at(1)->read_routes().size() != 0) {
        return law_broken("another client took a client's rows");
    }
    return law_held();
}

LawVerdict save_and_load(const World &) {
    Stage stage;
    const Ref<HeldBackend> backend(memnew(HeldBackend("table_tests_load")));
    const Ref<NetwDatabase> db = stage.database(-1, backend);
    const PackedInt64Array written = stage.claim(3);
    publish(stage.host, written, 100);
    PackedStringArray ids;
    ids.push_back("wolf");
    ids.push_back("bear");
    ids.push_back("deer");
    const Ref<NetwPromise> saving = stage.host->save(db, "forest", ids);
    stage.rig.pump(2);
    if (settled(saving) != OK) {
        return law_broken("the save answered %d", settled(saving));
    }

    publish(stage.host, stage.claim(1), 5);
    const Ref<NetwPromise> loading = stage.host->load(db, "forest");
    stage.rig.pump(2);
    if (loading.is_null() || !loading->get_is_settled()) {
        return law_broken("the load never settled");
    }
    const Dictionary loaded = loading->get_result();
    const PackedStringArray back = loaded.get("ids", PackedStringArray());
    const PackedInt64Array routes = loaded.get("routes", PackedInt64Array());
    if (int(loaded.get("error", -1)) != OK || !bool(loaded.get("found", false))
        || back != ids || routes.size() != 3) {
        return law_broken("the load did not answer the three saved ids");
    }
    for (int at = 0; at < routes.size(); ++at) {
        if (written.has(routes[at])) {
            return law_broken("a loaded row kept the route it was saved from");
        }
    }
    const LawVerdict here = holds_wave(stage.host, routes, 100);
    if (!here.held) {
        return here;
    }
    stage.rig.step_ticks(3);
    return holds_wave(stage.guest, routes, 100);
}

LawVerdict commit_during_load(const World &) {
    Stage stage;
    const Ref<HeldBackend> backend(memnew(HeldBackend("table_tests_race")));
    const Ref<NetwDatabase> db = stage.database(-1, backend);
    publish(stage.host, stage.claim(3), 100);
    PackedStringArray ids;
    ids.push_back("a");
    ids.push_back("b");
    ids.push_back("c");
    stage.host->save(db, "forest", ids);
    stage.rig.pump(2);

    backend->connection->defer(true);
    const Ref<NetwPromise> loading = stage.host->load(db, "forest");
    const PackedInt64Array standing = stage.claim(1);
    publish(stage.host, standing, 7);
    backend->connection->release();
    stage.rig.pump(2);

    if (loading.is_null() || !loading->get_is_settled()) {
        return law_broken("the load never settled");
    }
    const Dictionary refused = loading->get_result();
    if (int(refused.get("error", -1)) != ERR_BUSY) {
        return law_broken(
            "the load answered %d over a newer commit",
            int(refused.get("error", -1))
        );
    }
    return holds_wave(stage.host, standing, 7);
}

LawVerdict load_missing_and_empty(const World &) {
    Stage stage;
    const Ref<HeldBackend> backend(memnew(HeldBackend("table_tests_empty")));
    const Ref<NetwDatabase> db = stage.database(-1, backend);
    stage.host->save(db, "barren", PackedStringArray());
    stage.rig.pump(2);
    const PackedInt64Array routes = stage.claim(2);
    publish(stage.host, routes, 100);

    const Ref<NetwPromise> missing = stage.host->load(db, "nowhere");
    stage.rig.pump(2);
    const Dictionary missed = missing->get_result();
    if (int(missed.get("error", -1)) != OK || bool(missed.get("found", true))) {
        return law_broken("a key nobody saved did not answer found false");
    }
    const LawVerdict untouched = holds_wave(stage.host, routes, 100);
    if (!untouched.held) {
        return untouched;
    }

    const Ref<NetwPromise> empty = stage.host->load(db, "barren");
    stage.rig.pump(2);
    if (!bool(Dictionary(empty->get_result()).get("found", false))) {
        return law_broken("a save of zero rows was not stored");
    }
    stage.rig.step_ticks(3);
    if (stage.host->read_routes().size() != 0
        || stage.guest->read_routes().size() != 0) {
        return law_broken("loading zero rows did not empty the table");
    }
    return law_held();
}

LawVerdict save_bad_ids(const World &) {
    Stage stage;
    const Ref<HeldBackend> backend(memnew(HeldBackend("table_tests_ids")));
    const Ref<NetwDatabase> db = stage.database(-1, backend);
    publish(stage.host, stage.claim(2), 100);

    PackedStringArray repeated;
    repeated.push_back("a");
    repeated.push_back("a");
    PackedStringArray blank;
    blank.push_back("a");
    blank.push_back("");
    PackedStringArray three;
    three.push_back("a");
    three.push_back("b");
    three.push_back("c");
    const PackedStringArray tries[] = {repeated, blank, three};
    for (const PackedStringArray &ids : tries) {
        const Ref<NetwPromise> saving = stage.host->save(db, "forest", ids);
        stage.rig.pump(2);
        if (settled(saving) != ERR_INVALID_DATA) {
            return law_broken(
                "a save of %d ids answered %d",
                int(ids.size()),
                settled(saving)
            );
        }
    }
    const Ref<NetwPromise> loading = stage.host->load(db, "forest");
    stage.rig.pump(2);
    if (bool(Dictionary(loading->get_result()).get("found", true))) {
        return law_broken("a refused save stored a snapshot");
    }
    return law_held();
}

LawVerdict client_saves_nothing(const World &) {
    Stage stage;
    const Ref<HeldBackend> backend(memnew(HeldBackend("table_tests_client")));
    const Ref<NetwDatabase> db = stage.database(0, backend);
    const PackedInt64Array routes = stage.claim(2);
    publish(stage.host, routes, 100);
    stage.rig.step_ticks(3);

    PackedStringArray ids;
    ids.push_back("a");
    ids.push_back("b");
    const Ref<NetwPromise> saving = stage.guest->save(db, "forest", ids);
    const Ref<NetwPromise> loading = stage.guest->load(db, "forest");
    stage.rig.pump(2);
    if (settled(saving) != ERR_UNAUTHORIZED) {
        return law_broken("a client's save answered %d", settled(saving));
    }
    const Dictionary refused = loading->get_result();
    if (int(refused.get("error", -1)) != ERR_UNAUTHORIZED) {
        return law_broken(
            "a client's load answered %d",
            int(refused.get("error", -1))
        );
    }
    return holds_wave(stage.guest, routes, 100);
}

const Row ROWS[] = {
    {"a wave of three rows committed at the server",
     "the client holds the rows and their values with no node for any",
     &wave_without_nodes},
    {"two waves, the second keeping one route and adding one",
     "every lookup answers the same table, server and client each hear one "
     "received a wave, and the client reads one birth and one death for "
     "the second",
     &received_per_wave},
    {"a second wave over the routes a client already holds",
     "the client's rows move to the second wave's values",
     &second_wave_moves},
    {"a wave of 300 rows over a link that reorders packets",
     "the client holds every row with its value",
     &large_wave_shuffled},
    {"a client joining after a 300 row wave, over a link that reorders",
     "the late client holds every row",
     &large_table_late_joiner},
    {"a client that adopts the table and commits rows",
     "neither the server nor another client takes them",
     &client_publishes_nothing},
    {"a save of three rows, a newer commit, then a load",
     "the load answers the saved ids at new routes with the saved values, "
     "and the client follows",
     &save_and_load},
    {"a commit made while a load is still reading",
     "the load answers ERR_BUSY and the committed rows stand",
     &commit_during_load},
    {"a load of a key nobody saved, then of a save of zero rows",
     "the first leaves the table, the second empties it on every peer",
     &load_missing_and_empty},
    {"a save whose ids repeat, are empty or do not match the rows",
     "each answers ERR_INVALID_DATA and nothing is stored",
     &save_bad_ids},
    {"a client saving and loading the table",
     "both answer ERR_UNAUTHORIZED and the client's rows stand",
     &client_saves_nothing},
};

TEST_CASE(
    "[Networked][Table][SceneTree] a table the server commits reaches every "
    "client whole and in order, and a save loads back as it was"
) {
    const World world = {String("a server and clients on a loopback")};
    for (const Row &row : ROWS) {
        NETW_CELL(row, world);
        NETW_LAW_HOLDS(row, world);
    }
}

} // namespace TestTable

#endif
