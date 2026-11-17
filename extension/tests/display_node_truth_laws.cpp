#include "support/internals.h"
#include "support/netw_test.h"

#if defined(NETW_TIER_HOSTED)

#include "support/netw_call_log.h"
#include "support/value_flow_stand.h"

#include "netw/api/context.hpp"
#include "netw/api/display.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/interpolate.hpp"
#include "netw/api/prediction.hpp"
#include "netw/api/tests.hpp"
#include "netw/display/channel.hpp"
#include "netw/display/history.hpp"
#include "netw/display/runtime.hpp"
#include "netw/display/timing.hpp"
#include "netw/script/model.hpp"

#include <godot_cpp/classes/node2d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/rendering_server.hpp>

namespace TestDisplayNodeTruthLaws {

using namespace godot;
using netw::NetwDisplay;
using netw::NetwEntity;
using netw::NetwMultiplayer;
using netw::NetwNativeTests;
using netw_test::LoopbackRig;
using netw_test::Internals;

constexpr int TICKRATE = 30;
constexpr int REMOTE_PEER = 2;

bool near(const Vector2 &p_held, const Vector2 &p_wanted) {
    return p_held.distance_to(p_wanted) <= 0.1;
}

Vector2 p0() {
    return Vector2(0.0, 0.0);
}

Vector2 p1() {
    return Vector2(100.0, 0.0);
}

Vector2 p2() {
    return Vector2(200.0, 0.0);
}

Ref<netw::NetwInterpolate> lerp_to(const StringName &p_target) {
    Ref<netw::NetwInterpolate> spec;
    spec.instantiate();
    return spec->lerp()->smooth(0.0)->to(p_target);
}

Ref<netw::NetwInterpolate> slerp_to(const StringName &p_target) {
    Ref<netw::NetwInterpolate> spec;
    spec.instantiate();
    return spec->slerp()->smooth(0.0)->to(p_target);
}

class Bench {
public:
    LoopbackRig rig;

    Bench() : rig(0) {
        rig.mount();
        netw_test::flow_clocks(rig, TICKRATE, 0);
    }

    NetwMultiplayer *core() const {
        return netw_test::flow_core(rig.server());
    }

    netw::ClockEngine &clock() const {
        return rig.clock_of(rig.server());
    }

    void seat(Node *p_node) const {
        rig.branch(-1)->add_child(p_node);
    }

    void bind(const Ref<NetwEntity> &p_entity, int64_t p_route) const {
        core()->liveness_bind_route(p_route, p_entity.ptr());
    }

    void record(
        Node *p_node,
        const StringName &p_property,
        const Variant &p_value,
        int64_t p_tick
    ) const {
        NetwNativeTests::display_record(
            core(),
            p_node,
            p_property,
            p_value,
            p_tick,
            netw::script::model::get_node_property_interpolator(
                p_node,
                p_property
            ),
            false
        );
    }

    void display_at(int64_t p_tick, int p_offset, double p_factor) const {
        netw::ClockEngine &handle = clock();
        handle.set_tick(int(p_tick));
        handle.set_display_offset(p_offset);
        handle.set_tick_factor_override(p_factor);
        NetwNativeTests::display_pump(core(), 0.0);
    }

    void render(const RID &p_entity, double p_delta = 1.0 / 60.0) const {
        NetwNativeTests::display_pump_runtime(
            core(),
            NetwNativeTests::display_runtime_of(core(), p_entity),
            netw::display::capture_timing(
                &Internals::clock_engine(core()),
                p_delta
            )
        );
    }

    void render_many(const RID &p_entity, int p_frames) const {
        for (int frame = 0; frame < p_frames; ++frame) {
            render(p_entity);
        }
    }
};

struct Subject {
    Node2D *body = nullptr;
    Node2D *visual = nullptr;
    Ref<NetwEntity> entity;

    RID rid() const {
        return entity->get_rid_handle();
    }
};

Subject stand_interpolated(
    const Bench &p_bench,
    const char *p_name,
    NetwMultiplayer::DisplayRole p_role,
    double p_smooth_time = -1.0
) {
    Subject subject;
    subject.body = memnew(Node2D);
    subject.body->set_name(p_name);
    subject.body->set_multiplayer_authority(REMOTE_PEER);
    subject.entity = NetwEntity::ensure(subject.body);
    const Ref<NetwDisplay> display = subject.entity->get_display();
    display->set_enable_smart_dilation(false);
    subject.visual = memnew(Node2D);
    subject.visual->set_name("Visual");
    subject.body->add_child(subject.visual);
    display->set_visual_root(NodePath("Visual"));
    display->set_display_role(p_role);
    display->set_live_mode(NetwMultiplayer::LIVE_MODE_CHASE);
    if (p_smooth_time >= 0.0) {
        display->set_live_smooth_time(p_smooth_time);
    }
    netw::Netw::configure_property(subject.body, StringName("position"))
        ->interpolate(netw::gd::array_of(lerp_to(StringName("position"))));
    p_bench.seat(subject.body);
    p_bench.bind(subject.entity, 7);
    p_bench.core()->session_flush_deferred();
    return subject;
}

Subject stand_predicted(const Bench &p_bench) {
    return stand_interpolated(
        p_bench,
        "PredictedTarget",
        NetwMultiplayer::DISPLAY_ROLE_PREDICTED,
        0.05
    );
}

TEST_CASE(
    "[Netw][Display][SceneTree] a slerp channel brackets a "
    "quaternion, which no component-wise lerp would reach"
) {
    Bench bench;
    Node3D *node = memnew(Node3D);
    node->set_name("RotationTarget");
    node->set_multiplayer_authority(REMOTE_PEER);
    const Ref<NetwEntity> entity = NetwEntity::ensure(node);
    entity->get_display()->set_enable_smart_dilation(false);
    netw::Netw::configure_property(node, StringName("quaternion"))
        ->interpolate(netw::gd::array_of(slerp_to(StringName("quaternion"))));
    bench.seat(node);
    bench.bind(entity, 41);

    const Quaternion held;
    const Quaternion turned(Vector3(0.0, 1.0, 0.0), Math_PI / 2.0);
    bench.record(node, StringName("quaternion"), held, 0);
    bench.record(node, StringName("quaternion"), turned, 1);
    bench.display_at(1, 1, 0.5);

    CHECK(node->get_quaternion().is_equal_approx(held.slerp(turned, 0.5)));
}

TEST_CASE(
    "[Netw][Display][SceneTree] a predicted chase moves the visual "
    "toward the live source without moving the source"
) {
    Bench bench;
    const Subject subject = stand_predicted(bench);
    subject.body->set_position(p1());
    bench.render(subject.rid());

    NETW_CHECK_GT(subject.visual->get_global_position().x, 0.0);
    NETW_CHECK_LT(subject.visual->get_global_position().x, p1().x);
    CHECK(near(subject.body->get_position(), p1()));
}

TEST_CASE(
    "[Netw][Display][SceneTree] a reset snaps the chase to the live "
    "source"
) {
    Bench bench;
    const Subject subject = stand_predicted(bench);
    subject.body->set_position(p1());
    bench.render(subject.rid());
    NETW_CHECK_LT(subject.visual->get_global_position().x, p1().x);

    subject.entity->get_display()->reset();
    CHECK(near(subject.visual->get_global_position(), p1()));
}

TEST_CASE(
    "[Netw][Display][SceneTree] a demote to remote resumes from the "
    "rows recorded while predicted and seeds its role offset on the "
    "first frame"
) {
    Bench bench;
    const Subject subject = stand_predicted(bench);
    subject.entity->get_prediction()->set_teleport_threshold(250.0);
    subject.body->set_position(p0());
    bench.render_many(subject.rid(), 30);

    bench.record(subject.body, StringName("position"), p1(), 1);
    bench.record(subject.body, StringName("position"), p2(), 2);
    const netw::display::Runtime *runtime
        = NetwNativeTests::display_runtime_of(bench.core(), subject.rid());
    REQUIRE(runtime != nullptr);
    REQUIRE(runtime->channels().size() > 0);
    const netw::display::Channel *track = runtime->channels()[0];
    CHECK(!track->display_history().is_empty());
    bench.render(subject.rid());
    NETW_CHECK_LT(subject.visual->get_global_position().distance_to(p0()), 0.1);

    subject.entity->get_display()->set_display_role(
        NetwMultiplayer::DISPLAY_ROLE_REMOTE
    );
    CHECK(
        bool(runtime->track_stat(track->get_name(), StringName("offset_armed")))
    );
    bench.display_at(2, 0, 0.0);
    CHECK(!bool(
        runtime->track_stat(track->get_name(), StringName("offset_armed"))
    ));
    CHECK(
        bool(runtime->track_stat(track->get_name(), StringName("offset_held")))
    );
    NETW_CHECK_LT(subject.visual->get_global_position().distance_to(p0()), 0.1);

    bench.render_many(subject.rid(), 90);
    CHECK(near(subject.visual->get_global_position(), p2()));
}

TEST_CASE(
    "[Netw][Display][SceneTree] a promote to predicted adopts the "
    "display the remote role left, then glides to the live source"
) {
    Bench bench;
    const Subject subject = stand_interpolated(
        bench,
        "GlideTarget",
        NetwMultiplayer::DISPLAY_ROLE_REMOTE
    );
    subject.entity->get_prediction()->set_teleport_threshold(250.0);
    bench.core()->display_set_param(
        subject.rid(),
        NetwMultiplayer::DISPLAY_PARAM_CHASE_GLIDE_TIME,
        0.15
    );

    bench.record(subject.body, StringName("position"), p1(), 1);
    bench.display_at(1, 0, 0.0);
    bench.render_many(subject.rid(), 90);
    CHECK(near(subject.visual->get_global_position(), p1()));

    subject.body->set_position(p0());
    subject.entity->get_display()->set_display_role(
        NetwMultiplayer::DISPLAY_ROLE_PREDICTED
    );
    bench.render(subject.rid());
    NETW_CHECK_LT(
        subject.visual->get_global_position().distance_to(p1()),
        20.0
    );
    bench.render_many(subject.rid(), 90);
    CHECK(near(subject.visual->get_global_position(), p0()));
}

TEST_CASE(
    "[Netw][Display][SceneTree] a callback or a rendering item set on "
    "an entity takes its drawn values, and its visual node is left alone"
) {
    for (const bool called : {true, false}) {
        CAPTURE(called);
        Bench bench;
        const Subject subject = stand_interpolated(
            bench,
            "SinkTarget",
            NetwMultiplayer::DISPLAY_ROLE_REMOTE
        );
        netw_test::CallLog drawn;
        const RID item = RenderingServer::get_singleton()->canvas_item_create();
        if (called) {
            bench.core()->display_set_callback(
                subject.rid(),
                drawn.callable(StringName("drawn"))
            );
        } else {
            bench.core()->display_set_target_item(subject.rid(), item);
        }

        bench.record(subject.body, StringName("position"), p1(), 1);
        bench.display_at(1, 0, 0.0);
        bench.render_many(subject.rid(), 90);

        CHECK(near(subject.visual->get_global_position(), p0()));
        const Array last = drawn.args(
            StringName("drawn"),
            drawn.count(StringName("drawn")) - 1
        );
        CHECK(last.size() == (called ? 3 : 0));
        if (last.size() == 3) {
            CHECK(RID(last[0]) == subject.rid());
            CHECK(StringName(last[1]) == StringName("position"));
            CHECK(near(Vector2(last[2]), p1()));
        }
        RenderingServer::get_singleton()->free_rid(item);
    }
}

} // namespace TestDisplayNodeTruthLaws

#endif
