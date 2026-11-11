#include "support/internals.h"
#include "support/netw_test.h"

#if defined(NETW_TIER_HOSTED)

#include "support/fault_log.h"
#include "support/loopback_rig.h"
#include "support/minted_script.h"
#include "support/value_flow_stand.h"

#include "godot/node.hpp"
#include "godot/scene_tree.hpp"
#include "godot/script.hpp"
#include "godot/variant.hpp"
#include "netw/api/clock_config.hpp"
#include "netw/api/context.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/property_config.hpp"
#include "netw/script/model.hpp"

namespace TestUnlistedProperty {

using namespace godot;
using namespace netw_test;

constexpr int TICKRATE = 30;
constexpr const char *SINK_ID = "unlisted_property_sink";

constexpr const char *SINK_SOURCE = R"(extends Node2D

signal poked

var store := Vector2.ZERO
var tilt := 0.0
var shown := 0
var gets := 0
var lists := 0
var listing := false

func _init() -> void:
	Netw.configure_entity(self)
	Netw.configure_property(self, &"secret", TYPE_VECTOR2) \
		.state().on_spawn()
	Netw.configure_property(self, &"lean", TYPE_FLOAT).broadcast()
	Netw.configure_property(self, &"shown").state()

func poke() -> void:
	pass

func _get(property: StringName) -> Variant:
	if property == &"secret":
		gets += 1
		return store
	if property == &"lean":
		return tilt
	if property == &"loose":
		return 1
	return null

func _set(property: StringName, value: Variant) -> bool:
	if property == &"secret":
		store = value
		return true
	if property == &"lean":
		tilt = value
		return true
	return false

func _get_property_list() -> Array[Dictionary]:
	lists += 1
	if listing:
		return [{"name": "secret", "type": TYPE_VECTOR2}]
	return []
)";

Script *sink_script = nullptr;
Vector2 first_secret;

Node *build_sink(const Variant &p_name) {
    if (sink_script == nullptr) {
        return nullptr;
    }
    Node *made = Object::cast_to<Node>(sink_script->call("new"));
    if (made != nullptr) {
        made->set_name(String(p_name));
        made->set("secret", first_secret);
        first_secret = Vector2();
    }
    return made;
}

struct Stand {
    LoopbackRig rig;
    Ref<Script> script;
    int route = 0;

    explicit Stand(const Vector2 &p_first) : rig(1) {
        script = minted_script(SINK_SOURCE);
        sink_script = script.ptr();
        first_secret = p_first;
        rig.mount();
        flow_clocks(rig, TICKRATE);
        Array types;
        types.push_back(int(Variant::STRING));
        Array args;
        args.push_back(String("Sink"));
        route = rig.spawn_registered(
            StringName(SINK_ID),
            callable_mp_static(&build_sink),
            args,
            types,
            rig.mirror_child("Arena")
        );
        rig.pump(4);
    }

    ~Stand() {
        sink_script = nullptr;
    }

    Node *at(int p_client) const {
        return rig.route_node(route, p_client);
    }
};

int64_t counted(Node *p_node, const char *p_counter) {
    return int64_t(p_node->get(StringName(p_counter)));
}

void check_secret(Node *p_node, real_t p_x, real_t p_y) {
    const Vector2 held = p_node->get("secret");
    NETW_CHECK_CLOSE(held.x, p_x, 0.001);
    NETW_CHECK_CLOSE(held.y, p_y, 0.001);
}

TEST_CASE(
    "[Networked][Repl][Hosted] UP1 a property the node does not list "
    "replicates under the type its declaration names, and once bound it is "
    "read for its value and never looked up again"
) {
    Stand stand(Vector2(3, 4));
    Node *server = stand.at(-1);
    Node *client = stand.at(0);
    REQUIRE(server != nullptr);
    REQUIRE(client != nullptr);
    check_secret(client, 3, 4);
    NETW_CHECK_EQ(
        netw::script::model::get_node_property_type(
            client,
            StringName("secret")
        ),
        int64_t(Variant::VECTOR2)
    );

    server->set("secret", Vector2(5, 6));
    server->set("shown", 9);
    stand.rig.step_ticks(4);
    NETW_CHECK_EQ(int64_t(client->get("shown")), 9);
    check_secret(client, 5, 6);

    const int64_t server_lists = counted(server, "lists");
    const int64_t client_lists = counted(client, "lists");
    const int64_t server_gets = counted(server, "gets");
    for (int step = 0; step < 10; ++step) {
        server->set("secret", Vector2(float(step), 1));
        stand.rig.step_ticks(1);
    }
    stand.rig.step_ticks(4);
    check_secret(client, 9, 1);
    NETW_CHECK_EQ(counted(server, "lists"), server_lists);
    NETW_CHECK_EQ(counted(client, "lists"), client_lists);
    NETW_CHECK_GT(counted(server, "gets"), server_gets);

    netw::NetwEntity::of(server)->set_controller(stand.rig.peer_id(0));
    stand.rig.step_ticks(4);
    client->set("lean", 2.5);
    stand.rig.step_ticks(4);
    NETW_CHECK_CLOSE(double(server->get("lean")), 2.5, 0.001);
}

TEST_CASE(
    "[Networked][Repl][Hosted] UP2 a changed property list is looked up "
    "once more, and then the binding is steady again"
) {
    Stand stand(Vector2(1, 1));
    Node *server = stand.at(-1);
    Node *client = stand.at(0);
    REQUIRE(server != nullptr);
    REQUIRE(client != nullptr);

    server->set("listing", true);
    server->notify_property_list_changed();
    server->set("secret", Vector2(7, 7));
    stand.rig.step_ticks(4);
    check_secret(client, 7, 7);

    const int64_t settled = counted(server, "lists");
    for (int step = 0; step < 6; ++step) {
        server->set("secret", Vector2(float(step), 2));
        stand.rig.step_ticks(1);
    }
    stand.rig.step_ticks(4);
    check_secret(client, 5, 2);
    NETW_CHECK_EQ(counted(server, "lists"), settled);
}

TEST_CASE(
    "[Networked][Repl][Hosted] UP3 configure_property refuses a name the "
    "node cannot read as a property, an unlisted property with no declared "
    "type, and a declared type the node contradicts, with one error each"
) {
    const Ref<Script> script = minted_script(SINK_SOURCE);
    REQUIRE(script.is_valid());
    Node *node = Object::cast_to<Node>(script->call("new"));
    REQUIRE(node != nullptr);

    const char *refused[] = {"nope", "poke", "poked"};
    for (const char *name : refused) {
        const FaultWatch faults;
        CHECK(netw::Netw::configure_property(node, StringName(name)).is_null());
        NETW_CHECK_EQ(faults.errors(), int64_t(1));
    }
    {
        const FaultWatch faults;
        CHECK(
            netw::Netw::configure_property(node, StringName("loose")).is_null()
        );
        NETW_CHECK_EQ(faults.errors(), int64_t(1));
    }
    {
        const FaultWatch faults;
        CHECK(
            netw::Netw::configure_property(
                node,
                StringName("gets"),
                int64_t(Variant::FLOAT)
            )
                .is_null()
        );
        NETW_CHECK_EQ(faults.errors(), int64_t(1));
    }
    {
        const FaultWatch faults;
        CHECK(
            netw::Netw::configure_property(
                node,
                StringName("secret"),
                int64_t(Variant::VECTOR3)
            )
                .is_null()
        );
        NETW_CHECK_EQ(faults.errors(), int64_t(1));
    }
    const FaultWatch faults;
    CHECK(netw::Netw::configure_property(node, StringName("store")).is_valid());
    NETW_CHECK_EQ(faults.errors(), int64_t(0));

    memdelete(node);
}

TEST_CASE(
    "[Networked][Repl][SceneTree] IT1 a mounted node cannot declare a "
    "property and leaves the script's declarations unchanged"
) {
    const Ref<Script> script = minted_script("extends Node2D\n");
    Node *node = Object::cast_to<Node>(script->call("new"));
    REQUIRE(node != nullptr);
    node->set_name("LateProperty");
    netw::gd::scene_root()->add_child(node);
    FaultWatch faults;
    faults.expect_error("*'LateProperty'*[NW0036]*", 1);
    CHECK(netw::Netw::configure_property(node, "position").is_null());
    CHECK(
        netw::script::model::get_property_config(script, "position").is_null()
    );
    node->get_parent()->remove_child(node);
    memdelete(node);
}

constexpr const char *HOLDER_ID = "resource_holder";

constexpr const char *STATS_SOURCE = R"(extends Resource

var health := 0
var armor := 0
)";

constexpr const char *HOLDER_SOURCE = R"(extends Node2D

var stats: Resource

func _init() -> void:
	Netw.configure_entity(self)
	Netw.configure_property(self, &"stats:health").state()
	Netw.configure_property(self, &"stats:armor").on_spawn()
)";

Script *holder_script = nullptr;
Script *stats_script = nullptr;
Ref<RefCounted> shared_stats;
int64_t first_armor = 0;

Variant made_stats(int64_t p_armor) {
    const Variant stats = stats_script->call("new");
    netw::gd::live_object(stats)->set("armor", p_armor);
    return stats;
}

Node *build_holder(const Variant &p_name) {
    Node *made = Object::cast_to<Node>(holder_script->call("new"));
    made->set_name(String(p_name));
    made->set(
        "stats",
        shared_stats.is_valid() ? Variant(shared_stats)
                                : made_stats(first_armor)
    );
    first_armor = 0;
    return made;
}

Array holder_types() {
    Array types;
    types.push_back(int(Variant::STRING));
    return types;
}

Array holder_named(const char *p_name) {
    Array args;
    args.push_back(String(p_name));
    return args;
}

struct HolderStand {
    LoopbackRig rig;
    Ref<Script> holder;
    Ref<Script> stats;
    Node *arena = nullptr;

    HolderStand() : rig(1) {
        holder = minted_script(HOLDER_SOURCE);
        stats = minted_script(STATS_SOURCE);
        holder_script = holder.ptr();
        stats_script = stats.ptr();
        rig.mount();
        flow_clocks(rig, TICKRATE);
        arena = rig.mirror_child("Arena");
    }

    ~HolderStand() {
        holder_script = nullptr;
        stats_script = nullptr;
        shared_stats.unref();
        first_armor = 0;
    }

    int spawn(const char *p_name, bool p_pump = true) {
        return rig.spawn_registered(
            StringName(HOLDER_ID),
            callable_mp_static(&build_holder),
            holder_named(p_name),
            holder_types(),
            arena,
            Variant(),
            p_pump
        );
    }
};

Object *stats_of(Node *p_node) {
    return p_node != nullptr ? netw::gd::live_object(p_node->get("stats"))
                             : nullptr;
}

int64_t stat(Node *p_node, const char *p_name) {
    Object *stats = stats_of(p_node);
    return stats != nullptr ? int64_t(stats->get(StringName(p_name))) : -1;
}

TEST_CASE(
    "[Networked][Repl][Hosted] UP4 a property inside a resource replicates "
    "into the client's own resource, and an on_spawn one reaches a late "
    "joiner"
) {
    HolderStand stand;
    first_armor = 5;
    const int route = stand.spawn("Hero");
    Node *server = stand.rig.route_node(route, -1);
    Node *client = stand.rig.route_node(route, 0);
    REQUIRE(server != nullptr);
    REQUIRE(client != nullptr);
    Object *client_stats = stats_of(client);
    REQUIRE(client_stats != nullptr);
    NETW_CHECK_EQ(stat(client, "armor"), int64_t(5));

    stats_of(server)->set("health", 12);
    stats_of(server)->set("armor", 9);
    stand.rig.step_ticks(4);
    NETW_CHECK_EQ(stat(client, "health"), int64_t(12));
    NETW_CHECK_EQ(stat(client, "armor"), int64_t(5));
    CHECK(stats_of(client) == client_stats);

    const int late = stand.rig.add_client();
    Ref<netw::NetwClockConfig> clock;
    clock.instantiate();
    clock->set_tickrate(TICKRATE);
    NETW_REQUIRE_EQ(stand.rig.client(late)->clock_initialize(clock), OK);
    Internals::clock_engine(stand.rig.client(late)).set_manual_tick(true);
    stand.rig.register_constructor(
        stand.rig.client(late),
        StringName(HOLDER_ID),
        callable_mp_static(&build_holder),
        holder_types()
    );
    stand.rig.hold(late);
    stand.rig.mount_late(late);
    stand.rig.mirror_late(late, "Arena");
    stand.rig.release(late);
    stand.rig.step_ticks(24);
    Node *joined = stand.rig.route_node(route, late);
    REQUIRE(joined != nullptr);
    NETW_CHECK_EQ(stat(joined, "armor"), int64_t(9));
    NETW_CHECK_EQ(stat(joined, "health"), int64_t(12));
}

TEST_CASE(
    "[Networked][Repl][Hosted] UP5 two entities syncing one shared resource "
    "are reported once, naming both"
) {
    HolderStand stand;
    FaultWatch faults;
    faults.expect_error("*'HeroA'*'HeroB'*'stats:health'*[NW1026]*", 1);
    faults.expect_error("*'HeroA'*'HeroB'*'stats:armor'*[NW1026]*", 1);
    shared_stats = Ref<RefCounted>(made_stats(0));
    stand.spawn("HeroA", false);
    stand.spawn("HeroB", false);
    shared_stats.unref();
    stand.rig.step_ticks(8);
}

TEST_CASE(
    "[Networked][Repl][Hosted] UP6 a resource taken away is reported once "
    "and sends nothing, and the one put back is read on the next tick"
) {
    HolderStand stand;
    const int route = stand.spawn("Hero");
    Node *server = stand.rig.route_node(route, -1);
    Node *client = stand.rig.route_node(route, 0);
    REQUIRE(server != nullptr);
    REQUIRE(client != nullptr);
    stats_of(server)->set("health", 4);
    stand.rig.step_ticks(4);
    NETW_REQUIRE_EQ(stat(client, "health"), int64_t(4));
    {
        FaultWatch faults;
        faults.expect_error("*'Hero'*'stats:health'*[NW1025]*", 1);
        server->set("stats", Variant());
        stand.rig.step_ticks(8);
        NETW_CHECK_EQ(stat(client, "health"), int64_t(4));
    }
    const Variant swapped = made_stats(0);
    netw::gd::live_object(swapped)->set("health", 6);
    server->set("stats", swapped);
    stand.rig.step_ticks(4);
    NETW_CHECK_EQ(stat(client, "health"), int64_t(6));
}

} // namespace TestUnlistedProperty

#endif
