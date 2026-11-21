#include "support/netw_test.h"

#if defined(NETW_TIER_HOSTED)

#include "support/books.h"
#include "support/fault_log.h"
#include "support/link.h"
#include "support/loopback_rig.h"
#include "support/minted_script.h"
#include "support/netw_cells.h"
#include "support/store_arrival.h"
#include "support/value_flow_stand.h"

#include <cmath>
#include <cstdint>

#include "godot/node.hpp"
#include "godot/scene_tree.hpp"
#include "godot/script.hpp"
#include "godot/spatial_node.hpp"
#include "godot/variant.hpp"
#include "netw/api/context.hpp"
#include "netw/api/despawn_config.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/entity_options.hpp"
#include "netw/api/member_config.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/persistence_config.hpp"
#include "netw/api/promise.hpp"
#include "netw/api/property_config.hpp"
#include "netw/api/quantize.hpp"
#include "netw/api/schema_core.hpp"
#include "netw/api/schema_model.hpp"
#include "netw/call_args.hpp"
#include "netw/handle_ledger.hpp"
#include "netw/property_set_builder.hpp"
#include "netw/repl/property_binding.hpp"
#include "netw/repl/property_set.hpp"
#include "netw/repl/snapshot_frame.hpp"
#include "netw/schema_model.hpp"
#include "netw/script/model.hpp"
#include "netw/store/store.hpp"
#include "netw/wire/code_row.hpp"
#include "netw/wire/plan.hpp"
#include "netw/wire/registry.hpp"
#include "netw/wire/value_row.hpp"

namespace TestProperty {

using namespace godot;
using netw::NetwControlRequest;
using netw::NetwEntity;
using netw::NetwMemberConfig;
using netw::NetwMultiplayer;
using netw::NetwPromise;
using netw::NetwPropertyConfig;
using netw::NetwQuantize;
using netw::NetwQuantizeAngle;
using netw::NetwQuantizeQuaternion;
using netw::NetwQuantizeScalar;
using netw::NetwQuantizeTransform2D;
using netw::NetwQuantizeTransform3D;
using netw::NetwSchema;
using netw::SchemaCore;
using netw::repl::PropertyBindingPtr;
using netw::repl::PropertyColumn;
using netw::repl::PropertySet;
using netw::repl::SnapshotHeader;
using netw::table::DeltaMode;
using netw::table::SchemaRecord;
using netw::wire::CodeRow;
using netw::wire::WirePlan;
using netw_test::Books;
using netw_test::DeclaredSet;
using netw_test::FaultWatch;
using netw_test::law_broken;
using netw_test::law_held;
using netw_test::LawRowFor;
using netw_test::LawVerdict;
using netw_test::LoopbackRig;

constexpr double PI_VALUE = 3.14159265358979323846;
constexpr double TAU_VALUE = PI_VALUE * 2.0;
constexpr int TICKRATE = 30;

struct World {
    String label;
};

typedef LawRowFor<World> Row;

Variant round_trip(
    const Ref<NetwQuantize> &p_quantizer,
    const Variant &p_value,
    Variant::Type p_type
) {
    netw::wire::WriteStream writing;
    if (!p_quantizer->write(writing, p_value)) {
        return Variant();
    }
    netw::wire::ReadStream reading(writing.to_bytes());
    Variant out;
    if (!p_quantizer->read(reading, p_type, out)) {
        return Variant();
    }
    return out;
}

Ref<NetwQuantizeScalar> by_bits(int p_count, double p_low, double p_high) {
    Ref<NetwQuantizeScalar> made;
    made.instantiate();
    made->set_bit_count(p_count);
    made->set_min_limit(p_low);
    made->set_max_limit(p_high);
    return made;
}

Ref<NetwQuantizeScalar> by_step(double p_step, double p_low, double p_high) {
    Ref<NetwQuantizeScalar> made;
    made.instantiate();
    made->set_min_limit(p_low);
    made->set_max_limit(p_high);
    made->set_resolution_step(p_step);
    return made;
}

Ref<NetwQuantizeAngle> angle(int p_count, bool p_centered) {
    Ref<NetwQuantizeAngle> made;
    made.instantiate();
    made->set_bit_count(p_count);
    made->set_centered_on_zero(p_centered);
    return made;
}

Ref<NetwQuantizeQuaternion> rotation_of(int p_count) {
    Ref<NetwQuantizeQuaternion> made;
    made.instantiate();
    made->set_bit_count(p_count);
    return made;
}

double angle_error(const Quaternion &p_a, const Quaternion &p_b) {
    const Quaternion a = p_a.normalized();
    const Quaternion b = p_b.normalized();
    const double dot = std::abs(
        double(a.x) * double(b.x) + double(a.y) * double(b.y)
        + double(a.z) * double(b.z) + double(a.w) * double(b.w)
    );
    return 2.0 * std::acos(std::min(1.0, std::max(-1.0, dot)));
}

double shortest_arc(double p_from, double p_to) {
    double difference = std::fmod(p_to - p_from, TAU_VALUE);
    difference = std::fmod(2.0 * difference, TAU_VALUE) - difference;
    return difference;
}

LawVerdict limits_come_back(const World &) {
    for (int count : {1, 2, 4, 8, 16}) {
        const Ref<NetwQuantizeScalar> q = by_bits(count, -1.0, 1.0);
        const double low = round_trip(q, -1.0, Variant::FLOAT);
        const double high = round_trip(q, 1.0, Variant::FLOAT);
        const double rest = round_trip(q, 0.0, Variant::FLOAT);
        if (std::abs(low + 1.0) > 1e-6 || std::abs(high - 1.0) > 1e-6) {
            return law_broken("%d bits answer %f and %f", count, low, high);
        }
        if (count > 1 && std::abs(rest) > 1e-6) {
            return law_broken("%d bits answer %f for zero", count, rest);
        }
    }
    const Ref<NetwQuantizeScalar> offset = by_bits(8, 0.0, 100.0);
    const double low = round_trip(offset, 0.0, Variant::FLOAT);
    const double high = round_trip(offset, 100.0, Variant::FLOAT);
    if (std::abs(low) > 1e-6 || std::abs(high - 100.0) > 1e-4) {
        return law_broken("0 to 100 answers %f and %f", low, high);
    }
    return law_held();
}

LawVerdict vectors_within_error(const World &) {
    const Ref<NetwQuantizeScalar> q = by_bits(8, -1.0, 1.0);
    const double scalar = round_trip(q, 0.3123, Variant::FLOAT);
    if (std::abs(scalar - 0.3123) > q->max_error(Variant::FLOAT)) {
        return law_broken("a float lands at %f", scalar);
    }
    const Vector2 flat(0.25, -0.75);
    const Vector2 flat_got = round_trip(q, flat, Variant::VECTOR2);
    if (flat_got.distance_to(flat) > q->max_error(Variant::VECTOR2)) {
        return law_broken("a Vector2 lands %f off", flat_got.distance_to(flat));
    }
    const Vector3 deep(0.25, -0.75, 0.5);
    const Vector3 deep_got = round_trip(q, deep, Variant::VECTOR3);
    if (deep_got.distance_to(deep) > q->max_error(Variant::VECTOR3)) {
        return law_broken("a Vector3 lands %f off", deep_got.distance_to(deep));
    }
    const Vector3 edge(-1.0, 0.0, -1.0);
    if (!(Vector3(round_trip(q, edge, Variant::VECTOR3)) == edge)) {
        return law_broken("a Vector3 on the limits and zero moved");
    }
    const double axis = 2.0 / 256.0 * 0.5;
    if (std::abs(q->max_error(Variant::VECTOR3) - axis * std::sqrt(3.0))
        > 1e-4) {
        return law_broken(
            "the reported Vector3 error is %f",
            q->max_error(Variant::VECTOR3)
        );
    }
    return law_held();
}

LawVerdict step_grants_a_finer_grid(const World &) {
    const Ref<NetwQuantizeScalar> q = by_step(0.5, -100.0, 100.0);
    const double granted = q->get_resolution_step();
    if (granted > 0.5) {
        return law_broken("a step of %f was granted", granted);
    }
    if (std::abs(q->max_error(Variant::FLOAT) - granted * 0.5) > 1e-4) {
        return law_broken("the error is %f", q->max_error(Variant::FLOAT));
    }
    const double got = round_trip(q, 3.3, Variant::FLOAT);
    if (std::abs(got - 3.3) > q->max_error(Variant::FLOAT)) {
        return law_broken("3.3 lands at %f", got);
    }
    const Vector3 deep(10.0, -20.5, 30.0);
    const Vector3 deep_got = round_trip(q, deep, Variant::VECTOR3);
    if (deep_got.distance_to(deep) > q->max_error(Variant::VECTOR3)) {
        return law_broken("a Vector3 lands %f off", deep_got.distance_to(deep));
    }
    const Ref<NetwQuantizeScalar> eleven = by_bits(11, -100.0, 100.0);
    const double eleven_step = eleven->get_resolution_step();
    eleven->step(eleven_step);
    if (eleven->get_bit_count() != 11) {
        return law_broken(
            "asking for 11 bits' own step answers %d bits",
            int(eleven->get_bit_count())
        );
    }
    return law_held();
}

LawVerdict angle_wraps(const World &) {
    const Ref<NetwQuantizeAngle> q = angle(8, false);
    const double half = round_trip(q, PI_VALUE, Variant::FLOAT);
    const double whole = round_trip(q, TAU_VALUE, Variant::FLOAT);
    if (std::abs(half - PI_VALUE) > 0.03 || std::abs(whole) > 0.03) {
        return law_broken("pi lands at %f and tau at %f", half, whole);
    }
    return law_held();
}

LawVerdict centered_angle_range(const World &) {
    const Ref<NetwQuantizeAngle> q = angle(16, true);
    for (double value : {-3.0, -0.5, 0.0, 0.5, 3.0}) {
        const double got = round_trip(q, value, Variant::FLOAT);
        if (std::abs(shortest_arc(value, got)) > 0.0005) {
            return law_broken("%f lands at %f", value, got);
        }
        if (got <= -PI_VALUE - 0.0001 || got > PI_VALUE + 0.0001) {
            return law_broken("%f decodes outside -pi to pi", value);
        }
    }
    const double low = round_trip(q, -PI_VALUE, Variant::FLOAT);
    if (std::abs(low - PI_VALUE) > 0.0005) {
        return law_broken("-pi lands at %f", low);
    }
    const double step = TAU_VALUE / 65536.0;
    const double below = round_trip(q, PI_VALUE - step, Variant::FLOAT);
    const double above = round_trip(q, -PI_VALUE + step, Variant::FLOAT);
    if (std::abs(shortest_arc(below, above)) >= 3.0 * step) {
        return law_broken("the seam splits %f from %f", below, above);
    }
    return law_held();
}

LawVerdict quaternion_within_error(const World &) {
    const Ref<NetwQuantizeQuaternion> q = rotation_of(12);
    const Quaternion value(Vector3(0.3, 1.0, 0.2).normalized(), 1.234);
    const Quaternion got = round_trip(q, value, Variant::QUATERNION);
    if (angle_error(value, got) > q->max_error(Variant::QUATERNION)) {
        return law_broken("it lands %f radians away", angle_error(value, got));
    }
    return law_held();
}

LawVerdict transforms_within_error(const World &) {
    Ref<NetwQuantizeTransform2D> flat;
    flat.instantiate();
    flat->set_origin_quantizer(by_step(0.25, -10.0, 10.0));
    flat->set_rotation_quantizer(angle(12, false));
    flat->set_scale_quantizer(by_step(0.125, 0.0, 4.0));
    const Transform2D
        plane(PI_VALUE * 0.25, Vector2(2.0, 0.5), 0.0, Vector2(3.25, -4.5));
    const Transform2D plane_got = round_trip(flat, plane, Variant::TRANSFORM2D);
    if (plane_got.get_origin().distance_to(plane.get_origin())
            > flat->get_origin_quantizer()->max_error(Variant::VECTOR2)
        || std::abs(plane_got.get_rotation() - plane.get_rotation()) > 0.002
        || plane_got.get_scale().distance_to(plane.get_scale())
            > flat->get_scale_quantizer()->max_error(Variant::VECTOR2)) {
        return law_broken("the Transform2D lands off its parts' grids");
    }

    const Quaternion turn(Vector3(0.2, 1.0, 0.4).normalized(), 0.9);
    const Transform3D body(
        Basis(turn).scaled_local(Vector3(1.5, 0.75, 2.0)),
        Vector3(1.25, -2.5, 3.75)
    );
    const Ref<NetwQuantizeQuaternion> spin = rotation_of(12);
    Ref<NetwQuantizeTransform3D> deep;
    deep.instantiate();
    deep->set_origin_quantizer(by_step(0.25, -10.0, 10.0));
    deep->set_rotation_quantizer(spin);
    deep->set_scale_quantizer(by_step(0.125, 0.0, 4.0));
    const Transform3D body_got = round_trip(deep, body, Variant::TRANSFORM3D);
    if (body_got.origin.distance_to(body.origin)
            > deep->get_origin_quantizer()->max_error(Variant::VECTOR3)
        || angle_error(turn, body_got.basis.get_rotation_quaternion())
            > spin->max_error(Variant::QUATERNION)
        || body_got.basis.get_scale().distance_to(body.basis.get_scale())
            > deep->get_scale_quantizer()->max_error(Variant::VECTOR3)) {
        return law_broken("the Transform3D lands off its parts' grids");
    }
    return law_held();
}

PackedByteArray spawn_args(
    const Array &p_values,
    const Array &p_quantizers,
    const Array &p_types
) {
    netw::wire::WriteStream stream;
    const bool written = netw::call_args::values_write(
        stream,
        p_values,
        p_quantizers,
        p_types,
        nullptr
    );
    if (!written || !stream.align_verify()) {
        return PackedByteArray();
    }
    return stream.to_bytes();
}

LawVerdict spawn_quantizer_needs_its_type(const World &) {
    const Array values = netw::gd::array_of(Vector2(0.5, -0.25));
    const Array quantized = netw::gd::array_of(by_bits(10, -1.0, 1.0));
    const PackedByteArray raw = spawn_args(values, Array(), Array());
    const PackedByteArray admitted = spawn_args(
        values,
        quantized,
        netw::gd::array_of(int(Variant::VECTOR2))
    );
    const PackedByteArray refused = spawn_args(
        values,
        quantized,
        netw::gd::array_of(int(Variant::STRING))
    );
    if (!(refused == raw)) {
        return law_broken("a String argument took a Vector2 quantizer");
    }
    if (admitted == raw) {
        return law_broken("a Vector2 argument ignored its quantizer");
    }
    return law_held();
}

SnapshotHeader stepping(uint64_t p_revision, uint64_t p_mask) {
    SnapshotHeader out;
    out.token = 77;
    out.revision = p_revision;
    out.distance = 1;
    out.reconcile_ack = -1;
    out.tick = -1;
    out.mask = p_mask;
    return out;
}

LawVerdict wide_step_reads_back(const World &) {
    SchemaRecord record;
    record.name = StringName("Wide");
    SchemaCore::append_column(&record, StringName("n"), SchemaCore::I64, 1);
    record.at(0)->delta = DeltaMode::LADDER;
    SchemaCore::fix(&record);
    const WirePlan plan = WirePlan::compile(record);
    const uint64_t steps[][2] = {
        {0, UINT64_MAX},
        {UINT64_MAX, UINT64_MAX - 3},
        {UINT64_MAX, 0},
    };
    for (const auto &step : steps) {
        CodeRow base = CodeRow::for_plan(plan);
        base.write(plan.column(0), 0, step[0]);
        CodeRow moved = CodeRow::for_plan(plan);
        moved.write(plan.column(0), 0, step[1]);
        const PackedByteArray frame = netw::repl::write_snapshot_row(
            stepping(2, plan.full_mask()),
            41,
            plan,
            moved,
            &base
        );
        SnapshotHeader got;
        CodeRow into;
        if (!netw::repl::read_snapshot_row(
                frame,
                41,
                plan,
                got,
                into,
                &base,
                nullptr
            )
            || !into.equals(moved)) {
            return law_broken(
                "a step from %llu to %llu read back wrong",
                (unsigned long long)step[0],
                (unsigned long long)step[1]
            );
        }
    }
    return law_held();
}

LawVerdict unknown_baseline_refused(const World &) {
    SchemaRecord record;
    record.name = StringName("Stepping");
    SchemaCore::append_column(&record, StringName("x"), SchemaCore::I16, 1);
    SchemaCore::append_column(&record, StringName("y"), SchemaCore::I16, 1);
    record.at(0)->delta = DeltaMode::LADDER;
    record.at(1)->delta = DeltaMode::LADDER;
    SchemaCore::fix(&record);
    const WirePlan plan = WirePlan::compile(record);
    CodeRow base = CodeRow::for_plan(plan);
    base.write(plan.column(0), 0, 10000);
    base.write(plan.column(1), 0, 20000);
    CodeRow moved = base;
    moved.write(plan.column(0), 0, 10003);
    const PackedByteArray frame = netw::repl::write_snapshot_row(
        stepping(2, plan.full_mask()),
        41,
        plan,
        moved,
        &base
    );
    SnapshotHeader got;
    CodeRow into = CodeRow::for_plan(plan);
    const CodeRow untouched = into;
    if (netw::repl::read_snapshot_row(frame, 41, plan, got, into, nullptr)) {
        return law_broken("a step with no baseline was read");
    }
    if (!into.equals(untouched)) {
        return law_broken("the refused row wrote into the receiver's row");
    }
    return law_held();
}

struct ColumnDecl {
    const char *key;
    int64_t lane = PropertySet::VOLATILE;
    int64_t type = SchemaCore::VARIANT;
    double step = 0.0;
};

int64_t wire_hash(Books &r_books, std::initializer_list<ColumnDecl> p_decls) {
    const DeclaredSet set = netw_test::declare(r_books);
    for (const ColumnDecl &decl : p_decls) {
        Ref<NetwQuantizeScalar> quantizer;
        if (decl.step > 0.0) {
            quantizer.instantiate();
            quantizer->limits(-512.0, 512.0);
            quantizer->step(decl.step);
        }
        PropertyColumn column = PropertyColumn::create(
            StringName(decl.key),
            quantizer,
            false,
            NetwMultiplayer::ColumnType(decl.type)
        );
        column.lane = decl.lane;
        set->bind_column(column);
    }
    return set->wire_hash();
}

LawVerdict hash_reads_layout(const World &) {
    Books books;
    const int64_t base = wire_hash(books, {{"a"}, {"b"}});
    const int64_t again = wire_hash(books, {{"a"}, {"b"}});
    const int64_t others[] = {
        wire_hash(books, {{"b"}, {"a"}}),
        wire_hash(books, {{"a"}, {"b"}, {"c"}}),
        wire_hash(books, {{"a"}, {"b", PropertySet::RETAINED}}),
    };
    if (base != again) {
        return law_broken("one list hashed twice disagrees");
    }
    for (int64_t other : others) {
        if (other == base) {
            return law_broken("a different list hashed the same");
        }
    }
    return law_held();
}

LawVerdict hash_reads_contents(const World &) {
    Books books;
    const int64_t VOL = PropertySet::VOLATILE;
    const int64_t as_float = wire_hash(books, {{"pos", VOL, SchemaCore::F64}});
    const int64_t raw = wire_hash(books, {{"pos", VOL, SchemaCore::VECTOR3}});
    const int64_t fine
        = wire_hash(books, {{"pos", VOL, SchemaCore::VECTOR3, 0.03}});
    const int64_t coarse
        = wire_hash(books, {{"pos", VOL, SchemaCore::VECTOR3, 0.5}});
    if (as_float == raw) {
        return law_broken("a float and a Vector3 hashed the same");
    }
    if (raw == fine || fine == coarse) {
        return law_broken("two quantizers hashed the same");
    }
    return law_held();
}

struct Declarations {
    Declarations() {
        netw::schema_model::clear();
    }

    ~Declarations() {
        netw::schema_model::clear();
    }
};

int64_t columns_adopted(const char *p_name) {
    Ref<NetwMultiplayer> session;
    session.instantiate();
    const RID schema = session->schema_find_or_adopt(StringName(p_name));
    return schema.is_valid() ? session->get_schema_core()->column_count(schema)
                             : -1;
}

LawVerdict schema_order_is_address(const World &) {
    const Declarations declarations;
    const Ref<NetwQuantize> none;
    const Ref<NetwSchema> schema = NetwSchema::declare("PropMob");
    const int pos = schema->vector3("pos", none, 1);
    const int hp = schema->u16("hp", 1);
    const int cooldown = schema->f32("cooldown", none, 8);
    const int again = NetwSchema::declare("PropMob")->u16("hp", 1);
    if (pos != 0 || hp != 1 || cooldown != 2) {
        return law_broken("the columns answer %d %d %d", pos, hp, cooldown);
    }
    if (again != 1) {
        return law_broken("hp declared again answers %d", again);
    }
    const int64_t adopted = columns_adopted("PropMob");
    if (adopted != 3) {
        return law_broken("a session adopts %d columns", int(adopted));
    }
    return law_held();
}

LawVerdict schema_shape_moved_refused(const World &) {
    const Declarations declarations;
    const Ref<NetwQuantize> none;
    const Ref<NetwSchema> schema = NetwSchema::declare("PropSkew");
    schema->vector3("pos", none, 1);
    const int retyped = schema->vector2("pos", none, 1);
    const int restrided = schema->vector3("pos", none, 4);
    if (retyped != -1 || restrided != -1) {
        return law_broken("the column answers %d %d", retyped, restrided);
    }
    const int64_t adopted = columns_adopted("PropSkew");
    if (adopted != 1) {
        return law_broken("a session adopts %d columns", int(adopted));
    }
    return law_held();
}

LawVerdict schema_late_column_refused(const World &) {
    const Declarations declarations;
    const Ref<NetwSchema> schema = NetwSchema::declare("PropTiming");
    schema->i64("first", 1);
    Ref<NetwMultiplayer> session;
    session.instantiate();
    const RID adopted = session->schema_find_or_adopt("PropTiming");
    int late = 0;
    {
        FaultWatch faults;
        faults.expect_error("*'PropTiming'*[NW0036]*", 1);
        late = schema->i64("late", 1);
    }
    if (late != -1) {
        return law_broken("a late column answers %d", late);
    }
    if (session->get_schema_core()->column_count(adopted) != 1) {
        return law_broken("the live session holds a late column");
    }
    session.unref();
    if (schema->i64("late", 1) != 1) {
        return law_broken("a column after the session ended was refused");
    }
    return law_held();
}

Array floats(std::initializer_list<double> p_values) {
    Array out;
    for (double value : p_values) {
        out.push_back(value);
    }
    return out;
}

struct ValueCase {
    int type;
    int stride;
    Variant value;
    Error expected;
};

LawVerdict values_checked(std::initializer_list<ValueCase> p_cases) {
    for (const ValueCase &check : p_cases) {
        const Error got = netw::SchemaBook::validate_value(
            check.type,
            check.stride,
            check.value
        );
        if (got != check.expected) {
            return law_broken(
                "type %d stride %d answers %d, not %d",
                check.type,
                check.stride,
                int(got),
                int(check.expected)
            );
        }
    }
    return law_held();
}

LawVerdict values_fit_their_column(const World &) {
    return values_checked({
        {SchemaCore::I64, 1, 7, OK},
        {SchemaCore::F32, 1, 2, OK},
        {SchemaCore::VECTOR3, 1, Vector3(1, 2, 3), OK},
        {SchemaCore::STRING, 1, String("gold"), OK},
        {SchemaCore::U8, 1, 256, ERR_PARAMETER_RANGE_ERROR},
        {SchemaCore::I8, 1, -129, ERR_PARAMETER_RANGE_ERROR},
        {SchemaCore::I16, 1, 40000, ERR_PARAMETER_RANGE_ERROR},
        {SchemaCore::VECTOR3, 1, Vector2(1, 2), ERR_INVALID_DATA},
        {SchemaCore::STRING, 1, 5, ERR_INVALID_DATA},
    });
}

LawVerdict values_fit_their_stride(const World &) {
    return values_checked({
        {SchemaCore::F32, 3, floats({1.0, 2.0, 3.0}), OK},
        {SchemaCore::F32, 3, floats({1.0, 2.0}), ERR_INVALID_DATA},
        {SchemaCore::F32, 3, 1.0, ERR_INVALID_DATA},
        {SchemaCore::F32, 0, 1.0, ERR_INVALID_PARAMETER},
    });
}

LawVerdict values_carry_no_object(const World &) {
    netw::NetwHandleLedger ledger;
    Dictionary nested;
    nested["deep"] = ledger.rid_create();
    Dictionary plain;
    plain["gold"] = 12;
    plain["where"] = Vector2(3, 4);
    Array loop;
    loop.push_back(Array());
    Array inner = loop[0];
    inner.push_back(loop);
    return values_checked({
        {SchemaCore::VARIANT, 1, netw::gd::array_of(nested), ERR_INVALID_DATA},
        {SchemaCore::VARIANT, 1, Callable(), ERR_INVALID_DATA},
        {SchemaCore::VARIANT, 1, plain, OK},
        {SchemaCore::VARIANT, 1, loop, ERR_CYCLIC_LINK},
    });
}

LawVerdict missing_property_refused(bool p_written_on_arrival) {
    FaultWatch faults;
    faults.expect_warning("*[NW0025]*", 1);
    Books books;
    const DeclaredSet pose = netw_test::declare(books);
    pose->bind_column(
        PropertyColumn::create(
            StringName("position"),
            Ref<NetwQuantize>(),
            false,
            NetwMultiplayer::ColumnType(SchemaCore::VECTOR2)
        )
    );
    pose->bind_column(
        PropertyColumn::create(
            StringName("rotation"),
            Ref<NetwQuantize>(),
            false,
            NetwMultiplayer::ColumnType(SchemaCore::F32)
        )
    );
    const SchemaRecord &schema = pose->get_volatile_schema();
    const WirePlan plan = WirePlan::compile(schema);
    CodeRow row = CodeRow::for_plan(plan);
    netw::wire::encode_scalar_row(
        schema,
        netw::gd::array_of(Vector2(3.0, 3.0), 1.0),
        row
    );
    SnapshotHeader header;
    header.token = 41;
    header.revision = 1;
    header.distance = 0;
    header.tick = -1;
    header.reconcile_ack = -1;
    header.mask = plan.full_mask();
    const PackedByteArray frame
        = netw::repl::write_snapshot_row(header, -1, plan, row, nullptr);

    Node *bare = memnew(Node);
    const PropertyBindingPtr binding = netw_test::bind(pose, bare);
    netw_test::LandedLog log;
    log.watch(binding);
    binding->write_gate = p_written_on_arrival;
    netw_test::StoreArrival arrival(binding);
    const bool settled = arrival.settle(frame, -1, false);
    const netw::store::RowAddress at = arrival.at(0);
    const bool landed = arrival.world().table(at.table).holds_values(at.row);
    memdelete(bare);
    if (settled || landed || log.count != 0) {
        return law_broken(
            "settled %d, landed %d, notified %d times",
            int(settled),
            int(landed),
            int(log.count)
        );
    }
    return law_held();
}

LawVerdict changed_quantizer_told_once(const World &) {
    FaultWatch faults;
    faults.expect_error("*NetwQuantizeScalar*[NW1045]*", 1);
    const Ref<NetwQuantizeScalar> grid = by_bits(8, -1.0, 1.0);
    PropertyColumn::create(
        StringName("speed"),
        grid,
        false,
        NetwMultiplayer::ColumnType(SchemaCore::F64)
    );
    grid->bits(8);
    grid->limits(-1.0, 1.0);
    if (faults.errors() != 0) {
        return law_broken("a setting set to its own value was reported");
    }
    grid->bits(10);
    grid->limits(0.0, 2.0);
    if (faults.errors() != 1) {
        return law_broken(
            "two changes were reported %d times",
            faults.errors()
        );
    }
    return law_held();
}

LawVerdict changed_transform_part_told_once(const World &) {
    FaultWatch faults;
    faults.expect_error("*NetwQuantizeScalar*[NW1045]*", 1);
    faults.expect_error("*NetwQuantizeTransform3D*[NW1045]*", 1);
    const Ref<NetwQuantizeScalar> place = by_bits(8, -10.0, 10.0);
    Ref<NetwQuantizeTransform3D> pose;
    pose.instantiate();
    pose->set_origin_quantizer(place);
    PropertyColumn::create(
        StringName("pose"),
        pose,
        false,
        NetwMultiplayer::ColumnType(SchemaCore::VARIANT)
    );
    pose->set_origin_quantizer(place);
    place->bits(10);
    const Ref<NetwQuantizeQuaternion> spin = rotation_of(12);
    pose->set_rotation_quantizer(spin);
    pose->set_scale_quantizer(by_bits(8, 0.0, 4.0));
    pose->set_rotation_quantizer(rotation_of(10));
    spin->bits(14);
    if (faults.errors() != 2) {
        return law_broken(
            "a changed part, three swapped parts and a part swapped out "
            "were reported %d times",
            faults.errors()
        );
    }
    return law_held();
}

LawVerdict missing_property_lands_nothing(const World &) {
    return missing_property_refused(true);
}

LawVerdict missing_predicted_property_lands_nothing(const World &) {
    return missing_property_refused(false);
}

class Tenths : public NetwQuantize {
public:
    bool supports_type(Variant::Type p_type) const override {
        return p_type == Variant::FLOAT;
    }

    int bit_width(Variant::Type) const override {
        return 8;
    }

    int stride(Variant::Type) const override {
        return 1;
    }

    int64_t encode(const Variant &p_value, int) const override {
        return int64_t(std::round(double(p_value) * 10.0));
    }

    Variant decode(
        const PackedInt64Array &p_codes,
        Variant::Type
    ) const override {
        return double(p_codes[0]) / 10.0;
    }
};

LawVerdict custom_quantizer_lands(const World &) {
    Books books;
    const DeclaredSet pose = netw_test::declare(books);
    Ref<Tenths> tenths;
    tenths.instantiate();
    pose->bind_column(
        PropertyColumn::create(
            StringName("rotation"),
            tenths,
            false,
            NetwMultiplayer::ColumnType(SchemaCore::F32)
        )
    );
    const SchemaRecord &schema = pose->get_volatile_schema();
    const WirePlan plan = WirePlan::compile(schema);
    CodeRow row = CodeRow::for_plan(plan);
    netw::wire::encode_scalar_row(schema, netw::gd::array_of(2.5), row);
    SnapshotHeader header;
    header.token = 41;
    header.revision = 1;
    header.distance = 0;
    header.tick = -1;
    header.reconcile_ack = -1;
    header.mask = plan.full_mask();
    const PackedByteArray frame
        = netw::repl::write_snapshot_row(header, -1, plan, row, nullptr);

    Node2D *node = memnew(Node2D);
    const PropertyBindingPtr binding = netw_test::bind(pose, node);
    netw_test::StoreArrival arrival(binding);
    const bool settled = arrival.settle(frame, -1, false);
    const double landed = double(arrival.value(StringName("rotation")));
    memdelete(node);
    if (!settled || landed != 2.5) {
        return law_broken(
            "settled %d, and 2.5 written read %f",
            int(settled),
            landed
        );
    }
    return law_held();
}

const Row CODEC_ROWS[] = {
    {"a bit quantizer of 1 to 16 bits over -1 to 1, and 8 bits over 0 to 100",
     "both limits come back exact, and zero too above one bit",
     &limits_come_back},
    {"a float, a Vector2 and a Vector3 through 8 bits over -1 to 1",
     "each lands within the error the quantizer reports, and a value on the "
     "limits or at zero lands exact",
     &vectors_within_error},
    {"a step of 0.5 asked over -100 to 100",
     "the grid granted is at least that fine and values land within half of "
     "it, and asking for 11 bits' own step keeps 11 bits",
     &step_grants_a_finer_grid},
    {"an 8 bit angle at pi and at tau",
     "pi lands on pi and tau wraps to zero",
     &angle_wraps},
    {"a centered 16 bit angle on both sides of the seam at pi",
     "it decodes between -pi and pi, -pi lands on pi, and neighbours across "
     "the seam stay neighbours",
     &centered_angle_range},
    {"a quantizer a declared property holds, set again and then changed",
     "setting a value it already has is quiet, and changing it is reported "
     "once",
     &changed_quantizer_told_once},
    {"a Transform3D quantizer a declared property holds, its origin "
     "quantizer changed, parts swapped, and a swapped out part changed",
     "the changed part is reported once, the Transform3D once, and the "
     "part it no longer uses not at all",
     &changed_transform_part_told_once},
    {"a 12 bit quaternion",
     "the rotation lands within the error the quantizer reports",
     &quaternion_within_error},
    {"a Transform2D and a Transform3D of quantized parts",
     "origin, rotation and scale each land within their part's error",
     &transforms_within_error},
    {"a spawn argument whose quantizer its declared type does not admit",
     "the argument is sent unquantized, and a Vector2 argument is quantized",
     &spawn_quantizer_needs_its_type},
    {"a 64 bit property stepping across zero and past the signed ceiling",
     "the receiver reads the value the sender held",
     &wide_step_reads_back},
    {"a row stepped from a baseline the receiver does not hold",
     "the row is refused whole and the receiver's values stay",
     &unknown_baseline_refused},
    {"two peers whose property lists differ in order, membership or lane",
     "their wire hashes differ, and one list hashes the same twice",
     &hash_reads_layout},
    {"two peers whose property differs in type or quantizer",
     "their wire hashes differ",
     &hash_reads_contents},
    {"a NetwSchema declaring pos, hp and cooldown, then hp again",
     "each column answers its declaration order, and the repeat answers hp",
     &schema_order_is_address},
    {"a NetwSchema column declared again with another type or stride",
     "the redeclaration answers -1 and the schema keeps one column",
     &schema_shape_moved_refused},
    {"a NetwSchema column declared while a session holds the schema",
     "it answers -1 with one error, and is taken once the session ends",
     &schema_late_column_refused},
    {"a value out of its column's range or of another type",
     "the column refuses it",
     &values_fit_their_column},
    {"a value with more or fewer elements than its column's stride",
     "the column refuses it",
     &values_fit_their_stride},
    {"a stored value hiding an object or nesting itself",
     "the object is refused and the loop ends with ERR_CYCLIC_LINK",
     &values_carry_no_object},
    {"a row naming a property the receiving node does not carry",
     "nothing lands, nobody is told, and one warning names it",
     &missing_property_lands_nothing},
    {"the same row for a node whose arrivals prediction writes",
     "nothing lands, nobody is told, and one warning names it",
     &missing_predicted_property_lands_nothing},
    {"a row of a property a game's own quantizer codes",
     "the receiver reads the value the sender wrote",
     &custom_quantizer_lands},
};

TEST_CASE(
    "[Netw][Codec] a quantized value lands on its grid, and a row "
    "decodes only against what the receiver declared and holds"
) {
    const World world = {String("values with no session")};
    for (const Row &row : CODEC_ROWS) {
        NETW_CELL(row, world);
        NETW_LAW_HOLDS(row, world);
    }
}

enum Policy {
    POLICY_UNDECLARED,
    POLICY_ASKS_AUTHORITY,
    POLICY_ASKS_CONTROLLER,
};

struct FieldDecl {
    StringName key;
    bool in_state = false;
    bool in_input = false;
    bool in_broadcast = false;
    bool retained = false;
    bool persisted = false;
    bool server_only = false;
    bool quantized = false;
    int64_t window = -1;
    Policy policy = POLICY_UNDECLARED;

    bool marked_for(int64_t p_record) const {
        if (int(in_state) + int(in_input) + int(in_broadcast) != 1) {
            return false;
        }
        if (p_record == PropertySet::RECORD_INPUT) {
            return in_input;
        }
        if (p_record == PropertySet::RECORD_BROADCAST) {
            return in_broadcast;
        }
        return in_state;
    }
};

struct Declared {
    String label;
    int64_t record = PropertySet::RECORD_STATE;
    Vector<FieldDecl> fields;
};

FieldDecl field(const char *p_key, bool p_state, bool p_input, bool p_cast) {
    FieldDecl out;
    out.key = StringName(p_key);
    out.in_state = p_state;
    out.in_input = p_input;
    out.in_broadcast = p_cast;
    return out;
}

FieldDecl state_field(const char *p_key) {
    return field(p_key, true, false, false);
}

FieldDecl input_field(const char *p_key) {
    return field(p_key, false, true, false);
}

FieldDecl broadcast_field(const char *p_key) {
    return field(p_key, false, false, true);
}

FieldDecl retained_field(const char *p_key) {
    FieldDecl out = field(p_key, false, false, false);
    out.retained = true;
    return out;
}

Declared declared(
    const char *p_label,
    int64_t p_record,
    std::initializer_list<FieldDecl> p_fields
) {
    Declared out;
    out.label = p_label;
    out.record = p_record;
    for (const FieldDecl &each : p_fields) {
        out.fields.push_back(each);
    }
    return out;
}

FieldDecl with(FieldDecl p_field, void (*p_change)(FieldDecl &)) {
    p_change(p_field);
    return p_field;
}

Ref<NetwPropertyConfig> compile(const FieldDecl &p_field) {
    Ref<NetwPropertyConfig> config;
    config.instantiate();
    config->set_is_property(true);
    if (p_field.in_state) {
        config->state();
    }
    if (p_field.in_input) {
        config->input();
    }
    if (p_field.in_broadcast) {
        config->broadcast();
    }
    if (p_field.retained) {
        config->retained();
    }
    if (p_field.persisted) {
        config->persisted(StringName("value"));
    }
    if (p_field.server_only) {
        config->audience(true);
    }
    if (p_field.policy == POLICY_ASKS_AUTHORITY) {
        config->authority();
    }
    if (p_field.policy == POLICY_ASKS_CONTROLLER) {
        config->controller();
    }
    if (p_field.window >= 0) {
        config->windowed(p_field.window);
    }
    if (p_field.quantized) {
        Ref<NetwQuantizeScalar> codec;
        codec.instantiate();
        config->quantize(netw::gd::array_of(codec));
    }
    return config;
}

struct Derived {
    const Declared &declared;
    PropertySet set;
    bool made = false;
    Vector<FieldDecl> owed;

    explicit Derived(const Declared &p_declared) : declared(p_declared) {
        Dictionary configs;
        for (const FieldDecl &each : declared.fields) {
            configs[each.key] = compile(each);
            if (each.marked_for(declared.record)) {
                owed.push_back(each);
            }
        }
        made = netw::property_set_builder::from_property_configs(
            set,
            configs,
            declared.record
        );
    }
};

typedef LawRowFor<Derived> DerivationLaw;

LawVerdict law_collected(const Derived &p_run) {
    if (p_run.owed.is_empty()) {
        return p_run.made ? law_broken("a set derived from no mark")
                          : law_held();
    }
    if (!p_run.made) {
        return law_broken("%d marked fields derived no set", p_run.owed.size());
    }
    const TypedArray<StringName> keys = p_run.set.keys();
    if (keys.size() != p_run.owed.size()) {
        return law_broken(
            "the set names %d fields for %d marks",
            int(keys.size()),
            p_run.owed.size()
        );
    }
    for (int index = 0; index < keys.size(); ++index) {
        if (StringName(keys[index]) != p_run.owed[index].key) {
            return law_broken(
                "field %d reads '%s'",
                index,
                String(keys[index]).utf8().get_data()
            );
        }
    }
    return law_held();
}

LawVerdict law_presets(const Derived &p_run) {
    if (!p_run.made) {
        return law_held();
    }
    const PropertySet &set = p_run.set;
    const int64_t record = p_run.declared.record;
    const int64_t stamp = record == PropertySet::RECORD_STATE
        ? PropertySet::STAMP_TICK_ACK
        : PropertySet::STAMP_TICK;
    if (set.get_record() != record || set.get_profile() != PropertySet::STAMPED
        || set.get_cadence() != PropertySet::TICK
        || set.get_channel() != netw::wire::CHANNEL_SYNC
        || set.get_stamp() != stamp) {
        return law_broken(
            "record %d profile %d cadence %d channel %d stamp %d",
            int(set.get_record()),
            int(set.get_profile()),
            int(set.get_cadence()),
            int(set.get_channel()),
            int(set.get_stamp())
        );
    }
    return law_held();
}

LawVerdict law_reach(const Derived &p_run) {
    if (!p_run.made) {
        return law_held();
    }
    const int64_t record = p_run.declared.record;
    bool server_only = false;
    Policy asked = POLICY_UNDECLARED;
    for (const FieldDecl &each : p_run.owed) {
        server_only = server_only || each.server_only;
        asked = each.policy != POLICY_UNDECLARED ? each.policy : asked;
    }
    int64_t audience = record == PropertySet::RECORD_INPUT
        ? PropertySet::AUDIENCE_SERVER_ONLY
        : PropertySet::AUDIENCE_PUBLIC;
    if (server_only && record == PropertySet::RECORD_STATE) {
        audience = PropertySet::AUDIENCE_SERVER_ONLY;
    }
    int64_t policy = record == PropertySet::RECORD_STATE
        ? NetwMemberConfig::POLICY_AUTHORITY
        : NetwMemberConfig::POLICY_CONTROLLER;
    if (asked == POLICY_ASKS_AUTHORITY) {
        policy = NetwMemberConfig::POLICY_AUTHORITY;
    }
    if (asked == POLICY_ASKS_CONTROLLER) {
        policy = NetwMemberConfig::POLICY_CONTROLLER;
    }
    if (p_run.set.get_audience() != audience
        || p_run.set.get_policy() != policy) {
        return law_broken(
            "audience %d policy %d, owed %d and %d",
            int(p_run.set.get_audience()),
            int(p_run.set.get_policy()),
            int(audience),
            int(policy)
        );
    }
    return law_held();
}

LawVerdict law_knobs(const Derived &p_run) {
    if (!p_run.made) {
        return law_held();
    }
    const int64_t record = p_run.declared.record;
    int64_t window = record == PropertySet::RECORD_INPUT ? 2 : 0;
    for (const FieldDecl &each : p_run.owed) {
        if (each.window >= 0 && record != PropertySet::RECORD_BROADCAST) {
            window = each.window;
        }
    }
    if (p_run.set.get_window() != window) {
        return law_broken(
            "window %d, owed %d",
            int(p_run.set.get_window()),
            int(window)
        );
    }
    return law_held();
}

LawVerdict law_lanes(const Derived &p_run) {
    if (!p_run.made) {
        return law_held();
    }
    if (int(p_run.set.columns.size()) != p_run.owed.size()) {
        return law_broken("%d columns", int(p_run.set.columns.size()));
    }
    const Array quantizers = p_run.set.quantizers();
    for (int index = 0; index < p_run.owed.size(); ++index) {
        const FieldDecl &owed = p_run.owed[index];
        const PropertyColumn &column = p_run.set.columns[index];
        const int64_t lane
            = owed.retained ? PropertySet::RETAINED : PropertySet::VOLATILE;
        const bool quantized = Ref<NetwQuantize>(quantizers[index]).is_valid();
        if (column.lane != lane || column.watch != owed.retained
            || quantized != owed.quantized) {
            return law_broken(
                "'%s' lane %d watch %d quantized %d",
                String(owed.key).utf8().get_data(),
                int(column.lane),
                int(column.watch),
                int(quantized)
            );
        }
    }
    return law_held();
}

const DerivationLaw DERIVATION_LAWS[] = {
    {"collected",
     "the set names exactly the properties carrying the kind's mark, in "
     "declaration order",
     &law_collected},
    {"presets",
     "the kind stamps its own presets and rides the shared sync channel",
     &law_presets},
    {"reach",
     "the kind's audience and write policy stand until a property narrows "
     "them",
     &law_reach},
    {"knobs",
     "the last property to name a window owns it and a broadcast keeps none",
     &law_knobs},
    {"lanes",
     "each property rides the lane it declared, watches when retained, and "
     "keeps its own quantizer",
     &law_lanes},
};

void set_cast(FieldDecl &r_field) {
    r_field.in_broadcast = true;
}

void set_retained(FieldDecl &r_field) {
    r_field.retained = true;
}

void set_quantized(FieldDecl &r_field) {
    r_field.quantized = true;
}

void set_window_two(FieldDecl &r_field) {
    r_field.window = 2;
}

void set_window_five(FieldDecl &r_field) {
    r_field.window = 5;
}

void set_server_reach(FieldDecl &r_field) {
    r_field.server_only = true;
    r_field.window = 3;
}

void set_controller_secret(FieldDecl &r_field) {
    r_field.server_only = true;
    r_field.policy = POLICY_ASKS_CONTROLLER;
}

void set_persisted_only(FieldDecl &r_field) {
    r_field.retained = false;
    r_field.persisted = true;
}

TEST_CASE(
    "[Netw][Sync] the properties a node declares derive one property "
    "set a kind, in declaration order and on the lanes they asked for"
) {
    FaultWatch faults;
    faults.expect_error("*NetwPropertyConfig.windowed cannot*[NW0043]*", 1);
    faults.expect_error("*NetwPropertyConfig.audience cannot*[NW0043]*", 1);
    faults.expect_error("*broadcast together*[NW0043]*", 1);
    faults.expect_warning("*The window of*[NW0035]*", 1);

    const int64_t STATE = PropertySet::RECORD_STATE;
    const int64_t INPUT = PropertySet::RECORD_INPUT;
    const int64_t CAST = PropertySet::RECORD_BROADCAST;
    const Declared CORPUS[] = {
        declared(
            "two states around an unmarked property",
            STATE,
            {state_field("position"),
             retained_field("health"),
             state_field("rotation")}
        ),
        declared(
            "two inputs",
            INPUT,
            {input_field("move"), input_field("aim")}
        ),
        declared(
            "a broadcast asking for an audience and a window",
            CAST,
            {with(broadcast_field("aim"), &set_server_reach)}
        ),
        declared(
            "a broadcast beside a state that is also broadcast",
            CAST,
            {with(state_field("position"), &set_cast),
             broadcast_field("aim_dir")}
        ),
        declared(
            "a state that is also broadcast",
            STATE,
            {with(state_field("position"), &set_cast)}
        ),
        declared(
            "nothing marked",
            STATE,
            {retained_field("health"),
             with(retained_field("gold"), &set_persisted_only)}
        ),
        declared(
            "a volatile and a retained state",
            STATE,
            {state_field("position"),
             with(state_field("inventory"), &set_retained)}
        ),
        declared(
            "one quantized state of two",
            STATE,
            {with(state_field("position"), &set_quantized),
             state_field("rotation")}
        ),
        declared(
            "a state narrowed to the server and the controller",
            STATE,
            {with(state_field("secret"), &set_controller_secret)}
        ),
        declared(
            "two inputs naming different windows",
            INPUT,
            {with(input_field("move"), &set_window_two),
             with(input_field("aim"), &set_window_five)}
        ),
    };
    for (const Declared &scenario : CORPUS) {
        const Derived run(scenario);
        for (const DerivationLaw &law : DERIVATION_LAWS) {
            NETW_CELL(law, scenario);
            NETW_LAW_HOLDS(law, run);
        }
    }
}

Script *built_script = nullptr;
Vector2 first_secret;
Ref<RefCounted> shared_stats;
Script *stats_script = nullptr;
int64_t first_armor = 0;

Node *build_minted(const Variant &p_name) {
    Node *made = Object::cast_to<Node>(built_script->call("new"));
    made->set_name(String(p_name));
    return made;
}

Node *build_sink(const Variant &p_name) {
    Node *made = build_minted(p_name);
    made->set("secret", first_secret);
    first_secret = Vector2();
    return made;
}

Variant made_stats(int64_t p_armor) {
    const Variant stats = stats_script->call("new");
    netw::gd::live_object(stats)->set("armor", p_armor);
    return stats;
}

Node *build_holder(const Variant &p_name) {
    Node *made = build_minted(p_name);
    made->set(
        "stats",
        shared_stats.is_valid() ? Variant(shared_stats)
                                : made_stats(first_armor)
    );
    first_armor = 0;
    return made;
}

struct Stand {
    LoopbackRig rig;
    Ref<Script> script;
    Ref<Script> stats;
    Node *arena = nullptr;

    Stand(int p_clients, const char *p_source) : rig(p_clients) {
        script = netw_test::minted_script(p_source);
        built_script = script.ptr();
        rig.mount();
        netw_test::flow_clocks(rig, TICKRATE);
        arena = rig.mirror_child("Arena");
    }

    ~Stand() {
        built_script = nullptr;
        stats_script = nullptr;
        shared_stats.unref();
        first_armor = 0;
        first_secret = Vector2();
    }

    int spawn(
        Node *(*p_build)(const Variant &),
        const char *p_name,
        bool p_pump = true
    ) {
        return rig.spawn_registered(
            StringName("property_body"),
            callable_mp_static(p_build),
            netw::gd::array_of(String(p_name)),
            netw::gd::array_of(int(Variant::STRING)),
            arena,
            0,
            p_pump
        );
    }

    Node *at(int p_route, int p_client) const {
        return rig.route_node(p_route, p_client);
    }
};

constexpr const char *SINK_SOURCE = R"(extends Node2D

signal poked

var store := Vector2.ZERO
var tilt := 0.0
var shown := 0

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
	return []
)";

bool near(const Variant &p_held, const Vector2 &p_want) {
    return Vector2(p_held).distance_to(p_want) < 0.001;
}

LawVerdict unlisted_property_replicates(const World &) {
    Stand stand(1, SINK_SOURCE);
    first_secret = Vector2(3, 4);
    const int route = stand.spawn(&build_sink, "Sink");
    stand.rig.pump(4);
    Node *server = stand.at(route, -1);
    Node *client = stand.at(route, 0);
    if (server == nullptr || client == nullptr) {
        return law_broken("the sink did not spawn on both peers");
    }
    if (!near(client->get("secret"), Vector2(3, 4))) {
        return law_broken("the spawn value did not arrive");
    }
    server->set("secret", Vector2(5, 6));
    server->set("shown", 9);
    stand.rig.step_ticks(4);
    if (!near(client->get("secret"), Vector2(5, 6))
        || int(client->get("shown")) != 9) {
        return law_broken("a change did not arrive");
    }
    NetwEntity::of(server)->set_controller(stand.rig.peer_id(0));
    stand.rig.step_ticks(4);
    client->set("lean", 2.5);
    stand.rig.step_ticks(4);
    if (std::abs(double(server->get("lean")) - 2.5) > 0.001) {
        return law_broken("the controller's broadcast never reached it");
    }
    return law_held();
}

bool refused_and_inert(const Ref<NetwPropertyConfig> &p_config) {
    return p_config.is_valid() && p_config->is_refused()
        && !p_config->state()->get_in_state_set();
}

LawVerdict unreadable_declarations_refused(const World &) {
    const Ref<Script> script = netw_test::minted_script(SINK_SOURCE);
    Node *node = Object::cast_to<Node>(script->call("new"));
    struct Refusal {
        const char *name;
        int64_t type;
    };
    const Refusal refused[] = {
        {"nope", -1},
        {"poke", -1},
        {"poked", -1},
        {"loose", -1},
        {"shown", int64_t(Variant::FLOAT)},
        {"secret", int64_t(Variant::VECTOR3)},
    };
    for (const Refusal &each : refused) {
        const FaultWatch faults;
        const bool taken = !refused_and_inert(
            each.type < 0
                ? netw::Netw::configure_property(node, StringName(each.name))
                : netw::Netw::configure_property(
                      node,
                      StringName(each.name),
                      each.type
                  )
        );
        if (taken || faults.errors() != 1) {
            memdelete(node);
            return law_broken(
                "'%s' taken %d with %d errors",
                each.name,
                int(taken),
                int(faults.errors())
            );
        }
    }
    const FaultWatch faults;
    const Ref<NetwPropertyConfig> store
        = netw::Netw::configure_property(node, StringName("store"));
    const bool plain = store.is_valid() && !store->is_refused();
    memdelete(node);
    if (!plain || faults.errors() != 0) {
        return law_broken("a plain var was refused");
    }
    return law_held();
}

LawVerdict mounted_node_refused(const World &) {
    const Ref<Script> script = netw_test::minted_script("extends Node2D\n");
    Node *node = Object::cast_to<Node>(script->call("new"));
    node->set_name("LateProperty");
    netw::gd::scene_root()->add_child(node);
    bool taken = false;
    {
        FaultWatch faults;
        faults.expect_error("*'LateProperty'*[NW0036]*", 1);
        taken = !refused_and_inert(
            netw::Netw::configure_property(node, "position")
        );
    }
    node->get_parent()->remove_child(node);
    memdelete(node);
    return taken ? law_broken("a mounted node declared a property")
                 : law_held();
}

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

struct HolderStand : Stand {
    HolderStand() : Stand(1, HOLDER_SOURCE) {
        stats = netw_test::minted_script(STATS_SOURCE);
        stats_script = stats.ptr();
    }
};

int64_t stat(Node *p_node, const char *p_name) {
    Object *stats = p_node != nullptr
        ? netw::gd::live_object(p_node->get("stats"))
        : nullptr;
    return stats != nullptr ? int64_t(stats->get(StringName(p_name))) : -1;
}

LawVerdict resource_property_replicates(const World &) {
    HolderStand stand;
    first_armor = 5;
    const int route = stand.spawn(&build_holder, "Hero");
    Node *server = stand.at(route, -1);
    Node *client = stand.at(route, 0);
    if (server == nullptr || client == nullptr) {
        return law_broken("the holder did not spawn on both peers");
    }
    const Variant client_stats = client->get("stats");
    if (stat(client, "armor") != 5) {
        return law_broken("the spawn armor is %d", int(stat(client, "armor")));
    }
    netw::gd::live_object(server->get("stats"))->set("health", 12);
    netw::gd::live_object(server->get("stats"))->set("armor", 9);
    stand.rig.step_ticks(4);
    if (stat(client, "health") != 12 || stat(client, "armor") != 5) {
        return law_broken(
            "the client reads health %d armor %d",
            int(stat(client, "health")),
            int(stat(client, "armor"))
        );
    }
    if (!(client->get("stats") == client_stats)) {
        return law_broken("the client's resource was replaced");
    }

    const int late = stand.rig.late_client(StringName());
    stand.rig.hold(late);
    stand.rig.register_constructor(
        stand.rig.client(late),
        StringName("property_body"),
        callable_mp_static(&build_holder),
        netw::gd::array_of(int(Variant::STRING))
    );
    stand.rig.mirror_late(late, "Arena");
    stand.rig.release(late);
    stand.rig.step_ticks(24);
    Node *joined = stand.at(route, late);
    if (stat(joined, "armor") != 9 || stat(joined, "health") != 12) {
        return law_broken(
            "the late joiner reads health %d armor %d",
            int(stat(joined, "health")),
            int(stat(joined, "armor"))
        );
    }
    return law_held();
}

LawVerdict shared_resource_reported(const World &) {
    HolderStand stand;
    FaultWatch faults;
    faults.expect_error("*'HeroA'*'HeroB'*'stats:health'*[NW1026]*", 1);
    faults.expect_error("*'HeroA'*'HeroB'*'stats:armor'*[NW1026]*", 1);
    shared_stats = Ref<RefCounted>(made_stats(0));
    stand.spawn(&build_holder, "HeroA", false);
    stand.spawn(&build_holder, "HeroB", false);
    shared_stats.unref();
    stand.rig.step_ticks(8);
    return law_held();
}

LawVerdict resource_taken_away(const World &) {
    HolderStand stand;
    const int route = stand.spawn(&build_holder, "Hero");
    Node *server = stand.at(route, -1);
    Node *client = stand.at(route, 0);
    if (server == nullptr || client == nullptr) {
        return law_broken("the holder did not spawn on both peers");
    }
    netw::gd::live_object(server->get("stats"))->set("health", 4);
    stand.rig.step_ticks(4);
    if (stat(client, "health") != 4) {
        return law_broken("the first health did not arrive");
    }
    {
        FaultWatch faults;
        faults.expect_error("*'Hero'*'stats:health'*[NW1025]*", 1);
        server->set("stats", Variant());
        stand.rig.step_ticks(8);
    }
    if (stat(client, "health") != 4) {
        return law_broken("a missing resource changed the client");
    }
    const Variant swapped = made_stats(0);
    netw::gd::live_object(swapped)->set("health", 6);
    server->set("stats", swapped);
    stand.rig.step_ticks(4);
    if (stat(client, "health") != 6) {
        return law_broken("the resource put back was not read");
    }
    return law_held();
}

constexpr const char *BOARD_SOURCE = R"(extends Node2D

var ink := PackedVector2Array()
var breaks := PackedInt32Array()
var pose := Transform3D()

func _init() -> void:
	var e := Netw.configure_entity(self)
	e.authority = NetwEntity.AUTHORITY_CONTROLLER
	e.transfer = NetwEntity.TRANSFER_IMMEDIATE
	Netw.configure_property(self, &"ink").broadcast()
	Netw.configure_property(self, &"breaks").broadcast()
	Netw.configure_property(self, &"pose").broadcast()

func draw(point: Vector2) -> void:
	ink.append(point)
)";

struct Board : Stand {
    int route = 0;

    Board() : Stand(2, BOARD_SOURCE) {
        route = spawn(&build_minted, "Whiteboard");
        rig.pump(4);
    }

    Node *node(int p_client) const {
        return at(route, p_client);
    }

    bool everyone_holds(const char *p_property, const Variant &p_value) const {
        for (int client = -1; client < 2; ++client) {
            Node *held = node(client);
            if (held == nullptr || !(held->get(p_property) == p_value)) {
                return false;
            }
        }
        return true;
    }

    void draw(int p_client, int p_from, int p_to) {
        for (int at = p_from; at < p_to; ++at) {
            node(p_client)->call("draw", Vector2(float(at), float(at * 2)));
        }
    }
};

PackedVector2Array stroke(int p_points) {
    PackedVector2Array made;
    for (int at = 0; at < p_points; ++at) {
        made.push_back(Vector2(float(at), float(at * 2)));
    }
    return made;
}

LawVerdict packed_arrays_broadcast(const World &) {
    Board board;
    if (board.node(0) == nullptr || board.node(1) == nullptr) {
        return law_broken("the board did not spawn on every peer");
    }
    board.node(-1)->set("ink", stroke(3));
    board.rig.step_ticks(6);
    if (!board.everyone_holds("ink", stroke(3))) {
        return law_broken("the server's ink did not arrive");
    }
    NetwEntity::of(board.node(-1))->set_controller(board.rig.peer_id(1));
    board.rig.step_ticks(4);
    PackedInt32Array breaks;
    breaks.push_back(0);
    breaks.push_back(3);
    const Transform3D pose(Basis(Vector3(0, 1, 0), 0.5), Vector3(1, 2, 3));
    board.node(1)->set("ink", stroke(7));
    board.node(1)->set("breaks", breaks);
    board.node(1)->set("pose", pose);
    board.rig.step_ticks(6);
    if (!board.everyone_holds("ink", stroke(7))
        || !board.everyone_holds("breaks", breaks)
        || !board.everyone_holds("pose", pose)) {
        return law_broken("the controller's values did not reach everyone");
    }
    board.node(1)->set("ink", stroke(9));
    board.rig.step_ticks(6);
    if (!board.everyone_holds("ink", stroke(9))) {
        return law_broken("a second stroke did not arrive");
    }
    return law_held();
}

LawVerdict packed_array_grown_in_place(const World &) {
    Board board;
    if (board.node(0) == nullptr || board.node(1) == nullptr) {
        return law_broken("the board did not spawn on every peer");
    }
    for (int at = 0; at < 3; ++at) {
        board.draw(-1, at, at + 1);
        board.rig.step_ticks(3);
    }
    board.rig.step_ticks(3);
    if (!board.everyone_holds("ink", stroke(3))) {
        return law_broken("the server's growth did not arrive");
    }
    NetwEntity::of(board.node(-1))->set_controller(board.rig.peer_id(1));
    board.rig.step_ticks(4);
    board.draw(1, 3, 5);
    board.rig.step_ticks(6);
    if (!board.everyone_holds("ink", stroke(5))) {
        return law_broken("the controller's growth did not arrive");
    }
    board.draw(1, 5, 6);
    board.rig.step_ticks(6);
    if (!board.everyone_holds("ink", stroke(6))) {
        return law_broken("a single point did not arrive");
    }
    return law_held();
}

void deny_every_request(Object *p_request) {
    NetwControlRequest *asked = Object::cast_to<NetwControlRequest>(p_request);
    if (asked != nullptr) {
        asked->deny();
    }
}

void quiet(int64_t, const String &) {
}

LawVerdict refused_claim_writes_back(const World &) {
    Board board;
    if (board.node(0) == nullptr) {
        return law_broken("the board did not spawn on the claimant");
    }
    board.node(-1)->set("ink", stroke(3));
    board.rig.step_ticks(6);
    NetwEntity::of(board.node(-1))
        ->set_control_handler(callable_mp_static(&deny_every_request));
    const int flight = 8;
    Ref<netw::NetwLinkConditions> slow = netw_test::link(31);
    slow->set_latency_ms(double(flight) * 1000.0 / TICKRATE);
    board.rig.conditions(-1, slow, board.rig.peer_id(0));

    const Ref<NetwEntity> claimant = NetwEntity::of(board.node(0));
    const Ref<NetwPromise> claim = claimant->claim_control();
    claim->catch_error(callable_mp_static(&quiet));
    board.draw(0, 3, 7);
    board.rig.step_ticks(2);
    if (!(PackedVector2Array(board.node(0)->get("ink")) == stroke(7))) {
        return law_broken("the claimant could not draw while it waited");
    }
    board.rig.step_ticks(flight + 4);
    if (!claim->get_is_failed()) {
        return law_broken("the denied claim did not fail");
    }
    if (!board.everyone_holds("ink", stroke(3))) {
        return law_broken("the claimant kept what it drew");
    }
    return law_held();
}

constexpr const char *BOAT_SOURCE = R"(extends Node2D

var hull := 0
var sail := 0

func _init() -> void:
	Netw.configure_entity(self)
	Netw.configure_property(self, &"hull").state()

func rig() -> void:
	Netw.configure_property(self, &"sail").state()
)";

Node *build_boat(const Variant &p_name) {
    Node *made = build_minted(p_name);
    if (String(p_name) == "Sloop") {
        made->call("rig");
    }
    return made;
}

LawVerdict instances_send_what_each_declared(const World &) {
    Stand stand(1, BOAT_SOURCE);
    const FaultWatch faults;
    const int raft = stand.spawn(&build_boat, "Raft");
    const int sloop = stand.spawn(&build_boat, "Sloop");
    const int routes[] = {raft, sloop};
    for (const int route : routes) {
        Node *server = stand.at(route, -1);
        if (server == nullptr || stand.at(route, 0) == nullptr) {
            return law_broken("a boat did not spawn on both peers");
        }
        server->set("hull", 3);
        server->set("sail", 7);
    }
    stand.rig.step_ticks(6);
    for (const int route : routes) {
        Node *client = stand.at(route, 0);
        const int sail = route == sloop ? 7 : 0;
        if (int(client->get("hull")) != 3 || int(client->get("sail")) != sail) {
            return law_broken(
                "'%s' reads hull %d sail %d on the client",
                String(client->get_name()).utf8().get_data(),
                int(client->get("hull")),
                int(client->get("sail"))
            );
        }
    }
    if (faults.errors() != 0 || faults.warnings_logged() != 0) {
        return law_broken("the boats printed an error or a warning");
    }
    return law_held();
}

uint64_t turn_set(NetwMultiplayer *p_session, Node *p_node, int p_bits) {
    netw::Netw::configure_property(p_node, "rotation")
        ->state()
        ->quantize(netw::gd::array_of(by_bits(p_bits, 0.0, 7.0)));
    return netw::property_set_builder::from_node(
        p_node,
        PropertySet::RECORD_STATE,
        p_session
    );
}

LawVerdict scriptless_nodes_share_one_set(const World &) {
    Ref<NetwMultiplayer> session;
    session.instantiate();
    Node2D *nodes[] = {memnew(Node2D), memnew(Node2D), memnew(Node2D)};
    const uint64_t first = turn_set(session.ptr(), nodes[0], 12);
    const uint64_t alike = turn_set(session.ptr(), nodes[1], 12);
    const uint64_t coarse = turn_set(session.ptr(), nodes[2], 6);
    for (Node2D *node : nodes) {
        memdelete(node);
    }
    if (first == netw::repl::SetBook::NONE) {
        return law_broken("the first node compiled no set");
    }
    if (alike != first) {
        return law_broken("two nodes declaring alike hold two sets");
    }
    if (coarse == first) {
        return law_broken("a coarser quantizer shares the finer one's set");
    }
    return law_held();
}

LawVerdict scriptless_node_in_no_entity_reported(const World &) {
    Node2D *node = memnew(Node2D);
    node->set_name("Drifter");
    netw::Netw::configure_property(node, "position")->state();
    {
        FaultWatch faults;
        faults.expect_error("*'Drifter'*[NW1010]*", 1);
        netw::gd::scene_root()->add_child(node);
        node->get_parent()->remove_child(node);
        netw::gd::scene_root()->add_child(node);
    }
    node->get_parent()->remove_child(node);
    memdelete(node);
    return law_held();
}

constexpr const char *SETTINGS_SOURCE = R"(extends Node2D

var hp := 0
var speed := 0.0
var friend: Node
)";

constexpr const char *ONE_SHOT_SOURCE = R"(extends AnimationTree

func _init() -> void:
	var blend := AnimationNodeBlendTree.new()
	blend.add_node(&"shot", AnimationNodeOneShot.new())
	tree_root = blend
)";

LawVerdict settings_that_cannot_apply_refused(const World &) {
    FaultWatch faults;
    faults.expect_error("*extrapolate cannot*input on 'position'*[NW0043]*", 1);
    faults.expect_error("*extrapolate cannot*input on 'rotation'*[NW0043]*", 1);
    faults.expect_error("*unchecked cannot*broadcast on 'scale'*[NW0043]*", 1);
    faults.expect_error("*'skew' cannot*input together*[NW0043]*", 1);
    faults.expect_error("*'friend', which holds an Object*[NW1020]*", 1);
    faults.expect_error("*heartbeat got -3*for 'hp'*[NW0032]*", 1);
    faults.expect_error("*correction_threshold got -0.5 for 'hp'*[NW0032]*", 1);
    faults.expect_error("*NetwQuantizeQuaternion*'hp'*[NW1019]*", 1);
    faults.expect_error("*'speed' for 'z_index'*[NW0043]*", 1);
    faults.expect_error("*linger got -1*[NW0032]*", 1);
    faults.expect_error("*interval got -1*[NW0032]*", 1);
    faults.expect_error("*'parameters/shot/request' is a request*[NW0044]*", 1);

    Node *node = netw_test::minted_node(SETTINGS_SOURCE);
    const Ref<NetwPropertyConfig> late
        = netw::Netw::configure_property(node, "position")
              ->input()
              ->extrapolate("speed");
    const Ref<NetwPropertyConfig> early
        = netw::Netw::configure_property(node, "rotation")
              ->extrapolate("speed")
              ->input();
    const Ref<NetwPropertyConfig> cast
        = netw::Netw::configure_property(node, "scale")
              ->broadcast()
              ->unchecked()
              ->retained();
    const Ref<NetwPropertyConfig> twice
        = netw::Netw::configure_property(node, "skew")->state()->input();
    const Ref<NetwPropertyConfig> object
        = netw::Netw::configure_property(node, "friend");
    const Ref<NetwPropertyConfig> hp
        = netw::Netw::configure_property(node, "hp")
              ->state()
              ->heartbeat(-3)
              ->correction_threshold(-0.5);
    Ref<NetwQuantizeQuaternion> turn;
    turn.instantiate();
    hp->quantize(netw::gd::array_of(turn));
    netw::Netw::configure_property(node, "speed");
    netw::Netw::configure_property(node, "z_index")
        ->state()
        ->extrapolate("speed");
    PropertySet set;
    netw::property_set_builder::from_property_configs(
        set,
        netw::script::model::get_declared_property_configs(node),
        PropertySet::RECORD_STATE
    );
    bool extrapolates = false;
    for (const PropertyColumn &column : set.columns) {
        extrapolates = extrapolates || column.rate_property != StringName();
    }
    const double linger = netw::Netw::configure_despawn(node)
                              ->linger(-1.0)
                              ->get_linger_seconds();
    const double interval = netw::Netw::configure_persistence(node)
                                ->interval(-1.0)
                                ->get_save_interval();
    Node *tree = netw_test::minted_node(ONE_SHOT_SOURCE);
    const Ref<NetwPropertyConfig> request
        = netw::Netw::configure_property(tree, "parameters/shot/request");
    const Ref<NetwPropertyConfig> blend
        = netw::Netw::configure_property(tree, "parameters/shot/active");

    const char *kept = nullptr;
    if (!late->get_in_input_set() || !early->get_in_input_set()
        || late->get_rate_property() != StringName()
        || early->get_rate_property() != StringName()) {
        kept = "extrapolate on an input";
    } else if (!cast->get_in_broadcast_set() || cast->get_unchecked()) {
        kept = "unchecked on a broadcast";
    } else if (cast->get_lane() != PropertySet::RETAINED) {
        kept = "a broadcast without its retained";
    } else if (
        !twice->is_refused() || twice->get_in_state_set()
        || twice->get_in_input_set()
    ) {
        kept = "two kinds";
    } else if (!object->is_refused() || !request->is_refused()) {
        kept = "an Object or a request";
    } else if (blend->is_refused()) {
        kept = "no AnimationTree parameter at all";
    } else if (
        hp->get_set_heartbeat_ticks() != NetwPropertyConfig::UNSET
        || hp->get_correction_threshold() >= 0.0
        || !hp->get_quantizers().is_empty()
    ) {
        kept = "a negative value or a quantizer of the wrong type";
    } else if (extrapolates) {
        kept = "extrapolate naming no state property";
    } else if (linger != 0.0 || interval != 0.0) {
        kept = "a negative linger or interval";
    }
    memdelete(tree);
    memdelete(node);
    return kept == nullptr ? law_held() : law_broken("kept %s", kept);
}

const Row WORLD_ROWS[] = {
    {"a property the node does not list, declared with its type",
     "its spawn value and its changes reach the client, and a controller's "
     "broadcast reaches the server",
     &unlisted_property_replicates},
    {"configure_property naming no property, a method, a signal, an "
     "unlisted property with no type, a type the node contradicts, or a "
     "property declared twice",
     "each is refused with one error, and a plain var is taken",
     &unreadable_declarations_refused},
    {"configure_property on a node already in the tree",
     "it is refused with one error",
     &mounted_node_refused},
    {"a property inside a resource, one state and one on_spawn",
     "the client's own resource takes the state, and a late joiner gets both",
     &resource_property_replicates},
    {"two entities holding one shared resource",
     "one error a property names both entities",
     &shared_resource_reported},
    {"a resource taken away and then put back",
     "one error, the client keeps its value, and the new resource is read",
     &resource_taken_away},
    {"packed arrays and a Transform3D broadcast by the server, then by a "
     "client given control",
     "every peer holds each value its controller wrote",
     &packed_arrays_broadcast},
    {"a packed array grown in place by its controller",
     "every growth reaches every peer",
     &packed_array_grown_in_place},
    {"a client drawing into a board while its claim is denied",
     "the claim fails and the client's board goes back to what it received",
     &refused_claim_writes_back},
    {"two nodes of one script, one declaring a property the other does not",
     "each node sends the properties it declared and no error is printed",
     &instances_send_what_each_declared},
    {"three nodes with no script, two declaring alike and one coarser",
     "the two alike share one compiled set and the coarser has its own",
     &scriptless_nodes_share_one_set},
    {"a property declared on a node with no script, in no entity",
     "one error names the node, however often it enters the tree",
     &scriptless_node_in_no_entity_reported},
    {"a setting on a kind that never reads it in either order, two kinds, an "
     "Object, an AnimationTree request, a negative amount, a quantizer of "
     "the wrong type, and extrapolate naming no state property",
     "each is refused with one error and nothing of it is kept",
     &settings_that_cannot_apply_refused},
};

TEST_CASE(
    "[Netw][Sync][SceneTree] a declared property reaches every peer "
    "whole, and a declaration a node cannot honour is refused"
) {
    const World world = {String("a server and clients on a loopback")};
    for (const Row &row : WORLD_ROWS) {
        NETW_CELL(row, world);
        NETW_LAW_HOLDS(row, world);
    }
}

} // namespace TestProperty

#endif
