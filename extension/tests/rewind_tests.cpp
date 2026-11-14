#include "support/netw_test.h"

#if defined(NETW_TIER_HOSTED)

#include "support/fault_log.h"
#include "support/loopback_rig.h"
#include "support/minted_script.h"
#include "support/netw_cells.h"

#include "godot/node.hpp"
#include "godot/physics_body.hpp"
#include "netw/api/context.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/record.hpp"

namespace TestRewind {

using namespace godot;
using netw::Netw;
using netw::NetwEntity;
using netw_test::FaultWatch;
using netw_test::law_broken;
using netw_test::law_held;
using netw_test::LawRowFor;
using netw_test::LawVerdict;
using netw_test::LoopbackRig;
using netw_test::WorldDecl;

constexpr const char *SHOOTER = R"(extends Node

var offset := 0.0

func _init() -> void:
	Netw.configure_property(self, &"offset").state()

func move(tick: int) -> void:
	offset = float(tick)

func shoot(entities: Array[NetwEntity], tick: int) -> Variant:
	var me := self as Node
	return Netw.rewind(entities, tick, func() -> float: return me.offset)

func runs(entities: Array[NetwEntity], tick: int) -> int:
	var ran := [0]
	Netw.rewind(entities, tick, func() -> void: ran[0] += 1)
	return ran[0]

func write_inside(entities: Array[NetwEntity], tick: int) -> void:
	var me := self as Node
	Netw.rewind(entities, tick, func() -> void: me.offset = 999.0)

func await_inside(entities: Array[NetwEntity], tick: int) -> void:
	Netw.rewind(entities, tick, func() -> void:
		await Engine.get_main_loop().process_frame)
)";

constexpr const char *TURRET = R"(extends Node3D

var offset := 0.0:
	set(value):
		offset = value
		rotation_degrees = Vector3(value * 13.0, value * 37.0, value * 7.0)

func _init() -> void:
	Netw.configure_property(self, &"position").state()
	Netw.configure_property(self, &"quaternion").state()

func move(tick: int) -> void:
	offset = float(tick)

func look(entities: Array[NetwEntity], tick: int) -> void:
	Netw.rewind(entities, tick, func() -> void: pass)
)";

constexpr const char *GEAR = R"(extends Node

class Part extends Node:
	var offset := 0.0

	func _init() -> void:
		name = &"Part"
		Netw.configure_property(self, &"offset").state()

var stride := 0.0
var part: Part

func _init() -> void:
	part = Part.new()
	add_child(part)
	Netw.configure_property(self, &"stride").state()

func move(tick: int) -> void:
	stride = float(tick)
	part.offset = float(tick) * 10.0

func look(entities: Array[NetwEntity], tick: int) -> Array:
	var me := self
	return Netw.rewind(entities, tick, func() -> Array:
		return [me.stride, me.part.offset])
)";

constexpr const char *PLATFORM = R"(extends AnimatableBody3D

func _init() -> void:
	sync_to_physics = true
	var shape := CollisionShape3D.new()
	shape.shape = BoxShape3D.new()
	add_child(shape)
	add_child(Area3D.new())
	Netw.configure_property(self, &"position").state()

func move(tick: int) -> void:
	position = Vector3(float(tick), 0.0, 0.0)

func look(entities: Array[NetwEntity], tick: int) -> void:
	Netw.rewind(entities, tick, func() -> void: pass)
)";

constexpr const char *RUNNER = R"(extends CharacterBody3D

func _init() -> void:
	var shape := CollisionShape3D.new()
	shape.shape = BoxShape3D.new()
	add_child(shape)
	Netw.configure_property(self, &"position").state()

func move(tick: int) -> void:
	position = Vector3(float(tick), 0.0, 0.0)

func look(entities: Array[NetwEntity], tick: int) -> void:
	Netw.rewind(entities, tick, func() -> void: pass)
)";

constexpr const char *GLIDER = R"(extends Node2D

func _init() -> void:
	var visual := Node2D.new()
	visual.name = &"Visual"
	add_child(visual)
	var hitbox := Area2D.new()
	hitbox.name = &"Hitbox"
	visual.add_child(hitbox)
	var e := Netw.configure_entity(self)
	e.display.visual_root = ^"Visual"
	var glide := NetwInterpolate.new().lerp().smooth(0.0).to(&"position")
	Netw.configure_property(self, &"position").state().interpolate(glide)

func move(tick: int) -> void:
	position = Vector2(10.0 * tick, 0.0)
)";

constexpr int TICKS = 10;
constexpr int64_t AGO = 5;

struct World {
    String label;
};

typedef LawRowFor<World> Row;

Script *minted = nullptr;

Node *build_minted(const Variant &p_name) {
    Node *made = Object::cast_to<Node>(minted->call("new"));
    made->set_name(String(p_name));
    return made;
}

struct Range {
    Ref<Script> script;
    LoopbackRig rig;
    Node *body = nullptr;
    TypedArray<NetwEntity> entities;
    int64_t born = -1;
    int64_t past = -1;

    explicit Range(const char *p_source, int p_clients = 0)
        : script(netw_test::minted_script(p_source)), rig(p_clients) {
        REQUIRE(script.is_valid());
        rig.mount();
        rig.declare_world(WorldDecl().clocked(30, 0).lag_compensated());
        minted = script.ptr();
        Array args;
        args.push_back(String("Body"));
        Array types;
        types.push_back(int(Variant::STRING));
        const int route = rig.spawn_registered(
            StringName("rewound"),
            callable_mp_static(&build_minted),
            args,
            types
        );
        minted = nullptr;
        body = rig.route_node(route);
        REQUIRE(body != nullptr);
        entities.push_back(rig.server()->entity_get_view(rig.spawned_entity()));
        born = rig.server()->clock_get_tick();
        for (int tick = 1; tick <= TICKS; ++tick) {
            body->call(StringName("move"), tick);
            rig.step_ticks(1);
        }
        past = rig.server()->clock_get_tick() - AGO;
    }

    double offset() const {
        return double(body->get(StringName("offset")));
    }
};

int64_t count_body_runs = 0;

void count_body() {
    count_body_runs += 1;
}

LawVerdict no_entities(const World &) {
    const FaultWatch faults;
    count_body_runs = 0;
    const Variant answered = Netw::rewind(
        TypedArray<NetwEntity>(),
        1,
        callable_mp_static(&count_body)
    );
    if (count_body_runs != 0 || answered.get_type() != Variant::NIL) {
        return law_broken("the body ran %d time(s)", int(count_body_runs));
    }
    if (faults.errors_containing("[NW0031]") != 1) {
        return law_broken("no NW0031 named the empty list");
    }
    return law_held();
}

LawVerdict reads_the_past(const World &) {
    Range range(SHOOTER);
    const Variant answered = range.body->call(
        StringName("shoot"),
        range.entities,
        range.past
    );
    if (answered.get_type() != Variant::FLOAT) {
        return law_broken("the body's return value did not come back");
    }
    const Ref<netw::DictionaryRecord> sampled
        = Netw::sample(range.entities[0], range.past);
    if (sampled.is_null() || !sampled->has_value(StringName("offset"))) {
        return law_broken("Netw.sample holds no offset at that tick");
    }
    const double expected = double(sampled->get_value(StringName("offset")));
    if (Math::abs(double(answered) - expected) > 0.0001
        || expected >= double(TICKS)) {
        return law_broken(
            "read %f where Netw.sample holds %f",
            double(answered),
            expected
        );
    }
    if (Math::abs(range.offset() - double(TICKS)) > 0.0001) {
        return law_broken("the present %f was not put back", range.offset());
    }
    return law_held();
}

LawVerdict second_shot_reads_the_past(const World &) {
    Range range(SHOOTER);
    const Variant first = range.body->call(
        StringName("shoot"),
        range.entities,
        range.past
    );
    for (int tick = TICKS + 1; tick <= TICKS + 3; ++tick) {
        range.body->call(StringName("move"), tick);
        range.rig.step_ticks(1);
    }
    const Variant second = range.body->call(
        StringName("shoot"),
        range.entities,
        range.past
    );
    if (double(first) >= double(TICKS)
        || Math::abs(double(second) - double(first)) > 0.0001) {
        return law_broken(
            "the first shot read %f and the second %f",
            double(first),
            double(second)
        );
    }
    return law_held();
}

LawVerdict before_history(const World &) {
    Range range(SHOOTER);
    const int64_t ran = int64_t(range.body->call(
        StringName("runs"),
        range.entities,
        range.born - 20
    ));
    if (ran != 1) {
        return law_broken("the body ran %d time(s)", int(ran));
    }
    const Variant answered = range.body->call(
        StringName("shoot"),
        range.entities,
        range.born - 20
    );
    if (Math::abs(double(answered) - double(TICKS)) > 0.0001) {
        return law_broken("read %f before any history", double(answered));
    }
    return law_held();
}

LawVerdict write_inside_warns(const World &) {
    Range range(SHOOTER);
    const FaultWatch faults;
    for (int shot = 0; shot < 2; ++shot) {
        range.body->call(
            StringName("write_inside"),
            range.entities,
            range.past
        );
    }
    if (Math::abs(range.offset() - double(TICKS)) > 0.0001) {
        return law_broken("the write %f stayed", range.offset());
    }
    if (faults.warnings_containing("[NW1015]") != 1) {
        return law_broken(
            "%d warning(s) for two writes",
            int(faults.warnings_containing("[NW1015]"))
        );
    }
    return law_held();
}

LawVerdict basis_read_back_quiet(const World &) {
    Range range(TURRET);
    const FaultWatch faults;
    range.body->call(StringName("look"), range.entities, range.past);
    if (faults.warnings_containing("[NW1015]") != 0) {
        return law_broken("a body that wrote nothing warned");
    }
    return law_held();
}

LawVerdict await_warns(const World &) {
    Range range(SHOOTER);
    const FaultWatch faults;
    range.body->call(
        StringName("await_inside"),
        range.entities,
        range.past
    );
    if (faults.warnings_containing("[NW1015]") != 1) {
        return law_broken(
            "%d NW1015 warning(s)",
            int(faults.warnings_containing("[NW1015]"))
        );
    }
    if (Math::abs(range.offset() - double(TICKS)) > 0.0001) {
        return law_broken("the present %f was not put back", range.offset());
    }
    return law_held();
}

LawVerdict child_state_rewound(const World &) {
    Range range(GEAR);
    const Ref<NetwEntity> entity = range.entities[0];
    const Ref<netw::DictionaryRecord> sampled
        = Netw::sample(entity, range.past);
    if (sampled.is_null() || !sampled->has_value(StringName("stride"))
        || !sampled->has_value(StringName("Part:offset"))) {
        return law_broken("Netw.sample holds no stride or Part:offset");
    }
    const double stride = double(sampled->get_value(StringName("stride")));
    const double offset
        = double(sampled->get_value(StringName("Part:offset")));
    if (Math::abs(offset - stride * 10.0) > 0.0001 || stride >= TICKS) {
        return law_broken("sampled stride %f and offset %f", stride, offset);
    }
    const Array seen = range.body->call(
        StringName("look"),
        range.entities,
        range.past
    );
    if (seen.size() != 2 || Math::abs(double(seen[0]) - stride) > 0.0001
        || Math::abs(double(seen[1]) - offset) > 0.0001) {
        return law_broken("the rewind did not stand the child at the sample");
    }
    Node *part = range.body->get_node_or_null(NodePath("Part"));
    if (part == nullptr
        || Math::abs(double(part->get(StringName("offset"))) - TICKS * 10.0)
            > 0.0001) {
        return law_broken("the child's present was not put back");
    }
    return law_held();
}

LawVerdict hitbox_under_visual(const World &) {
    const FaultWatch faults;
    Range range(GLIDER, 1);
    const int64_t warned = faults.warnings_containing("[NW1015]");
    if (warned != 2) {
        return law_broken("%d warning(s) over two peers", int(warned));
    }
    return law_held();
}

LawVerdict synced_platform_warns(const World &) {
    Range range(PLATFORM);
    const FaultWatch faults;
    range.body->call(StringName("look"), range.entities, range.past);
    if (faults.warnings_containing("sync_to_physics") != 1) {
        return law_broken(
            "%d sync_to_physics warning(s)",
            int(faults.warnings_containing("sync_to_physics"))
        );
    }
    if (faults.warnings_containing("[NW1015]") != 1
        || faults.warnings_containing("[NW1017]") != 0) {
        return law_broken("the platform drew a warning it did not earn");
    }
    return law_held();
}

LawVerdict kinematic_needs_flush(const World &) {
    Range range(RUNNER);
    const FaultWatch faults;
    range.body->call(StringName("look"), range.entities, range.past);
    const int64_t expected = netw::gd::can_flush_kinematic() ? 0 : 1;
    if (faults.warnings_containing("[NW1017]") != expected) {
        return law_broken(
            "%d NW1017 warning(s), expected %d",
            int(faults.warnings_containing("[NW1017]")),
            int(expected)
        );
    }
    return law_held();
}

const Row ROWS[] = {
    {"Netw.rewind with no entities",
     "the body never runs and one NW0031 is reported",
     &no_entities},
    {"a lambda that reads a state property five ticks ago",
     "it returns the value of that tick, and the present is put back",
     &reads_the_past},
    {"a second shot at the same tick after the target moved on",
     "it reads what the first shot read",
     &second_shot_reads_the_past},
    {"a rewind to a tick before the entity's first recording",
     "the body runs once and reads the present",
     &before_history},
    {"a body that writes a state property, twice",
     "the write is put back and one warning names the change",
     &write_inside_warns},
    {"a body that writes nothing on a node whose quaternion follows its "
     "rotation",
     "no change is reported",
     &basis_read_back_quiet},
    {"a body that awaits",
     "one NW1015 says the rest ran after the rewind, and the present is put "
     "back",
     &await_warns},
    {"a state property declared on a child node",
     "Netw.sample holds it beside the root's, and the rewind stands both at "
     "that tick",
     &child_state_rewound},
    {"a hitbox under the display's visual root, on a server and a client",
     "each peer warns once that it moves with the drawn body",
     &hitbox_under_visual},
    {"a rewind of an AnimatableBody3D with sync_to_physics on",
     "one warning names sync_to_physics",
     &synced_platform_warns},
    {"a rewind of a body whose only collider is kinematic",
     "NW1017 is reported once on a build that cannot flush, and never on one "
     "that can",
     &kinematic_needs_flush},
};

TEST_CASE(
    "[Networked][LagComp][Rewind][SceneTree] Netw.rewind stands a body where "
    "it was, children included, puts the present back, and warns when a "
    "body or a collider cannot be rewound"
) {
    const World world = {String("a server with lag compensation")};
    for (const Row &row : ROWS) {
        NETW_CELL(row, world);
        NETW_LAW_HOLDS(row, world);
    }
}

} // namespace TestRewind

#endif
