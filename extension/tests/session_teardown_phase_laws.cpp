#include "support/internals.h"
#include "support/loopback_rig.h"
#include "support/netw_test.h"

#include "godot/node.hpp"
#include "godot/object.hpp"
#include "godot/spatial_node.hpp"
#include "godot/templates.hpp"
#include "godot/variant.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/predict_slot_engine.hpp"
#include "netw/api/session.hpp"
#include "netw/api/simulation.hpp"
#include "netw/display/book.hpp"
#include "netw/predict/engine.hpp"
#include "netw/session_core.hpp"
#include "support/netw_recorder.h"

namespace TestNetwSessionTeardownPhaseLaws {

using namespace godot;
using netw::NetwMultiplayer;
using netw::SessionCore;
using netw_test::Internals;
using netw_test::Recorder;

Ref<NetwMultiplayer> online() {
    Ref<NetwMultiplayer> core;
    core.instantiate();
    Internals::session_plane(core).transition(SessionCore::STATE_CONNECTING);
    Internals::session_plane(core).transition(SessionCore::STATE_ONLINE);
    return core;
}

TEST_CASE(
    "[Netw][Session][Hosted] TD9 observing an entity does not pin its "
    "wrapper, so a node that is freed takes its record with it rather than "
    "leaving one behind for the life of the session"
) {
    Ref<NetwMultiplayer> core;
    core.instantiate();

    Node *body = memnew(Node);
    body->set_name("Observed");
    uint64_t wrapper_id = 0;
    {
        const Ref<netw::NetwEntity> entity = netw::NetwEntity::ensure(body);
        REQUIRE(entity.is_valid());
        wrapper_id = uint64_t(entity->get_instance_id());
        core->observe_node_entity(body);
    }
    REQUIRE(netw::gd::object_of(godot::ObjectID(wrapper_id)) != nullptr);

    memdelete(body);

    CHECK(netw::gd::object_of(godot::ObjectID(wrapper_id)) == nullptr);
}

TEST_CASE(
    "[Netw][Session][Hosted] TP4 exiting rides the ending edge ahead of "
    "exited, so it fires once per ending and never for a bring-up that was "
    "not live"
) {
    Ref<NetwMultiplayer> failed;
    failed.instantiate();
    const Recorder unlived(failed->get_session().ptr(), {"exiting"});
    Internals::session_plane(failed).transition(SessionCore::STATE_CONNECTING);
    Internals::session_plane(failed).transition(SessionCore::STATE_OFFLINE);

    NETW_CHECK_EQ(unlived.count(StringName("exiting")), 0);

    Ref<NetwMultiplayer> core = online();
    const Recorder heard(core->get_session().ptr(), {"exiting", "exited"});
    Internals::session_plane(core).transition(SessionCore::STATE_ONLINE);

    NETW_CHECK_EQ(heard.count(StringName("exiting")), 0);

    Internals::session_plane(core).transition(SessionCore::STATE_DISCONNECTING);
    Internals::session_plane(core).transition(SessionCore::STATE_DISCONNECTING);

    NETW_CHECK_EQ(heard.count(StringName("exiting")), 1);
    NETW_CHECK_EQ(heard.count(StringName("exited")), 0);
}

TEST_CASE(
    "[Netw][Session][Hosted] TP6 session shutdown drops interpolation "
    "before releasing its entity wrappers and accepts fresh runtimes"
) {
    Ref<NetwMultiplayer> core = online();
    Node2D *body = memnew(Node2D);
    Ref<netw::NetwEntity> entity;
    entity.instantiate();
    entity->set_owner(body);
    REQUIRE(core->liveness_adopt_route(17, entity, body) == OK);
    REQUIRE(Internals::display_runtime_for(core, 17, entity) != nullptr);
    const RID handle = entity->get_rid_handle();
    auto *pool = Internals::get_prediction_engine(core);
    const int64_t slot = Internals::seat(pool, entity);
    REQUIRE(pool->bind_owner(slot, body));
    auto *prediction = memnew(netw::NetwPredictSlotEngine);
    Internals::attach_slot_engine(*prediction, core, entity);
    core->predict_engine_install(handle, prediction);
    const ObjectID wrapper = netw::gd::instance_id(entity.ptr());
    entity.unref();
    memdelete(body);

    Internals::session_plane(core).transition(SessionCore::STATE_DISCONNECTING);
    core->NETW_API_VIRTUAL(poll)();

    CHECK(Internals::get_display_book(core)->runtimes().is_empty());
    CHECK_FALSE(core->predict_is_declared(handle));
    NETW_CHECK_EQ(pool->slot_registered(), 0);
    const bool wrapper_freed = netw::gd::object_of(wrapper) == nullptr;
    CHECK(wrapper_freed);
    if (core->predict_is_declared(handle)) {
        core->unregister_prediction(handle);
    }
    Internals::get_display_book(core)->clear();

    Internals::session_plane(core).transition(SessionCore::STATE_OFFLINE);
    Internals::session_plane(core).transition(SessionCore::STATE_CONNECTING);
    Internals::session_plane(core).transition(SessionCore::STATE_ONLINE);
    body = memnew(Node2D);
    entity.instantiate();
    entity->set_owner(body);
    REQUIRE(core->liveness_adopt_route(17, entity, body) == OK);
    auto *runtime = Internals::display_runtime_for(core, 17, entity);
    REQUIRE(runtime != nullptr);
    CHECK(runtime->entity_rid() == entity->get_rid_handle());
    core->liveness_clear_session();
    CHECK(Internals::get_display_book(core)->runtimes().is_empty());
    core->display_clear_runtimes();
    memdelete(body);
}

TEST_CASE(
    "[Netw][Session][Hosted] TP7 prediction release detaches its owner "
    "without discovering a lost node while clearing state parts"
) {
    Ref<NetwMultiplayer> core;
    core.instantiate();
    Node2D *body = memnew(Node2D);
    Ref<netw::NetwEntity> entity;
    entity.instantiate();
    entity->set_owner(body);
    auto *pool = Internals::get_prediction_engine(core);
    const int64_t slot = Internals::seat(pool, entity);
    REQUIRE(pool->bind_owner(slot, body));
    netw::NetwPredictSlotEngine engine;
    Internals::attach_slot_engine(engine, core, entity);
    REQUIRE(engine.seated_pool() == pool);
    REQUIRE(engine.native_slot() == slot);
    entity->get_simulation()->set_schedule(
        netw::NetwSimulation::SCHEDULE_FRAME
    );

    SUBCASE("the owner remains alive") {
    }
    SUBCASE("the owner has already been freed") {
        memdelete(body);
        body = nullptr;
    }

    engine.release();

    CHECK_FALSE(pool->owner_bound(slot));
    NETW_CHECK_EQ(
        pool->drive_stat(slot, netw::NetwPredictionEngine::STAT_OWNER_LOST),
        0
    );
    engine.release();
    Internals::unseat(pool, entity);
    CHECK(engine.runs_in_pass(netw::NetwSimulation::SCHEDULE_FRAME));
    CHECK_FALSE(engine.runs_in_pass(netw::NetwSimulation::SCHEDULE_TICK));
    if (body != nullptr) {
        memdelete(body);
    }
}

} // namespace TestNetwSessionTeardownPhaseLaws
