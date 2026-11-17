#include "support/netw_cells.h"
#include "support/netw_test.h"

#include <cmath>

#include "godot/scene_tree.hpp"
#include "godot/spatial_node.hpp"
#include "netw/api/interpolate.hpp"
#include "netw/display/history.hpp"
#include "netw/display/playhead.hpp"
#include "netw/display/port.hpp"
#include "netw/display/pump.hpp"
#include "netw/display/runtime.hpp"

namespace TestNetwDisplay {

using namespace godot;
using netw::NetwInterpolate;
using netw::display::Channel;
using netw::display::Decl;
using netw::display::dilate_playhead;
using netw::display::Playhead;
using netw::display::Port;
using netw::display::PumpStats;
using netw::display::Runtime;
using netw::display::Timing;
using netw_test::law_broken;
using netw_test::law_held;
using netw_test::LawVerdict;

constexpr double HALF_TURN = 3.1415926535897932384626433833;
constexpr double TAU = 2.0 * HALF_TURN;

struct Stand {
    String label;
};

typedef netw_test::LawRowFor<Stand> Row;

Port make_port(
    Object *p_target,
    Object *p_host,
    const StringName &p_target_prop,
    const StringName &p_source_prop
) {
    Port port;
    port.bind(p_target, p_host);
    port.declare(p_target_prop, p_source_prop, true);
    return port;
}

struct Flat {
    Node2D *holder = memnew(Node2D);
    Node2D *body = memnew(Node2D);
    Node2D *visual = memnew(Node2D);

    Flat() {
        holder->add_child(body);
        body->add_child(visual);
    }

    ~Flat() {
        memdelete(holder);
    }
};

struct Solid {
    Node3D *holder = memnew(Node3D);
    Node3D *body = memnew(Node3D);
    Node3D *visual = memnew(Node3D);
    bool mounted = false;

    explicit Solid(bool p_mounted) : mounted(p_mounted) {
        holder->add_child(body);
        body->add_child(visual);
        if (mounted) {
            REQUIRE(netw::gd::scene_root() != nullptr);
            netw::gd::scene_root()->add_child(holder);
        }
    }

    ~Solid() {
        if (mounted) {
            netw::gd::scene_root()->remove_child(holder);
        }
        memdelete(holder);
    }
};

LawVerdict flat_parent(const Stand &) {
    Flat flat;
    flat.holder->set_position(Vector2(100.0, 0.0));
    flat.holder->set_rotation(HALF_TURN * 0.5);
    make_port(flat.visual, flat.body, "position", "position")
        .write(Vector2(5.0, 0.0));
    make_port(flat.visual, flat.body, "rotation", "rotation")
        .write(HALF_TURN * 0.25);

    const Vector2 expected = flat.holder->get_global_transform().xform(
        Vector2(5.0, 0.0)
    );
    if (!flat.visual->get_global_position().is_equal_approx(expected)) {
        return law_broken("the visual left its parent's frame");
    }
    if (!Math::is_equal_approx(
            double(flat.visual->get_global_rotation()),
            HALF_TURN * 0.75
        )) {
        return law_broken("the rotation ignored the parent's turn");
    }
    return law_held();
}

LawVerdict global_source(const Stand &) {
    Flat flat;
    flat.holder->set_position(Vector2(100.0, 0.0));
    make_port(flat.visual, flat.body, "position", "global_position")
        .write(Vector2(5.0, 0.0));
    const Vector2 drawn = flat.visual->get_global_position();
    if (!drawn.is_equal_approx(Vector2(5.0, 0.0))) {
        return law_broken("a global value was moved by the parent");
    }
    return law_held();
}

LawVerdict solid_parent(const Stand &) {
    Solid solid(true);
    solid.holder->set_position(Vector3(0.0, 10.0, 0.0));
    solid.holder->set_rotation(Vector3(0.0, HALF_TURN * 0.5, 0.0));
    make_port(solid.visual, solid.body, "position", "position")
        .write(Vector3(0.0, 2.0, 0.0));
    if (!solid.visual->get_global_position().is_equal_approx(
            Vector3(0.0, 12.0, 0.0)
        )) {
        return law_broken("the 3D visual left its parent's frame");
    }

    make_port(solid.visual, solid.body, "rotation", "rotation")
        .write(Vector3(0.0, HALF_TURN * 0.25, 0.0));
    if (!solid.visual->get_global_rotation().is_equal_approx(
            Vector3(0.0, HALF_TURN * 0.75, 0.0)
        )) {
        return law_broken("the 3D rotation ignored the parent's turn");
    }

    const Quaternion shown(Vector3(1.0, 0.0, 0.0), HALF_TURN * 0.25);
    make_port(solid.visual, solid.body, "quaternion", "quaternion")
        .write(shown);
    const Quaternion expected
        = solid.holder->get_global_transform().basis.get_rotation_quaternion()
        * shown;
    if (!solid.visual->get_global_transform()
             .basis.get_rotation_quaternion()
             .is_equal_approx(expected)) {
        return law_broken("the quaternion ignored the parent's turn");
    }
    return law_held();
}

LawVerdict solid_unmounted(const Stand &) {
    Solid solid(false);
    solid.holder->set_position(Vector3(0.0, 10.0, 0.0));
    solid.body->set_position(Vector3(3.0, 6.0, 3.0));
    make_port(solid.visual, solid.body, "position", "position")
        .write(Vector3(3.0, 8.0, 3.0));
    if (!solid.visual->get_position().is_equal_approx(Vector3(0.0, 2.0, 0.0))) {
        return law_broken("the unmounted visual took a frame it lacks");
    }
    return law_held();
}

LawVerdict authored_offset(const Stand &) {
    Solid solid(true);
    const Transform3D authored(
        Basis(Vector3(0.0, 1.0, 0.0), HALF_TURN * 0.25),
        Vector3(0.0, -0.5, -1.0)
    );
    solid.visual->set_transform(authored);
    solid.holder->set_position(Vector3(0.0, 10.0, 0.0));
    solid.body->set_position(Vector3(3.0, 1.0, 2.0));
    solid.body->set_rotation(Vector3(0.0, HALF_TURN * 0.5, 0.0));

    Port turn = make_port(solid.visual, solid.body, "rotation", "rotation");
    Port move = make_port(solid.visual, solid.body, "position", "position");
    turn.seat_authored(authored);
    move.seat_authored(authored);
    turn.write(solid.body->get_rotation());
    move.write(solid.body->get_position());

    const Transform3D expected = solid.body->get_global_transform() * authored;
    if (!solid.visual->get_global_position().is_equal_approx(expected.origin)
        || !solid.visual->get_global_transform().basis.is_equal_approx(
            expected.basis
        )) {
        return law_broken("the visual lost its authored offset");
    }
    return law_held();
}

LawVerdict no_global_setter(const Stand &) {
    Flat flat;
    flat.holder->set_scale(Vector2(3.0, 3.0));
    make_port(flat.visual, flat.body, "scale", "scale")
        .write(Vector2(2.0, 2.0));
    if (!flat.visual->get_scale().is_equal_approx(Vector2(2.0, 2.0))) {
        return law_broken("the scale was not written");
    }
    return law_held();
}

LawVerdict freed_visual(const Stand &) {
    Node2D *visual = memnew(Node2D);
    Port port = make_port(visual, nullptr, "position", "position");
    memdelete(visual);
    if (port.write(Vector2(1.0, 1.0)) != Port::WRITE_UNBOUND) {
        return law_broken("a freed visual was written");
    }
    return law_held();
}

LawVerdict angle_between(const Stand &) {
    Runtime holder;
    Channel *channel = holder.add_channel();
    netw::display::History &history = channel->display_history();
    history.set_mode(NetwInterpolate::MODE_ANGLE);
    history.record(10, 0.1, false);
    history.record(11, TAU - 0.1, false);
    const double halfway = double(history.sample(
        10,
        0.5,
        Variant(),
        1,
        false,
        0,
        1.0 / 60.0,
        Variant(),
        false
    ));
    if (std::abs(std::remainder(halfway, TAU)) > 1e-6) {
        return law_broken("halfway read %f, the long way round", halfway);
    }
    return law_held();
}

LawVerdict frame_rate(const Stand &) {
    Ref<NetwInterpolate> spec;
    spec.instantiate();
    spec->smooth(0.1);
    const double slow = 1.0 - spec->smoothing_weight(1.0 / 60.0);
    const double fast = 1.0 - spec->smoothing_weight(1.0 / 120.0);
    if (!Math::is_equal_approx(slow, fast * fast)) {
        return law_broken(
            "one 60 Hz frame left %f, two 120 Hz frames %f",
            slow,
            fast * fast
        );
    }
    spec->smooth(0.0);
    if (spec->smoothing_weight(1.0 / 60.0) != 1.0) {
        return law_broken("no smoothing still eased toward the target");
    }
    return law_held();
}

LawVerdict blendable(const Stand &) {
    Ref<NetwInterpolate> spec;
    spec.instantiate();
    const Variant::Type smoothed[] = {
        Variant::FLOAT,
        Variant::VECTOR2,
        Variant::VECTOR3,
        Variant::QUATERNION,
        Variant::COLOR,
    };
    for (const Variant::Type type : smoothed) {
        if (!spec->supports_type(type)) {
            return law_broken("type %d was not smoothed", int(type));
        }
    }
    const Variant::Type refused[] = {
        Variant::INT,
        Variant::STRING,
        Variant::TRANSFORM3D,
    };
    for (const Variant::Type type : refused) {
        if (spec->supports_type(type)) {
            return law_broken("type %d was smoothed", int(type));
        }
    }
    return law_held();
}

Playhead playhead() {
    Playhead out;
    out.set_expected_interval_ticks(3);
    return out;
}

LawVerdict factor_above_one(const Stand &) {
    Playhead head = playhead();
    const double first = head.place(10, 1.4, true);
    if (!Math::is_equal_approx(first, 0.4) || head.get_display_tick() != 10) {
        return law_broken(
            "a factor of 1.4 read %f at tick %lld",
            first,
            (long long)head.get_display_tick()
        );
    }
    const double second = head.place(10, 2.25, true);
    if (!Math::is_equal_approx(second, 0.25)
        || head.get_display_tick() != 11) {
        return law_broken(
            "a factor of 2.25 read %f at tick %lld",
            second,
            (long long)head.get_display_tick()
        );
    }
    return law_held();
}

LawVerdict past_newest(const Stand &) {
    Runtime holder;
    Channel *channel = holder.add_channel();
    channel->set_name("position");
    channel->display_history().record(9, Vector2(0.0, 0.0), false);
    channel->display_history().record(10, Vector2(1.0, 0.0), false);

    Timing timing;
    timing.display_tick = 9;
    timing.frame_ticks = 1.0;

    PumpStats short_of_newest;
    timing.tick_factor = 0.8;
    dilate_playhead(&holder, timing, short_of_newest, false);
    if (short_of_newest.starving != 0) {
        return law_broken("a playhead short of the newest row starved");
    }
    PumpStats past;
    timing.tick_factor = 1.2;
    dilate_playhead(&holder, timing, past, false);
    if (past.starving != 1) {
        return law_broken("a playhead past the newest row did not starve");
    }
    return law_held();
}

LawVerdict jittered_arrivals(const Stand &) {
    const Decl settings;
    Playhead head = playhead();
    int64_t tick = 100;
    for (int at = 0; at < 400; ++at) {
        ++tick;
        const double age = at % 2 == 0 ? 3.0 : 5.0;
        head.hear_newest(settings, tick, double(tick) + age);
    }
    const double spread = 4.0 + settings.arrival_spread_weight * 1.0 + 1.0;
    if (std::abs(head.min_lag(settings, 2, 2) - spread) > 0.2) {
        return law_broken(
            "the floor read %f, not %f",
            head.min_lag(settings, 2, 2),
            spread
        );
    }

    Playhead far = playhead();
    int64_t far_tick = 100;
    for (int at = 0; at < 400; ++at) {
        ++far_tick;
        far.hear_newest(settings, far_tick, double(far_tick) + 40.0);
    }
    if (far.min_lag(settings, 2, 2) != settings.max_arrival_lag) {
        return law_broken("a late stream's floor passed the cap");
    }

    constexpr int SEND_INTERVAL = 3;
    Playhead paced = playhead();
    paced.set_send_interval_ticks(SEND_INTERVAL);
    int64_t paced_tick = 100;
    for (int at = 0; at < 400; ++at) {
        paced_tick += SEND_INTERVAL;
        paced.hear_newest(settings, paced_tick, double(paced_tick) + 1.0);
    }
    const double waited = 1.0 + double(SEND_INTERVAL);
    if (std::abs(paced.min_lag(settings, 2, 2) - waited) > 0.01) {
        return law_broken(
            "the floor of a stream sent on every third tick read %f, not %f",
            paced.min_lag(settings, 2, 2),
            waited
        );
    }
    return law_held();
}

LawVerdict starved_frames(const Stand &) {
    Playhead head = playhead();
    Decl settings;
    settings.set_param(netw::display::PARAM_LAG_ADAPT_RATE, 0.0);
    settings.set_param(netw::display::PARAM_FLOOR_SMOOTHING, 0.0);

    head.dilate(settings, 0, 0, true);
    head.dilate(settings, 0, 0, true);
    if (head.get_display_lag() != 0.0) {
        return law_broken(
            "starved frames grew the lag to %f",
            head.get_display_lag()
        );
    }
    head.dilate(settings, 0, 0, false);
    if (head.get_starvation_ticks() != 0) {
        return law_broken("a fed frame left the stream starving");
    }
    return law_held();
}

const Row ROWS[] = {
    {"a visual under a moved and turned 2D parent",
     "it is drawn at the body's pose in the parent's frame",
     &flat_parent},
    {"a value that is already global",
     "it is drawn where it says whatever the parent is",
     &global_source},
    {"a mounted 3D visual under a moved and turned parent",
     "its position, rotation and quaternion follow the parent's frame",
     &solid_parent},
    {"a 3D visual outside the tree",
     "it is drawn at its offset from the body",
     &solid_unmounted},
    {"a visual with an offset set in the editor",
     "it is drawn at the body's pose times that offset",
     &authored_offset},
    {"a property with no global setter, such as scale",
     "it is written as a local value",
     &no_global_setter},
    {"a visual freed while the body still moves",
     "nothing is written to it",
     &freed_visual},
    {"an angle either side of zero",
     "it is drawn the short way round",
     &angle_between},
    {"one frame at 60 Hz or two at 120 Hz",
     "smoothing covers the same ground, and no smoothing snaps",
     &frame_rate},
    {"a property of a type that cannot be blended",
     "it is not smoothed, while floats, vectors, quaternions and colors are",
     &blendable},
    {"a tick factor above one",
     "the playhead moves its whole ticks into the tick and keeps the "
     "fraction",
     &factor_above_one},
    {"a playhead past the newest row by its tick factor alone",
     "it counts as starving",
     &past_newest},
    {"snapshots arriving with jitter, or very late",
     "the lag settles at their age plus the spread and one tick, under the "
     "cap",
     &jittered_arrivals},
    {"frames with no new snapshot",
     "the lag never grows, and the first fed frame ends the starvation",
     &starved_frames},
};

TEST_CASE(
    "[Netw][Display] a remote visual is drawn where and when the server "
    "had it"
) {
    const Stand stand = {String("a visual drawn between snapshots")};
    for (const Row &row : ROWS) {
        NETW_CELL(row, stand);
        NETW_LAW_HOLDS(row, stand);
    }
}

} // namespace TestNetwDisplay
