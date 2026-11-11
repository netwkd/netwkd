#include "support/fault_log.h"
#include "support/netw_cells.h"
#include "support/netw_test.h"

#include "godot/callable.hpp"
#include "godot/file_access.hpp"
#include "godot/file_system.hpp"
#include "godot/node.hpp"
#include "godot/scene_tree.hpp"
#include "netw/api/context.hpp"
#include "netw/api/database.hpp"
#include "netw/api/database_backend.hpp"
#include "netw/api/database_config.hpp"
#include "netw/api/nodes/multiplayer_viewport.hpp"
#include "netw/api/promise.hpp"
#include "netw/api/schema_model.hpp"
#include "netw/persist/memory_store.hpp"
#include "netw/schema_model.hpp"

namespace TestNetwDatabase {

using namespace godot;
using netw::FileSystemDatabase;
using netw::MemoryDatabase;
using netw::Netw;
using netw::NetwDatabase;
using netw::NetwDatabaseBackend;
using netw::NetwPromise;
using netw::NetwSchema;
using netw::NetwWriteBatch;
using netw::persist::MemoryConnection;
using netw_test::law_broken;
using netw_test::law_held;
using netw_test::LawVerdict;

struct Stand {
    String label;
};

typedef netw_test::LawRowFor<Stand> Row;

class HeldBackend : public NetwDatabaseBackend {
public:
    Ref<MemoryConnection> connection;
    Ref<NetwPromise> next;

    explicit HeldBackend(const char *p_store) {
        connection = MemoryConnection::opened(p_store, "slot1");
    }

    Ref<NetwPromise> open_builtin(Object *, const StringName &) override {
        if (next.is_valid()) {
            const Ref<NetwPromise> answer = next;
            next.unref();
            return answer;
        }
        return NetwPromise::resolved(connection);
    }
};

Ref<NetwDatabaseBackend> memory(const char *p_store) {
    Ref<MemoryDatabase> made;
    made.instantiate();
    made->set_store(StringName(p_store));
    return made;
}

Ref<FileSystemDatabase> files(
    const String &p_root,
    FileSystemDatabase::Format p_format
) {
    Ref<FileSystemDatabase> made;
    made.instantiate();
    made->set_root(p_root);
    made->set_format(p_format);
    return made;
}

Node *declaring(const char *p_name, const Ref<NetwDatabaseBackend> &p_with) {
    Node *made = memnew(Node);
    made->set_name(StringName(p_name));
    Netw::configure_database(made, "saves")->backend(p_with);
    return made;
}

struct World {
    Node *viewport = nullptr;
    Node *author = nullptr;
    Ref<NetwDatabase> db;

    World(const char *p_name, const Ref<NetwDatabaseBackend> &p_backend) {
        viewport = memnew(netw::MultiplayerViewport);
        viewport->set_name(StringName(p_name));
        netw::gd::scene_root()->add_child(viewport);
        author = declaring("Author", p_backend);
        viewport->add_child(author);
        db = NetwDatabase::of(author, "saves");
    }

    ~World() {
        db.unref();
        viewport->get_parent()->remove_child(viewport);
        memdelete(viewport);
        netw::schema_model::clear();
        netw::persist::forget_stores();
    }

    bool opened() {
        return db.is_valid() && answer(db->open("slot1")) == OK;
    }

    static int answer(const Ref<NetwPromise> &p_promise) {
        if (p_promise.is_null() || !p_promise->get_is_settled()) {
            return -1;
        }
        if (p_promise->get_is_failed()) {
            return int(p_promise->get_code());
        }
        return int(p_promise->get_result());
    }
};

Ref<NetwSchema> players(bool p_titled = false) {
    const Ref<NetwSchema> schema = NetwSchema::create(
        p_titled ? "database_tests_titled" : "database_tests_players"
    );
    schema->replicated(false);
    schema->i64("gold", 1);
    if (p_titled) {
        schema->string("title", 1);
    }
    return schema;
}

Dictionary row(int64_t p_gold) {
    Dictionary out;
    out["gold"] = p_gold;
    return out;
}

Dictionary read(World &p_world, const Ref<NetwSchema> &p_schema, String p_id) {
    const Ref<NetwPromise> reading = p_world.db->read(p_schema, p_id);
    if (!reading->get_is_completed()) {
        return Dictionary();
    }
    return reading->get_result();
}

bool holds(const Dictionary &p_read, int64_t p_gold) {
    return int(p_read.get("error", -1)) == OK
        && bool(p_read.get("found", false))
        && int64_t(Dictionary(p_read["values"]).get("gold", -1)) == p_gold;
}

bool misses(const Dictionary &p_read) {
    return int(p_read.get("error", -1)) == OK
        && !bool(p_read.get("found", true));
}

LawVerdict work_before_open(const Stand &) {
    const Ref<HeldBackend> backend(memnew(HeldBackend("database_tests_open")));
    Ref<NetwPromise> connecting;
    connecting.instantiate();
    backend->next = connecting;
    World world("OpenSettles", backend);
    const Ref<NetwSchema> schema = players();

    const Ref<NetwPromise> opening = world.db->open("slot1");
    if (opening->get_is_settled()) {
        return law_broken("the open settled before the backend answered");
    }
    if (World::answer(world.db->write(schema, "hero", row(1)))
        != ERR_UNCONFIGURED) {
        return law_broken("a write ran before the open settled");
    }
    connecting->resolve(backend->connection);
    if (World::answer(opening) != OK) {
        return law_broken("the open did not settle OK");
    }
    if (!misses(read(world, schema, "hero"))) {
        return law_broken("the write refused before the open was kept");
    }
    if (World::answer(world.db->write(schema, "hero", row(1))) != OK
        || !holds(read(world, schema, "hero"), 1)) {
        return law_broken("a write after the open did not read back");
    }
    return law_held();
}

LawVerdict open_twice(const Stand &) {
    World world("OpenTwice", memory("database_tests_twice"));
    if (!world.opened() || World::answer(world.db->open("slot1")) != OK) {
        return law_broken("opening the open slot again did not answer OK");
    }
    if (World::answer(world.db->open("slot2")) != ERR_BUSY) {
        return law_broken("another slot opened over the open one");
    }
    return law_held();
}

class RefusingBackend : public NetwDatabaseBackend {
public:
    Ref<NetwPromise> open_builtin(Object *, const StringName &) override {
        return NetwPromise::rejected(ERR_CANT_OPEN, "refused open");
    }
};

LawVerdict refused_open(const Stand &) {
    const Ref<HeldBackend> backend(memnew(HeldBackend("database_tests_retry")));
    backend->next = NetwPromise::rejected(ERR_CANT_OPEN, "no such directory");
    World world("OpenRefused", backend);
    if (World::answer(world.db->open("slot1")) != ERR_CANT_OPEN) {
        return law_broken("the open did not fail with the backend's error");
    }
    if (!world.opened()) {
        return law_broken("the open did not succeed on retry");
    }

    World always(
        "OpenAlwaysRefused",
        Ref<NetwDatabaseBackend>(memnew(RefusingBackend))
    );
    if (World::answer(always.db->open("slot1")) != ERR_CANT_OPEN) {
        return law_broken("a declared database swallowed the open failure");
    }
    return law_held();
}

LawVerdict write_read_erase(const Stand &) {
    World world("Records", memory("database_tests_records"));
    const Ref<NetwSchema> schema = players();
    if (!world.opened()
        || World::answer(world.db->write(schema, "hero", row(12))) != OK) {
        return law_broken("the write did not answer OK");
    }
    if (!holds(read(world, schema, "hero"), 12)) {
        return law_broken("the written record did not read back");
    }
    if (!misses(read(world, schema, "nobody"))) {
        return law_broken("a record never written was not a plain miss");
    }
    if (World::answer(world.db->erase(schema, "hero")) != OK
        || !misses(read(world, schema, "hero"))) {
        return law_broken("the erased record still read back");
    }
    if (World::answer(world.db->erase(schema, "hero")) != OK) {
        return law_broken("erasing an absent record did not answer OK");
    }
    return law_held();
}

LawVerdict failing_backend(const Stand &) {
    const Ref<HeldBackend> backend(memnew(HeldBackend("database_tests_fail")));
    World world("Failing", backend);
    const Ref<NetwSchema> schema = players();
    if (!world.opened()
        || World::answer(world.db->write(schema, "hero", row(12))) != OK) {
        return law_broken("the first write did not answer OK");
    }

    backend->connection->fail_next(ERR_FILE_CANT_READ);
    const Dictionary unread = read(world, schema, "hero");
    if (int(unread.get("error", -1)) != ERR_FILE_CANT_READ
        || bool(unread.get("found", true))) {
        return law_broken("an unreadable record did not read as a failure");
    }

    backend->connection->fail_next(ERR_FILE_CANT_READ);
    if (World::answer(world.db->patch(schema, "hero", row(99)))
        != ERR_FILE_CANT_READ) {
        return law_broken("a patch whose read failed did not answer it");
    }

    backend->connection->defer(true);
    const Ref<NetwPromise> unwritten = world.db->patch(schema, "hero", row(99));
    backend->connection->defer(false);
    backend->connection->fail_next(ERR_FILE_CANT_WRITE);
    backend->connection->release();
    if (World::answer(unwritten) != ERR_FILE_CANT_WRITE) {
        return law_broken("a patch whose write failed did not answer it");
    }
    if (!holds(read(world, schema, "hero"), 12)) {
        return law_broken("a failed patch changed the record");
    }
    return law_held();
}

LawVerdict bad_rows(const Stand &) {
    World world("BadRows", memory("database_tests_bad"));
    const Ref<NetwSchema> schema = players(true);
    Dictionary missing = row(1);
    Dictionary mistyped = row(1);
    mistyped["title"] = 7;
    Dictionary extra = row(1);
    extra["title"] = String("hero");
    extra["unknown"] = 1;
    if (!world.opened()) {
        return law_broken("the database did not open");
    }
    const Dictionary refused[] = {missing, mistyped, extra};
    for (const Dictionary &values : refused) {
        if (!world.db->write(schema, "hero", values)->get_is_failed()) {
            return law_broken("a write with a bad row was not refused");
        }
    }
    if (!misses(read(world, schema, "hero"))) {
        return law_broken("a refused row reached the backend");
    }
    return law_held();
}

LawVerdict patch_some(const Stand &) {
    World world("Patch", memory("database_tests_patch"));
    const Ref<NetwSchema> schema = players(true);
    Dictionary values = row(12);
    values["title"] = String("knight");
    if (!world.opened()
        || World::answer(world.db->write(schema, "hero", values)) != OK) {
        return law_broken("the write did not answer OK");
    }
    if (World::answer(world.db->patch(schema, "hero", row(99))) != OK) {
        return law_broken("the patch did not answer OK");
    }
    const Dictionary after = read(world, schema, "hero");
    if (!holds(after, 99)
        || String(Dictionary(after["values"])["title"]) != String("knight")) {
        return law_broken("the patch did not keep the property it left out");
    }
    if (World::answer(world.db->patch(schema, "ghost", row(1)))
        != ERR_DOES_NOT_EXIST) {
        return law_broken("a patch of an absent record was not refused");
    }
    Dictionary unknown;
    unknown["nothing"] = 1;
    if (!world.db->patch(schema, "hero", unknown)->get_is_failed()) {
        return law_broken("a patch of an unknown property was not refused");
    }
    return law_held();
}

LawVerdict slow_writes(const Stand &) {
    const Ref<HeldBackend> backend(memnew(HeldBackend("database_tests_slow")));
    World world("SlowWrites", backend);
    const Ref<NetwSchema> schema = players();
    if (!world.opened()) {
        return law_broken("the database did not open");
    }
    backend->connection->defer(true);
    const Ref<NetwPromise> first = world.db->write(schema, "hero", row(1));
    const Ref<NetwPromise> second = world.db->write(schema, "hero", row(2));
    backend->connection->release();
    if (!first->get_is_settled() || second->get_is_settled()) {
        return law_broken("the later write settled with the earlier one");
    }
    backend->connection->release();
    backend->connection->defer(false);
    if (World::answer(second) != OK || !holds(read(world, schema, "hero"), 2)) {
        return law_broken("the later write is not what the record holds");
    }
    return law_held();
}

LawVerdict caller_dictionaries(const Stand &) {
    const Ref<HeldBackend> backend(memnew(HeldBackend("database_tests_copy")));
    World world("Copies", backend);
    const Ref<NetwSchema> schema = players();
    if (!world.opened()) {
        return law_broken("the database did not open");
    }
    backend->connection->defer(true);
    world.db->write(schema, "hero", row(0));
    Dictionary values = row(1);
    const Ref<NetwPromise> queued = world.db->write(schema, "hero", values);
    values["gold"] = 999;
    backend->connection->release();
    backend->connection->release();
    backend->connection->defer(false);
    if (World::answer(queued) != OK || !holds(read(world, schema, "hero"), 1)) {
        return law_broken("a change after the write reached the record");
    }

    const Dictionary answered = read(world, schema, "hero");
    Dictionary answered_values = answered["values"];
    answered_values["gold"] = 404;
    if (!holds(read(world, schema, "hero"), 1)) {
        return law_broken("a change to a read reached the record");
    }
    return law_held();
}

LawVerdict batch(const Stand &) {
    World world("Batch", memory("database_tests_batch"));
    const Ref<NetwSchema> schema = players();
    if (!world.opened()
        || World::answer(world.db->write(schema, "two", row(2))) != OK) {
        return law_broken("the first write did not answer OK");
    }
    const Ref<NetwWriteBatch> batch = world.db->batch();
    batch->write(schema, "one", row(5));
    batch->erase(schema, "two");
    const Ref<NetwPromise> submitted = batch->submit();
    if (!submitted->get_is_completed()) {
        return law_broken("the batch did not settle");
    }
    const Dictionary answered = submitted->get_result();
    if (int(answered.get("error", -1)) != OK
        || PackedInt32Array(answered["errors"]).size() != 2
        || PackedByteArray(answered["uncertain"]).size() != 2) {
        return law_broken("the batch did not answer once per operation");
    }
    if (!holds(read(world, schema, "one"), 5)
        || !misses(read(world, schema, "two"))) {
        return law_broken("the batch did not write and erase");
    }
    return law_held();
}

LawVerdict scan_pages(const Stand &) {
    World world("Scan", memory("database_tests_scan"));
    const Ref<NetwSchema> schema = players();
    if (!world.opened()) {
        return law_broken("the database did not open");
    }
    world.db->write(schema, "a", row(1));
    world.db->write(schema, "b", row(2));
    world.db->write(schema, "c", row(3));
    const Dictionary first
        = world.db->scan(schema, Dictionary(), "", 2)->get_result();
    if (int(first.get("error", -1)) != OK || Array(first["records"]).size() != 2
        || String(first["cursor"]).is_empty()) {
        return law_broken("the first page did not hold two and a cursor");
    }
    const Dictionary next
        = world.db->scan(schema, Dictionary(), String(first["cursor"]), 2)
              ->get_result();
    if (Array(next["records"]).size() != 1
        || !String(next["cursor"]).is_empty()) {
        return law_broken("the cursor did not continue to the last record");
    }
    return law_held();
}

LawVerdict close_while_saving(const Stand &) {
    const Ref<HeldBackend> backend(memnew(HeldBackend("database_tests_close")));
    World world("CloseWhileSaving", backend);
    const Ref<NetwSchema> schema = players();
    if (!world.opened() || !world.db->flush()->get_is_completed()) {
        return law_broken("an idle database did not flush at once");
    }
    backend->connection->defer(true);
    const Ref<NetwPromise> written = world.db->write(schema, "hero", row(1));
    const Ref<NetwPromise> flushing = world.db->flush();
    const Ref<NetwPromise> closing = world.db->close();
    if (flushing->get_is_settled() || closing->get_is_settled()) {
        return law_broken("a flush or close settled before the write");
    }
    backend->connection->release();
    backend->connection->defer(false);
    if (World::answer(written) != OK || !flushing->get_is_completed()
        || !closing->get_is_completed()) {
        return law_broken("the write, flush and close did not all complete");
    }
    if (!world.opened() || !holds(read(world, schema, "hero"), 1)) {
        return law_broken("the write was lost across the close");
    }
    return law_held();
}

LawVerdict uncertain_write(const Stand &) {
    const Ref<HeldBackend> backend(memnew(HeldBackend("database_tests_doubt")));
    World world("Uncertain", backend);
    const Ref<NetwSchema> schema = players();
    if (!world.opened()
        || World::answer(world.db->write(schema, "hero", row(1))) != OK) {
        return law_broken("the first write did not answer OK");
    }
    backend->connection->doubt_next(OK);
    if (World::answer(world.db->write(schema, "hero", row(2)))
        != ERR_UNAVAILABLE) {
        return law_broken("an uncertain write answered OK");
    }
    backend->connection->doubt_next(OK);
    if (World::answer(world.db->patch(schema, "hero", row(3)))
        != ERR_UNAVAILABLE) {
        return law_broken("an uncertain patch answered OK");
    }
    backend->connection->doubt_next(OK);
    if (World::answer(world.db->erase(schema, "hero")) != ERR_UNAVAILABLE) {
        return law_broken("an uncertain erase answered OK");
    }
    backend->connection->doubt_next(ERR_TIMEOUT);
    if (World::answer(world.db->write(schema, "hero", row(4))) != ERR_TIMEOUT) {
        return law_broken("an uncertain write lost the backend's error");
    }
    return law_held();
}

void lookup_at_ready(Node *p_child) {
    p_child->set_meta("found", NetwDatabase::of(p_child, "saves").is_valid());
}

LawVerdict declared_before_ready(const Stand &) {
    World world("DeclaredBeforeReady", memory("database_tests_ready"));
    Node *author = declaring("Later", memory("database_tests_ready"));
    Node *child = memnew(Node);
    child->set_name("Child");
    child->connect("ready", callable_mp_static(&lookup_at_ready).bind(child));
    author->add_child(child);
    world.viewport->add_child(author);
    if (!bool(child->get_meta("found", false))) {
        return law_broken("the child's ready found no database");
    }
    return law_held();
}

LawVerdict author_freed(const Stand &) {
    World world("AuthorFreed", memory("database_tests_freed"));
    const Ref<NetwSchema> schema = players();
    const Ref<NetwDatabase> db = world.db;
    if (!world.opened()) {
        return law_broken("the database did not open");
    }
    world.viewport->remove_child(world.author);
    memdelete(world.author);
    if (World::answer(db->write(schema, "hero", row(7))) != OK
        || !holds(read(world, schema, "hero"), 7)) {
        return law_broken("the database stopped serving with its author gone");
    }
    db->close();

    Node *replacement = declaring("Author", memory("database_tests_fresh"));
    world.viewport->add_child(replacement);
    if (NetwDatabase::of(replacement, "saves") != db) {
        return law_broken("the new author declared another database");
    }
    if (!world.opened() || !misses(read(world, schema, "hero"))) {
        return law_broken("the reopened database did not use the new backend");
    }
    return law_held();
}

LawVerdict conflicting_backends(const Stand &) {
    netw_test::FaultWatch faults;
    faults.expect_error("*Database 'saves'*[NW0034]*", 1);
    World world("Conflicting", memory("database_tests_one"));
    Node *other = declaring("Other", memory("database_tests_two"));
    world.viewport->add_child(other);
    const Ref<NetwPromise> opening = world.db->open("slot1");
    if (World::answer(opening) != ERR_ALREADY_EXISTS) {
        return law_broken("two live backends did not refuse the open");
    }
    world.viewport->remove_child(other);
    memdelete(other);
    if (!world.opened()) {
        return law_broken("the open failed with one backend left");
    }
    return law_held();
}

String scratch(const String &p_name) {
    const String root = String("user://netw_database_tests/") + p_name;
    DirAccess::make_dir_recursive_absolute(netw::gd::globalized(root));
    return root;
}

void files_under(const String &p_dir, PackedStringArray &r_paths) {
    const PackedStringArray names = DirAccess::get_files_at(p_dir);
    for (int at = 0; at < names.size(); ++at) {
        r_paths.push_back(p_dir.path_join(names[at]));
    }
    const PackedStringArray folders = DirAccess::get_directories_at(p_dir);
    for (int at = 0; at < folders.size(); ++at) {
        files_under(p_dir.path_join(folders[at]), r_paths);
    }
}

String the_one_file(const String &p_root) {
    PackedStringArray paths;
    files_under(p_root, paths);
    return paths.size() == 1 ? paths[0] : String();
}

LawVerdict failed_replacement(const Stand &) {
    const String root = scratch("replace");
    World world(
        "FailedReplacement",
        files(root, FileSystemDatabase::FORMAT_BINARY)
    );
    const Ref<NetwSchema> schema = players();
    world.db->delete_slot("slot1");
    if (!world.opened()
        || World::answer(world.db->write(schema, "hero", row(3))) != OK) {
        return law_broken("the first write did not answer OK");
    }
    const String folder = the_one_file(root).get_base_dir();
    if (folder.is_empty()) {
        return law_broken("the write left no single record file");
    }
    const String disk = netw::gd::globalized(folder);
    FileAccess::set_unix_permissions(disk, 0555);
    const int refused = World::answer(world.db->write(schema, "hero", row(99)));
    FileAccess::set_unix_permissions(disk, 0755);
    if (refused == OK) {
        return law_broken("a write into a read only folder answered OK");
    }
    if (!holds(read(world, schema, "hero"), 3)) {
        return law_broken("a failed replacement lost the record");
    }
    return law_held();
}

LawVerdict escaping_keys(const Stand &) {
    netw_test::FaultWatch expected_faults;
    expected_faults.expect_stderr(
        "Unicode parsing error*Unexpected NUL character",
        1
    );
    const String root = scratch("escape");
    World world("EscapingKeys", files(root, FileSystemDatabase::FORMAT_BINARY));
    const Ref<NetwSchema> schema = players();
    world.db->delete_slot("slot1");
    if (!world.opened()) {
        return law_broken("the database did not open");
    }
    const String keys[] = {
        "../../../../escaped",
        "../../../../../escaped",
        "..",
        "a/b\\c",
        String("a") + String::chr(0) + String("b"),
    };
    for (int at = 0; at < 5; ++at) {
        if (World::answer(world.db->write(schema, keys[at], row(at))) != OK
            || !holds(read(world, schema, keys[at]), at)) {
            return law_broken("the key %d did not write and read back", at);
        }
    }
    PackedStringArray everywhere;
    files_under("user://netw_database_tests", everywhere);
    for (int at = 0; at < everywhere.size(); ++at) {
        if (everywhere[at].begins_with("user://netw_database_tests/escape/")) {
            continue;
        }
        if (everywhere[at].contains("escaped")) {
            return law_broken("a record landed outside its root");
        }
    }
    const PackedStringArray top = DirAccess::get_files_at("user://");
    for (int at = 0; at < top.size(); ++at) {
        if (top[at].begins_with("escaped")) {
            return law_broken("a record landed in the user folder");
        }
    }
    return law_held();
}

LawVerdict edited_text(const Stand &) {
    const String root = scratch("text");
    World world("EditedText", files(root, FileSystemDatabase::FORMAT_TEXT));
    const Ref<NetwSchema> schema = players(true);
    world.db->delete_slot("slot1");
    Dictionary values = row(1);
    values["title"] = String("Object(RefCounted)");
    if (!world.opened()
        || World::answer(world.db->write(schema, "hero", values)) != OK) {
        return law_broken("the write did not answer OK");
    }
    const Dictionary clean_read = read(world, schema, "hero");
    if (!holds(clean_read, 1)
        || String(Dictionary(clean_read["values"])["title"])
            != String("Object(RefCounted)")) {
        return law_broken("a title spelled like an object did not load");
    }
    const String path = the_one_file(root);
    const String clean = FileAccess::get_file_as_string(path);
    const char *payloads[] = {
        "Object(RefCounted,\"script\":null)",
        "Resource(\"res://project.godot\")",
        "ExtResource(\"1\")",
        "SubResource(\"1\")",
        "1Object(RefCounted)",
    };
    for (const char *payload : payloads) {
        const Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
        if (file.is_null()) {
            return law_broken("the save file could not be edited");
        }
        file->store_string(
            clean.replace("\"gold\": 1", String("\"gold\": ") + payload)
        );
        file->close();
        const Dictionary answered = read(world, schema, "hero");
        if (int(answered.get("error", -1)) != ERR_FILE_UNRECOGNIZED) {
            return law_broken("the edit '%s' was not refused", payload);
        }
    }
    return law_held();
}

const Row ROWS[] = {
    {"work sent before the database has opened",
     "it is refused, and work after the open settles runs",
     &work_before_open},
    {"a second open of the open slot, or of another slot",
     "the same slot answers OK and another is refused as busy",
     &open_twice},
    {"an open the backend refuses",
     "it fails with the backend's error and a later open succeeds",
     &refused_open},
    {"a record written, read, missed and erased",
     "it reads back, a missing one is absent, and erasing twice is OK",
     &write_read_erase},
    {"a backend that cannot read or write",
     "the read or patch fails with its error and the record is unchanged",
     &failing_backend},
    {"a row with a missing, mistyped or unknown property",
     "the write is refused before the backend sees it",
     &bad_rows},
    {"a patch of some properties",
     "it keeps the rest, and an absent record or unknown property is refused",
     &patch_some},
    {"two writes to one record while the first is still saving",
     "they settle in order and the record holds the later one",
     &slow_writes},
    {"a Dictionary changed after it was written or read",
     "the record holds what was written",
     &caller_dictionaries},
    {"a batch of a write and an erase",
     "it answers once per operation and does both",
     &batch},
    {"more records than one scan page",
     "the cursor continues where the page stopped",
     &scan_pages},
    {"a flush or a close while a write is still saving",
     "they wait for the write, and the record is there after reopening",
     &close_while_saving},
    {"a write whose outcome the backend cannot confirm",
     "it never answers OK",
     &uncertain_write},
    {"a database looked up in a child's ready",
     "the declaring node's database is already there",
     &declared_before_ready},
    {"a declaring node freed and declared again",
     "the database keeps serving, and reopening uses the new backend",
     &author_freed},
    {"two live nodes declaring one database with different backends",
     "the open is refused with one error until one of them leaves",
     &conflicting_backends},
    {"a file write that fails before it replaces the record",
     "the record it would have overwritten still reads back",
     &failed_replacement},
    {"a record key naming a parent folder, a separator or a NUL",
     "the record reads back and no file lands outside the save folder",
     &escaping_keys},
    {"a text save edited to build an object or load a resource",
     "the read is refused, and a string that only looks like one loads",
     &edited_text},
};

TEST_CASE("[Networked][Table][SceneTree] a save holds what a game wrote") {
    const Stand stand = {String("a database a node declares")};
    for (const Row &row : ROWS) {
        NETW_CELL(row, stand);
        NETW_LAW_HOLDS(row, stand);
    }
}

} // namespace TestNetwDatabase
