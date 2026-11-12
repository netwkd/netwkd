#include "support/netw_test.h"

#if defined(NETW_TIER_HOSTED)

#include "support/fault_log.h"
#include "support/loopback_rig.h"
#include "support/minted_script.h"
#include "support/netw_call_log.h"
#include "support/netw_cells.h"
#include "support/value_flow_stand.h"

#include "godot/node.hpp"
#include "godot/script.hpp"
#include "godot/spatial_node.hpp"
#include "netw/api/context.hpp"
#include "netw/api/database.hpp"
#include "netw/api/database_backend.hpp"
#include "netw/api/database_config.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/participant.hpp"
#include "netw/api/persistence.hpp"
#include "netw/api/persistence_config.hpp"
#include "netw/api/promise.hpp"
#include "netw/api/schema_model.hpp"
#include "netw/api/session.hpp"
#include "netw/persist/memory_store.hpp"
#include "netw/schema_model.hpp"

namespace TestPersistence {

using namespace godot;
using netw::Netw;
using netw::NetwDatabase;
using netw::NetwDatabaseBackend;
using netw::NetwEntity;
using netw::NetwMultiplayer;
using netw::NetwPersistence;
using netw::NetwPromise;
using netw::NetwSession;
using netw::persist::MemoryConnection;
using netw_test::CallLog;
using netw_test::FaultWatch;
using netw_test::law_broken;
using netw_test::law_held;
using netw_test::LawVerdict;
using netw_test::LoopbackRig;

const char *SAVED_SOURCE = R"(extends Node2D

var seen := []
var where := Vector2(1, 1):
	set(value):
		where = value
		seen.append(value)


func _init() -> void:
	Netw.configure_property(self, &"where").on_spawn().persisted(&"where")
	Netw.configure_persistence(self).database(&"saves").table(&"players") \
			.record_id(account)


func account() -> StringName:
	return NetwEntity.of(self).name
)";

const Vector2 START(1, 1);
const Vector2 STORED(9, 9);
const Vector2 MOVED(5, 5);

class HeldBackend : public NetwDatabaseBackend {
public:
    Ref<MemoryConnection> connection;

    explicit HeldBackend(const String &p_store) {
        connection = MemoryConnection::opened(p_store, "slot1");
    }

    Ref<NetwPromise> open_builtin(Object *, const StringName &) override {
        return NetwPromise::resolved(connection);
    }
};

Node *build_saved(
    const Variant &p_name,
    const Variant &p_script,
    const Variant &p_loads_at_spawn
) {
    const Ref<Script> script = p_script;
    Node *made
        = Object::cast_to<Node>(netw::gd::live_object(script->call("new")));
    made->set_name(String(p_name));
    Netw::configure_persistence(made)->load_on_spawn(bool(p_loads_at_spawn));
    return made;
}

struct Cleared {
    Cleared() {
        netw::schema_model::clear();
        netw::persist::forget_stores();
    }

    ~Cleared() {
        netw::schema_model::clear();
        netw::persist::forget_stores();
    }
};

struct Stand {
    Cleared cleared;
    LoopbackRig rig;
    NetwMultiplayer *server = nullptr;
    Ref<Script> script;
    Ref<HeldBackend> backend;
    Ref<NetwDatabase> db;
    CallLog log;
    Node2D *body = nullptr;
    ObjectID body_id;
    Ref<netw::NetwSchema> schema;
    int route = 0;

    Stand(const char *p_store, bool p_loads_at_spawn = true) : rig(0) {
        rig.add_client(StringName("hero"));
        rig.mount();
        netw_test::flow_clocks(rig, 30);
        server = rig.server();
        script = netw_test::minted_script(SAVED_SOURCE);
        REQUIRE(script.is_valid());

        backend = Ref<HeldBackend>(memnew(HeldBackend(p_store)));
        db = declare_database(-1, backend);
        db->connect("failed", log.callable("failed"));
        declare_database(
            0,
            Ref<HeldBackend>(memnew(HeldBackend(String(p_store) + "_client")))
        );

        const Callable build
            = callable_mp_static(&build_saved).bind(script, p_loads_at_spawn);
        Array types;
        types.push_back(int(Variant::STRING));
        rig.register_constructor(server, "saved", build, types);
        rig.register_constructor(rig.client(0), "saved", build, types);
        rig.step_ticks(4);

        Array named;
        named.push_back(String("Body"));
        const RID entity = server->spawn_registered(
            StringName("saved"),
            named,
            rig.player(0).ptr()
        );
        body = Object::cast_to<Node2D>(server->entity_get_node(entity));
        REQUIRE(body != nullptr);
        body_id = netw::gd::instance_id(body);
        route = int(NetwEntity::of(body)->get_route());
        saved()->connect("saved", log.callable("saved"));
        saved()->connect("loaded", log.callable("loaded"));
        schema = saved()->get_schema();
        REQUIRE(schema.is_valid());
    }

    ~Stand() {
        Node *left = Object::cast_to<Node>(netw::gd::live_object(body_id));
        if (left != nullptr && left->get_parent() == nullptr) {
            memdelete(left);
        }
    }

    Ref<NetwDatabase> declare_database(
        int p_side,
        const Ref<HeldBackend> &p_backend
    ) {
        Node *saves = memnew(Node);
        saves->set_name(StringName("Saves"));
        Netw::configure_database(saves, "saves")->backend(p_backend);
        rig.branch(p_side)->add_child(saves);
        const Ref<NetwDatabase> made = NetwDatabase::of(saves, "saves");
        REQUIRE(made.is_valid());
        made->open("slot1");
        return made;
    }

    Ref<NetwPersistence> saved() const {
        return NetwEntity::of(body)->get_persistence();
    }

    void store(const Vector2 &p_where) {
        Dictionary row;
        row["where"] = p_where;
        const Ref<NetwPromise> wrote = db->write(schema, "hero", row);
        rig.pump(2);
        REQUIRE(wrote->get_is_completed());
    }

    Vector2 stored() {
        const Ref<NetwPromise> read = db->read(schema, "hero");
        rig.pump(2);
        const Dictionary row = read->get_result();
        if (!bool(row.get("found", false))) {
            return Vector2(-1, -1);
        }
        return Vector2(Dictionary(row["values"])["where"]);
    }

    void mount() {
        rig.branch(-1)->add_child(body);
        rig.step_ticks(4);
    }

    void free_body() {
        memdelete(body);
        body = nullptr;
        rig.step_ticks(4);
    }

    Node *view() const {
        return rig.route_node(route, 0);
    }

    Ref<NetwSession> session() const {
        return Netw::session(rig.branch(-1));
    }

    int failed_code() const {
        const Array carried = log.args("failed");
        return carried.is_empty() ? -1 : int(carried[0]);
    }
};

int answer(const Ref<NetwPromise> &p_promise) {
    if (p_promise.is_null() || !p_promise->get_is_settled()) {
        return -1;
    }
    if (p_promise->get_is_failed()) {
        return int(p_promise->get_code());
    }
    return int(p_promise->get_result());
}

struct World {
    String label;
};

typedef netw_test::LawRowFor<World> Row;

LawVerdict saved_reads_back(const World &) {
    Stand stand("persistence_tests_saved");
    stand.mount();
    stand.body->set("where", MOVED);

    const Ref<NetwPromise> saving = stand.saved()->save();
    stand.rig.pump(2);
    if (answer(saving) != 1 || stand.log.count("saved") != 1) {
        return law_broken("the save answered %d", answer(saving));
    }
    if (stand.saved()->get_dirty() || stand.stored() != MOVED) {
        return law_broken("the saved values did not read back");
    }
    const Ref<NetwPromise> again = stand.saved()->save();
    stand.rig.pump(2);
    if (answer(again) != 0) {
        return law_broken("a save with nothing changed answered true");
    }
    return law_held();
}

LawVerdict refused_write(const World &) {
    const FaultWatch faults;
    Stand stand("persistence_tests_refused");
    stand.mount();
    stand.body->set("where", MOVED);
    stand.backend->connection->fail_next(ERR_FILE_CANT_WRITE);

    const Ref<NetwPromise> refused = stand.saved()->save();
    stand.rig.pump(2);
    if (answer(refused) != ERR_FILE_CANT_WRITE) {
        return law_broken("the refused save answered %d", answer(refused));
    }
    if (stand.log.count("failed") != 1
        || stand.failed_code() != ERR_FILE_CANT_WRITE
        || stand.log.count("saved") != 0) {
        return law_broken(
            "the database reported %d failures and %d saves",
            stand.log.count("failed"),
            stand.log.count("saved")
        );
    }
    if (!stand.saved()->get_dirty()) {
        return law_broken("a refused write became the saved values");
    }
    stand.saved()->save();
    stand.rig.pump(2);
    if (stand.stored() != MOVED) {
        return law_broken("the next save did not carry the values again");
    }
    return law_held();
}

LawVerdict uncertain_write(const World &) {
    const FaultWatch faults;
    Stand stand("persistence_tests_uncertain");
    stand.mount();
    stand.body->set("where", MOVED);
    stand.backend->connection->doubt_next(OK);

    const Ref<NetwPromise> saving = stand.saved()->save();
    stand.rig.pump(2);
    if (answer(saving) != ERR_UNAVAILABLE) {
        return law_broken("the uncertain save answered %d", answer(saving));
    }
    if (!stand.saved()->get_dirty() || stand.log.count("failed") != 1) {
        return law_broken("the uncertain save was treated as stored");
    }
    return law_held();
}

LawVerdict client_cannot_save(const World &) {
    Stand stand("persistence_tests_client");
    stand.store(STORED);
    stand.saved()->load();
    stand.mount();
    Node *view = stand.view();
    if (view == nullptr) {
        return law_broken("the client never saw the entity");
    }
    const Ref<NetwPersistence> replica
        = NetwEntity::of(view)->get_persistence();
    view->set("where", MOVED);
    const Ref<NetwPromise> saving = replica->save();
    const Ref<NetwPromise> loading = replica->load();
    stand.rig.pump(2);
    if (answer(saving) != ERR_UNAUTHORIZED
        || answer(loading) != ERR_UNAUTHORIZED) {
        return law_broken(
            "the client's save answered %d and its load %d",
            answer(saving),
            answer(loading)
        );
    }
    if (stand.stored() != STORED || view->get("where") != MOVED) {
        return law_broken("the client's calls reached the record");
    }
    return law_held();
}

LawVerdict despawn_saves(const World &) {
    Stand stand("persistence_tests_despawn");
    stand.mount();
    stand.body->set("where", MOVED);
    stand.free_body();
    if (stand.stored() != MOVED) {
        return law_broken("the freed entity's last values were not saved");
    }
    return law_held();
}

LawVerdict final_write_retried(const World &) {
    const FaultWatch faults;
    Stand stand("persistence_tests_retry");
    stand.mount();
    stand.body->set("where", MOVED);
    stand.backend->connection->fail_next(ERR_FILE_CANT_WRITE);
    stand.free_body();
    if (stand.log.count("failed") != 1) {
        return law_broken(
            "the refused final write was reported %d times",
            stand.log.count("failed")
        );
    }

    const Ref<NetwPromise> retry = stand.session()->save_entities();
    stand.rig.pump(2);
    if (answer(retry) != OK || stand.stored() != MOVED) {
        return law_broken("the retry answered %d", answer(retry));
    }
    return law_held();
}

LawVerdict save_entities_waits(const World &) {
    Stand stand("persistence_tests_waits");
    stand.mount();
    stand.body->set("where", MOVED);
    stand.backend->connection->defer(true);
    stand.saved()->save();
    const Ref<NetwPromise> waiting = stand.session()->save_entities();
    stand.rig.pump(2);
    if (waiting->get_is_settled()) {
        return law_broken("save_entities settled before the write landed");
    }
    stand.backend->connection->release();
    stand.backend->connection->defer(false);
    stand.rig.pump(2);
    if (answer(waiting) != OK || stand.stored() != MOVED) {
        return law_broken("save_entities answered %d", answer(waiting));
    }
    return law_held();
}

LawVerdict leave_keeps_unsaved(const World &) {
    const FaultWatch faults;
    Stand stand("persistence_tests_quit");
    stand.mount();
    stand.body->set("where", MOVED);
    stand.backend->connection->fail_next(ERR_FILE_CANT_WRITE);

    const Ref<NetwPromise> refused = stand.session()->leave();
    stand.rig.pump(2);
    if (answer(refused) != ERR_FILE_CANT_WRITE) {
        return law_broken("the leave answered %d", answer(refused));
    }
    if (stand.server->session_get_state()
        != NetwMultiplayer::SESSION_STATE_ONLINE) {
        return law_broken("the server went down with a row unsaved");
    }
    stand.session()->leave();
    stand.rig.pump(2);
    if (stand.stored() != MOVED) {
        return law_broken("the second leave did not save the row");
    }
    return law_held();
}

const Row SAVING[] = {
    {"a spawned entity changed and saved, then saved again",
     "the first save stores the values and the second answers false",
     &saved_reads_back},
    {"a save the database refuses",
     "the promise fails, failed is emitted once, the entity stays dirty and "
     "the next save stores the values",
     &refused_write},
    {"a save whose outcome the database cannot confirm",
     "the promise fails with ERR_UNAVAILABLE and the entity stays dirty",
     &uncertain_write},
    {"a client saving and loading its view of the entity",
     "both fail with ERR_UNAUTHORIZED, and neither the record nor the view "
     "changes",
     &client_cannot_save},
    {"an entity changed and then despawned",
     "the values it held as it left are stored",
     &despawn_saves},
    {"a despawn whose final write the database refuses",
     "the failure is reported and save_entities stores the row",
     &final_write_retried},
    {"save_entities while a save is still being written",
     "it waits for the write and answers OK",
     &save_entities_waits},
    {"a server leaving while the database refuses a row",
     "the leave answers the error, the server stays online, and a second "
     "leave stores the row",
     &leave_keeps_unsaved},
};

TEST_CASE(
    "[Networked][Persistence][SceneTree] a saved entity's values reach the "
    "database, and a write that fails is reported and tried again"
) {
    const World world = {String("a server, one player and a database")};
    for (const Row &row : SAVING) {
        NETW_CELL(row, world);
        NETW_LAW_HOLDS(row, world);
    }
}

LawVerdict loads_before_mount(const World &) {
    {
        Stand stand("persistence_tests_found");
        stand.store(STORED);
        const Ref<NetwPromise> loading = stand.saved()->load();
        stand.rig.pump(2);
        if (answer(loading) != 1 || stand.body->get("where") != STORED) {
            return law_broken("the stored record was not loaded");
        }
        stand.mount();
        Node *view = stand.view();
        if (view == nullptr || view->get("where") != STORED) {
            return law_broken("the client did not see the stored values");
        }
    }
    Stand stand("persistence_tests_missing");
    const Ref<NetwPromise> loading = stand.saved()->load();
    stand.rig.pump(2);
    if (answer(loading) != 0 || stand.body->get("where") != START) {
        return law_broken("a missing record did not leave the spawn values");
    }
    return law_held();
}

LawVerdict withheld_while_loading(const World &) {
    Stand stand("persistence_tests_withheld");
    stand.store(STORED);
    stand.backend->connection->defer(true);
    stand.saved()->load();
    stand.mount();
    if (stand.view() != nullptr) {
        return law_broken("the client saw the entity before it loaded");
    }
    stand.backend->connection->release();
    stand.backend->connection->defer(false);
    stand.rig.step_ticks(4);

    Node *view = stand.view();
    if (view == nullptr) {
        return law_broken("the client never saw the loaded entity");
    }
    const Array seen = view->get("seen");
    if (seen.has(START) || view->get("where") != STORED) {
        return law_broken("the client saw values from before the load");
    }
    if (stand.log.count("loaded") != 1) {
        return law_broken("loaded came %d times", stand.log.count("loaded"));
    }
    return law_held();
}

LawVerdict failed_load_retried(const World &) {
    const FaultWatch faults;
    Stand stand("persistence_tests_unread");
    stand.store(STORED);
    stand.backend->connection->fail_next(ERR_FILE_CANT_READ);
    stand.mount();
    if (stand.log.count("failed") != 1 || stand.log.count("loaded") != 0
        || stand.failed_code() != ERR_FILE_CANT_READ) {
        return law_broken(
            "the refused load was reported %d times and loaded %d",
            stand.log.count("failed"),
            stand.log.count("loaded")
        );
    }
    if (stand.view() != nullptr) {
        return law_broken("the client saw an entity whose load failed");
    }
    if (stand.stored() != STORED) {
        return law_broken("the failed load wrote over the stored record");
    }

    const Ref<NetwPromise> retry = stand.saved()->load();
    stand.rig.step_ticks(4);
    Node *view = stand.view();
    if (answer(retry) != 1 || view == nullptr
        || view->get("where") != STORED) {
        return law_broken("the retried load did not publish the entity");
    }
    return law_held();
}

LawVerdict published_without_load(const World &) {
    Stand stand("persistence_tests_unloaded", false);
    stand.store(STORED);
    stand.mount();
    Node *view = stand.view();
    if (view == nullptr || view->get("where") != START) {
        return law_broken("the entity did not publish its spawn values");
    }
    if (stand.log.count("loaded") != 0) {
        return law_broken("an entity that loads nothing emitted loaded");
    }
    return law_held();
}

LawVerdict moved_while_loading(const World &) {
    Stand stand("persistence_tests_moved");
    stand.store(STORED);
    stand.backend->connection->defer(true);
    stand.mount();
    stand.body->set("where", MOVED);
    stand.backend->connection->release();
    stand.backend->connection->defer(false);
    stand.rig.step_ticks(4);
    if (stand.failed_code() != ERR_BUSY || stand.log.count("loaded") != 0) {
        return law_broken("the load was reported as %d", stand.failed_code());
    }
    if (stand.body->get("where") != MOVED) {
        return law_broken("the load wrote over a value the game set");
    }
    if (stand.view() != nullptr) {
        return law_broken("the client saw an entity whose load was refused");
    }
    return law_held();
}

LawVerdict freed_before_mount(const World &) {
    const FaultWatch faults;
    Stand stand("persistence_tests_freed");
    stand.store(STORED);
    stand.saved()->load();
    stand.rig.pump(2);
    stand.body->set("where", MOVED);
    memdelete(stand.body);
    stand.body = nullptr;
    stand.rig.step_ticks(4);
    stand.session()->save_entities();
    stand.rig.pump(2);

    if (stand.stored() != STORED) {
        return law_broken("a node that never entered the tree saved");
    }
    if (faults.errors() != 0) {
        return law_broken("freeing it raised %d errors", int(faults.errors()));
    }
    return law_held();
}

const Row LOADING[] = {
    {"a spawned entity loading a stored record, and one with none stored",
     "the first takes the stored values before the client sees it, the "
     "second keeps its spawn values",
     &loads_before_mount},
    {"an entity added to the tree while its load is in flight",
     "the client sees nothing until the load lands, then only the stored "
     "values",
     &withheld_while_loading},
    {"a load the database refuses, then a retry",
     "failed is emitted once, the client sees nothing and the record is "
     "untouched until the retry publishes the entity",
     &failed_load_retried},
    {"an entity declared with load_on_spawn(false)",
     "the client sees it at once with its spawn values",
     &published_without_load},
    {"a saved property the game sets while the load is in flight",
     "the load fails with ERR_BUSY, the game's value stands and the client "
     "sees nothing",
     &moved_while_loading},
    {"a spawned entity freed before it is added to the tree",
     "nothing is written over the stored record and nothing is reported",
     &freed_before_mount},
};

TEST_CASE(
    "[Networked][Persistence][SceneTree] a spawned entity reaches its "
    "clients only with its stored values"
) {
    const World world = {String("a server, one player and a database")};
    for (const Row &row : LOADING) {
        NETW_CELL(row, world);
        NETW_LAW_HOLDS(row, world);
    }
}

} // namespace TestPersistence

#endif
