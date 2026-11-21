#include "support/fault_log.h"
#include "support/frame_drive.h"
#include "support/internals.h"
#include "support/minted_script.h"
#include "support/netw_test.h"

#if defined(NETW_TIER_HOSTED)

#include <string>
#include <vector>

#include "support/alloc_probe.h"
#include "support/sim_stand.h"

#include "godot/node.hpp"
#include "godot/physics_body.hpp"
#include "godot/physics_server.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/store/store.hpp"

using namespace godot;
using netw_test::Internals;

namespace TestNetwStorePull {

using netw::NetwMultiplayer;
using netw::store::RowAddress;
using netw::store::Table;
using netw::store::Store;

constexpr const char *CUBE_ID = "store_pull_cube";

constexpr const char *CUBE_SOURCE = R"(extends RigidBody3D

var tint := 0

func _init() -> void:
	gravity_scale = 0.0
	collision_layer = 0
	collision_mask = 0
	var shape := CollisionShape3D.new()
	shape.shape = BoxShape3D.new()
	add_child(shape)
	Netw.configure_entity(self)
	Netw.configure_property(self, &"position").broadcast()
	Netw.configure_property(self, &"quaternion").broadcast()
	Netw.configure_property(self, &"linear_velocity").broadcast()
	Netw.configure_property(self, &"angular_velocity").broadcast()
	Netw.configure_property(self, &"sleeping").broadcast()
	Netw.configure_property(self, &"tint").broadcast()
	Netw.configure_property(self, &"mass").broadcast()
)";

constexpr const char *BODY_SOURCE = R"(extends RigidBody3D

func _init() -> void:
	gravity_scale = 0.0
	collision_layer = 0
	collision_mask = 0
	var shape := CollisionShape3D.new()
	shape.shape = BoxShape3D.new()
	add_child(shape)
	Netw.configure_entity(self)
	Netw.configure_property(self, &"position").broadcast()
	Netw.configure_property(self, &"quaternion").broadcast()
	Netw.configure_property(self, &"linear_velocity").broadcast()
	Netw.configure_property(self, &"angular_velocity").broadcast()
	Netw.configure_property(self, &"sleeping").broadcast()
)";

constexpr int CUBES = 5;
constexpr int TICKRATE = 30;
constexpr int SETTLE_FRAMES = 60;
constexpr int WINDOW_FRAMES = 30;
constexpr int WINDOW_AT = SETTLE_FRAMES + WINDOW_FRAMES;
constexpr int WAKE_AT = WINDOW_AT + 2;
constexpr int MISS_AT = WAKE_AT + 3;
constexpr int MISS_READ_AT = MISS_AT + 2;
constexpr int PULL_END = MISS_AT + int(Store::SWEEP_TICKS) + 2;
const Vector3 NUDGE(1.0, 0.0, 0.0);
const Vector3 SHIFT(4.0, 0.0, 0.0);

Script *cube_script = nullptr;

Node *build_cube(const Variant &p_name) {
    if (cube_script == nullptr) {
        return nullptr;
    }
    Node *made = Object::cast_to<Node>(cube_script->call("new"));
    if (made != nullptr) {
        made->set_name(String(p_name));
    }
    return made;
}

RowAddress row_of_node(const Store &p_world, Node *p_node) {
    RowAddress found;
    const uint64_t id = uint64_t(p_node->get_instance_id());
    for (uint32_t table = 0; table < p_world.table_count(); ++table) {
        const Table &held = p_world.table(table);
        for (uint32_t row = 0; row < held.row_count(); ++row) {
            if (p_world.pull->residents.node_of(table, row) == id) {
                found.table = table;
                found.row = row;
            }
        }
    }
    return found;
}

struct PullEvidence {
    bool driven = false;
    int asleep = 0;
    int64_t window_ticks = 0;
    int64_t window_rows = 0;
    int64_t window_rested = 0;
    int64_t window_body = 0;
    int64_t window_script = 0;
    int64_t window_engine = 0;
    bool woken_read = false;
    int64_t woken_body = 0;
    int misses = 0;
    bool miss_named = false;
    bool miss_restored = false;
    std::string miss_gate;
    int64_t moved_at = -1;
    int64_t moved_changed = -1;
    bool moved_quiet = false;
};

PullEvidence &pull_evidence() {
    static PullEvidence evidence;
    return evidence;
}

class PullStand {
protected:
    netw_test::SimStand *stand = nullptr;
    Ref<Script> script;
    LocalVector<RID> cubes;

    NetwMultiplayer *server() const {
        return stand->session(-1);
    }

    RigidBody3D *cube(int p_index) const {
        return Object::cast_to<RigidBody3D>(
            server()->entity_get_node(cubes[p_index])
        );
    }

    Table &table_of(int p_index) const {
        const RowAddress at
            = row_of_node(Internals::store(server()), cube(p_index));
        return Internals::store(server()).table(at.table);
    }

    uint32_t row_at(int p_index) const {
        return row_of_node(Internals::store(server()), cube(p_index)).row;
    }

    bool open_stand(const char *p_source, int p_cubes = CUBES) {
        script = netw_test::minted_script(p_source);
        if (script.is_null()) {
            return false;
        }
        cube_script = script.ptr();
        stand = new netw_test::SimStand(0);
        if (!stand->ready()) {
            return false;
        }
        stand->teach(StringName(CUBE_ID), callable_mp_static(&build_cube));
        stand->mount();
        stand->arm(TICKRATE);
        Node *arena = stand->arena();
        for (int index = 0; index < p_cubes; ++index) {
            Array args;
            args.push_back(String("pulled") + String::num_int64(index));
            const StringName id(CUBE_ID);
            const RID made = server()->spawn_registered(id, args, 0);
            Node *built = server()->entity_get_node(made);
            if (built == nullptr) {
                return false;
            }
            arena->add_child(built);
            cubes.push_back(made);
        }
        stand->pump(8);
        return true;
    }

    void close_stand() {
        delete stand;
        stand = nullptr;
        cube_script = nullptr;
        script = Ref<Script>();
        cubes.clear();
    }
};

class PullScenario final : public netw_test::FrameScenario, PullStand {
    int frame = 0;
    bool done = false;
    Store::PullWork opened;
    int64_t faults_before = 0;
    std::string first_before;
    int64_t count_before = 0;
    int64_t count_last = 0;

    void read_window() {
        PullEvidence &seen = pull_evidence();
        const Store::PullWork &now = Internals::store(server()).work;
        seen.window_ticks = WINDOW_FRAMES;
        seen.window_rows = now.rows - opened.rows;
        seen.window_rested = now.rested - opened.rested;
        seen.window_body = now.body - opened.body;
        seen.window_script = now.script - opened.script;
        seen.window_engine = now.engine - opened.engine;
        for (int index = 0; index < CUBES; ++index) {
            seen.asleep += cube(index)->is_sleeping() ? 1 : 0;
        }
    }

    bool changed_now(int p_index) const {
        const Table &table = table_of(p_index);
        return table.changed_ticks[row_at(p_index)] == table.pulled_tick;
    }

    void plant_miss() {
        netw::store::validation_audit::Ledger &book
            = netw::store::validation_audit::ledger();
        faults_before = book.faults;
        first_before = book.first;
        count_before = Internals::store(server()).miss_count;
        Internals::store(server()).validate_every = 1;
        Internals::store(server()).misses.clear();
        pull_evidence().moved_at = table_of(4).pulled_tick;
        RigidBody3D *held = cube(4);
        PhysicsServer3D *physics = PhysicsServer3D::get_singleton();
        Transform3D moved = held->get_global_transform();
        moved.origin += SHIFT;
        physics->body_set_state(
            held->get_rid(),
            PhysicsServer3D::BODY_STATE_TRANSFORM,
            moved
        );
        physics->body_set_state(
            held->get_rid(),
            PhysicsServer3D::BODY_STATE_SLEEPING,
            true
        );
    }

    void read_misses() {
        PullEvidence &seen = pull_evidence();
        Store &world = Internals::store(server());
        const int64_t route = table_of(4).routes[row_at(4)];
        seen.misses = int(world.misses.size());
        seen.miss_named = !world.misses.is_empty();
        for (uint32_t at = 0; at < world.misses.size(); ++at) {
            const netw::store::Miss &missed = world.misses[at];
            seen.miss_named = seen.miss_named && missed.route == route
                && missed.column == StringName("position");
            seen.miss_gate = missed.gate;
        }
    }

    void read_caught() {
        PullEvidence &seen = pull_evidence();
        Store &world = Internals::store(server());
        seen.moved_changed = table_of(4).changed_ticks[row_at(4)];
        seen.moved_quiet = world.miss_count == count_last;
        netw::store::validation_audit::Ledger &book
            = netw::store::validation_audit::ledger();
        seen.miss_restored
            = book.faults - faults_before == world.miss_count - count_before;
        book.planted += book.faults - faults_before;
        book.faults = faults_before;
        book.first = first_before;
        world.validate_every = netw::store::validation_interval();
    }

public:
    bool advance() override {
        if (netw::gd::scene_root() == nullptr || done) {
            return false;
        }
        if (frame == 0 && !open_stand(CUBE_SOURCE)) {
            close_stand();
            pull_evidence().driven = true;
            done = true;
            return false;
        }
        PullEvidence &seen = pull_evidence();
        if (frame == SETTLE_FRAMES) {
            opened = Internals::store(server()).work;
        }
        if (frame == WINDOW_AT) {
            read_window();
        }
        const int64_t body_before = Internals::store(server()).work.body;
        if (frame == WAKE_AT) {
            cube(3)->set_linear_velocity(NUDGE);
        }
        if (frame == MISS_AT) {
            plant_miss();
        }
        if (frame == MISS_READ_AT) {
            read_misses();
        }
        if (frame == PULL_END) {
            read_caught();
            close_stand();
            seen.driven = true;
            done = true;
            return false;
        }
        count_last = Internals::store(server()).miss_count;
        stand->step_ticks(1);
        if (frame == WAKE_AT) {
            seen.woken_body
                = Internals::store(server()).work.body - body_before;
            seen.woken_read = changed_now(3)
                && (table_of(3).flags[row_at(3)] & Table::RESTED) == 0;
        }
        ++frame;
        return true;
    }
};

NETW_FRAME_SCENARIO(PullScenario, store_pull_scenario);

constexpr int PILE = 64;
constexpr int ALLOC_WINDOW = 30;

struct AllocEvidence {
    bool driven = false;
    int asleep = 0;
    int64_t pulls = 0;
    int64_t rested = 0;
    int64_t read = 0;
    int64_t in_pull = 0;
    int64_t in_window = 0;
};

AllocEvidence &alloc_evidence() {
    static AllocEvidence evidence;
    return evidence;
}

class AllocScenario final : public netw_test::FrameScenario, PullStand {
    int frame = 0;
    bool done = false;
    netw_test::AllocProbe probe;
    Store::PullWork opened;
    int64_t seen_opened = 0;

    void read() {
        AllocEvidence &seen = alloc_evidence();
        const Store::PullWork &now = Internals::store(server()).work;
        seen.pulls = now.pulls - opened.pulls;
        seen.rested = now.rested - opened.rested;
        seen.read = now.rows - opened.rows;
        seen.in_pull = now.allocations - opened.allocations;
        seen.in_window = netw::store::allocations_seen() - seen_opened;
        for (int index = 0; index < PILE; ++index) {
            seen.asleep += cube(index)->is_sleeping() ? 1 : 0;
        }
    }

public:
    bool advance() override {
        if (netw::gd::scene_root() == nullptr || done) {
            return false;
        }
        if (frame == 0 && !open_stand(BODY_SOURCE, PILE)) {
            close_stand();
            alloc_evidence().driven = true;
            done = true;
            return false;
        }
        if (frame == SETTLE_FRAMES) {
            Internals::store(server()).validate_every = 0;
            probe.arm();
            opened = Internals::store(server()).work;
            seen_opened = netw::store::allocations_seen();
        }
        if (frame == SETTLE_FRAMES + ALLOC_WINDOW) {
            probe.disarm();
            read();
            close_stand();
            alloc_evidence().driven = true;
            done = true;
            return false;
        }
        stand->step_ticks(1);
        ++frame;
        return true;
    }
};

NETW_FRAME_SCENARIO(AllocScenario, store_alloc_scenario);

constexpr const char *BEATING_SOURCE = R"(extends RigidBody3D

func _init() -> void:
	gravity_scale = 0.0
	collision_layer = 0
	collision_mask = 0
	var shape := CollisionShape3D.new()
	shape.shape = BoxShape3D.new()
	add_child(shape)
	Netw.configure_entity(self)
	Netw.configure_property(self, &"position").broadcast().heartbeat(3)
	Netw.configure_property(self, &"quaternion").broadcast().heartbeat(3)
)";

constexpr int SEND_PILE = 16;
constexpr int SEND_WINDOW = 60;

struct SendAllocEvidence {
    bool driven = false;
    int asleep = 0;
    int64_t sends = 0;
    int64_t sent_rows = 0;
    int64_t in_pump = 0;
    int64_t in_send = 0;
    int64_t in_frames = 0;
    int64_t in_carrier = 0;
    int64_t in_display = 0;
    int64_t in_transport = 0;
};

enum SendArm { ONE_PEER, ALONE };

SendAllocEvidence &send_alloc_evidence(SendArm p_arm) {
    static SendAllocEvidence evidence[2];
    return evidence[p_arm];
}

class SendAllocScenario final : public netw_test::FrameScenario {
    const SendArm arm;
    int frame = 0;
    bool done = false;
    netw_test::SimStand *stand = nullptr;
    Ref<Script> script;
    LocalVector<RID> cubes;
    netw_test::AllocProbe probe;
    NetwMultiplayer::IdleWork opened;

    NetwMultiplayer *server() const {
        return stand->session(-1);
    }

    bool open() {
        script = netw_test::minted_script(BEATING_SOURCE);
        if (script.is_null()) {
            return false;
        }
        cube_script = script.ptr();
        stand = new netw_test::SimStand(arm == ALONE ? 0 : 1);
        if (!stand->ready()) {
            return false;
        }
        stand->teach(StringName(CUBE_ID), callable_mp_static(&build_cube));
        stand->mount();
        stand->arm(TICKRATE);
        Node *arena = stand->arena();
        for (int index = 0; index < SEND_PILE; ++index) {
            Array args;
            args.push_back(String("beating") + String::num_int64(index));
            const StringName id(CUBE_ID);
            const RID made = server()->spawn_registered(id, args, 0);
            Node *built = server()->entity_get_node(made);
            if (built == nullptr) {
                return false;
            }
            arena->add_child(built);
            cubes.push_back(made);
        }
        stand->pump(8);
        return true;
    }

    void read() {
        SendAllocEvidence &seen = send_alloc_evidence(arm);
        const NetwMultiplayer::IdleWork &now = Internals::idle_work(server());
        seen.in_frames = now.frame_allocations - opened.frame_allocations;
        seen.in_carrier = now.carrier_allocations - opened.carrier_allocations;
        seen.in_display = now.display_allocations - opened.display_allocations;
        seen.in_transport
            = now.transport_allocations - opened.transport_allocations;
        seen.sends = now.sends - opened.sends;
        seen.sent_rows = now.sent_rows - opened.sent_rows;
        seen.in_pump = now.pump_allocations - opened.pump_allocations;
        seen.in_send = now.send_allocations - opened.send_allocations;
        for (const RID &cube : cubes) {
            RigidBody3D *body
                = Object::cast_to<RigidBody3D>(server()->entity_get_node(cube));
            seen.asleep += body != nullptr && body->is_sleeping() ? 1 : 0;
        }
    }

    void close() {
        send_alloc_evidence(arm).driven = true;
        delete stand;
        stand = nullptr;
        cube_script = nullptr;
        script = Ref<Script>();
    }

public:
    explicit SendAllocScenario(SendArm p_arm) : arm(p_arm) {
    }

    bool advance() override {
        if (netw::gd::scene_root() == nullptr || done) {
            return false;
        }
        if (frame == 0 && !open()) {
            close();
            done = true;
            return false;
        }
        if (frame == SETTLE_FRAMES) {
            Internals::store(server()).validate_every = 0;
            probe.arm();
            opened = Internals::idle_work(server());
        }
        if (frame == SETTLE_FRAMES + SEND_WINDOW) {
            probe.disarm();
            read();
            close();
            done = true;
            return false;
        }
        stand->step_ticks(1);
        ++frame;
        return true;
    }
};

NETW_FRAME_SCENARIO(SendAllocScenario, store_send_alloc_scenario, ONE_PEER);
NETW_FRAME_SCENARIO(SendAllocScenario, store_send_alone_scenario, ALONE);

TEST_CASE(
    "[Netw][Store][Frame] P1 a pile of sleeping bodies is read only "
    "by its sweep share on a steady tick, while its script and engine "
    "columns are read on every tick"
) {
    const PullEvidence &seen = pull_evidence();
    REQUIRE(seen.driven);
    const int64_t sweep = int64_t(Store::SWEEP_TICKS);
    const int64_t share = (CUBES + sweep - 1) / sweep;
    NETW_CHECK_EQ(seen.asleep, CUBES);
    NETW_CHECK_EQ(seen.window_body, seen.window_ticks * share);
    NETW_CHECK_EQ(seen.window_rested, seen.window_ticks * (CUBES - share));
    NETW_CHECK_EQ(seen.window_rows, seen.window_ticks * CUBES);
    NETW_CHECK_EQ(seen.window_script, seen.window_ticks * CUBES);
    NETW_CHECK_EQ(seen.window_engine, seen.window_ticks * CUBES);
}

TEST_CASE(
    "[Netw][Store][Frame] P3 a body woken out of a sleeping pile is "
    "read through the body port on its first awake tick"
) {
    const PullEvidence &seen = pull_evidence();
    REQUIRE(seen.driven);
    CHECK(seen.woken_read);
    NETW_CHECK_GE(seen.woken_body, int64_t(1));
    NETW_CHECK_LE(seen.woken_body, int64_t(2));
}

TEST_CASE(
    "[Netw][Store][Frame] P5 a sleeping body moved through the physics "
    "server is named by the validation mode, then sent by its sweep"
) {
    const PullEvidence &seen = pull_evidence();
    REQUIRE(seen.driven);
    NETW_CHECK_GE(seen.misses, 1);
    CHECK(seen.miss_named);
    CHECK(bool(seen.miss_gate == "rested"));
    NETW_CHECK_GT(seen.moved_changed, seen.moved_at);
    CHECK(seen.moved_quiet);
    CHECK(seen.miss_restored);
}

TEST_CASE(
    "[Netw][Store][Frame] P7 a steady pull over a sleeping pile makes "
    "no heap allocation through the extension's allocator, while the same "
    "window around it does"
) {
    const AllocEvidence &seen = alloc_evidence();
    REQUIRE(seen.driven);
    NETW_CHECK_EQ(seen.asleep, PILE);
    NETW_CHECK_EQ(seen.pulls, int64_t(ALLOC_WINDOW));
    const int64_t sweep = int64_t(Store::SWEEP_TICKS);
    NETW_CHECK_EQ(seen.rested, int64_t(0));
    NETW_CHECK_EQ(
        seen.read,
        int64_t(ALLOC_WINDOW) * ((PILE + sweep - 1) / sweep)
    );
    NETW_CHECK_GT(seen.in_window, int64_t(0));
    NETW_CHECK_EQ(seen.in_pull, int64_t(0));
}

TEST_CASE(
    "[Netw][Store][Frame] P8 a steady tick at one idle peer makes no "
    "heap allocation through the extension's allocator in the source pass, "
    "the pull, the send pass or the row encode"
) {
    const SendAllocEvidence &seen = send_alloc_evidence(ONE_PEER);
    REQUIRE(seen.driven);
    NETW_CHECK_EQ(seen.asleep, SEND_PILE);
    NETW_CHECK_EQ(seen.sends, int64_t(SEND_WINDOW));
    NETW_CHECK_GT(seen.sent_rows, int64_t(SEND_PILE));
    NETW_CHECK_EQ(seen.in_pump, int64_t(0));
    NETW_CHECK_EQ(seen.in_send, int64_t(0));
}

TEST_CASE(
    "[Netw][Store][Frame] P10 a steady tick with no peer and with one "
    "idle peer makes no heap allocation through the extension's allocator "
    "in the row frames, the carrier flush or the display"
) {
    const SendAllocEvidence &alone = send_alloc_evidence(ALONE);
    const SendAllocEvidence &paired = send_alloc_evidence(ONE_PEER);
    REQUIRE(alone.driven);
    REQUIRE(paired.driven);
    NETW_CHECK_EQ(alone.asleep, SEND_PILE);
    NETW_CHECK_EQ(alone.sent_rows, int64_t(0));
    NETW_CHECK_EQ(alone.in_pump, int64_t(0));
    NETW_CHECK_EQ(alone.in_send, int64_t(0));
    NETW_CHECK_EQ(alone.in_frames, int64_t(0));
    NETW_CHECK_EQ(alone.in_carrier, int64_t(0));
    NETW_CHECK_EQ(alone.in_display, int64_t(0));
    NETW_CHECK_GT(paired.sent_rows, int64_t(SEND_PILE));
    NETW_CHECK_EQ(paired.in_frames, int64_t(0));
    NETW_CHECK_EQ(paired.in_carrier, paired.in_transport);
    NETW_CHECK_EQ(paired.in_display, int64_t(0));
}

} // namespace TestNetwStorePull

#endif
