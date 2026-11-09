#include "support/netw_test.h"

#include "support/fault_log.h"
#include "support/loopback_rig.h"

#if defined(NETW_TIER_HOSTED)

#include "godot/variant.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/sync_pipeline.hpp"
#include "netw/predict/engine.hpp"

namespace TestNetwPredictTreeExitLaws {

using namespace godot;
using namespace netw_test;

constexpr int SERVER = -1;
constexpr int CLIENT = 0;

struct BookSizes {
    int64_t slots_open = 0;
    int64_t store_slots = 0;
    int64_t bindings = 0;
    int64_t derived_sets = 0;
};

struct ExitRig {
    LoopbackRig rig;

    ExitRig() : rig(1) {
        rig.mount();
        rig.declare_world(
            WorldDecl().clocked(30, 3).lag_compensated().player(
                EntityDecl()
                    .named("P")
                    .on_schema("PredictedPose")
                    .synced("position")
                    .placed_at(Vector2())
                    .predicted(0.01)
                    .scheduled(netw::Schedule::TICK)
                    .mounted()
                    .spawned(),
                CLIENT
            )
        );
        drive(10, Vector2(1.0, 0.0));
    }

    netw::NetwMultiplayer *api(int p_side) const {
        return p_side == SERVER ? rig.server() : rig.client(p_side);
    }

    Node *owner(int p_side) const {
        return rig.node_of(rig.entity_of("P", p_side), p_side);
    }

    int64_t slot(int p_side) const {
        Node *held = owner(p_side);
        return held != nullptr ? api(p_side)->get_prediction_engine()->slot_of(
                                     netw::NetwEntity::of(held)
                                 )
                               : int64_t(-1);
    }

    BookSizes books(int p_side) const {
        netw::NetwMultiplayer *session = api(p_side);
        BookSizes sizes;
        sizes.slots_open = session->get_prediction_engine()->open_count();
        sizes.store_slots = int64_t(session->store().live_slot_count());
        sizes.bindings = int64_t(session->property_binding_book().count());
        sizes.derived_sets
            = int64_t(session->sync_pipeline()
                          ->counters()[StringName("derived_sets_active")]);
        return sizes;
    }

    int64_t sync_counter(int p_side, const char *p_name) const {
        return int64_t(
            api(p_side)->sync_pipeline()->counters()[StringName(p_name)]
        );
    }

    int64_t rows_sent(int p_side) const {
        return sync_counter(p_side, "row_frames_out")
            + sync_counter(p_side, "retained_frames_out")
            + sync_counter(p_side, "window_frames_out");
    }

    Ref<netw::NetwPrediction> inputs(int p_side) const {
        return Ref<netw::NetwPrediction>(rig.prediction_handle("P", p_side));
    }

    static int64_t commands_received(
        const Ref<netw::NetwPrediction> &p_inputs
    ) {
        return int64_t(p_inputs->get_stats()->to_dictionary().get(
            StringName("command_frames_received"),
            0
        ));
    }

    void drive(int p_ticks, const Vector2 &p_motion) {
        for (int at = 0; at < p_ticks; ++at) {
            if (Node *controlled = owner(CLIENT)) {
                controlled->set("motion", p_motion);
            }
            rig.step_ticks(1);
        }
    }

    double height(int p_side) const {
        const Vector2 at = owner(p_side)->get("position");
        return double(at.y);
    }
};

void check_empty(const BookSizes &p_sizes) {
    NETW_CHECK_EQ(p_sizes.slots_open, int64_t(0));
    NETW_CHECK_EQ(p_sizes.store_slots, int64_t(0));
    NETW_CHECK_EQ(p_sizes.bindings, int64_t(0));
    NETW_CHECK_EQ(p_sizes.derived_sets, int64_t(0));
}

void readded_in_one_frame_keeps_predicting(int p_side) {
    ExitRig stand;
    const int64_t seated = stand.slot(p_side);
    REQUIRE(seated >= 0);
    Node *owner = stand.owner(p_side);
    Node *parent = owner->get_parent();
    parent->remove_child(owner);
    parent->add_child(owner);
    owner->set_owner(parent);

    stand.drive(10, Vector2(0.0, 1.0));
    NETW_CHECK_EQ(stand.slot(p_side), seated);
    NETW_CHECK_EQ(stand.books(SERVER).slots_open, int64_t(1));
    NETW_CHECK_EQ(stand.books(CLIENT).slots_open, int64_t(1));
    const bool client_moved = stand.height(CLIENT) > 0.0;
    const bool server_moved = stand.height(SERVER) > 0.0;
    CHECK(client_moved);
    CHECK(server_moved);
}

void left_out_of_the_tree_releases_its_slot(int p_side) {
    ExitRig stand;
    const int other = p_side == SERVER ? CLIENT : SERVER;
    Node *owner = stand.owner(p_side);
    const RID copy = stand.rig.entity_of("P", other);
    const Ref<netw::NetwPrediction> server_inputs = stand.inputs(SERVER);
    const FaultWatch faults;
    const int64_t rows_out = stand.rows_sent(p_side);
    const int64_t rows_in = stand.sync_counter(other, "derived_frames_in");
    const int64_t commands_in = ExitRig::commands_received(server_inputs);
    owner->get_parent()->remove_child(owner);

    stand.drive(5, Vector2(0.0, 1.0));
    const bool copy_despawned = stand.api(other)->entity_get_state(copy)
        == netw::NetwMultiplayer::ENTITY_STATE_DEAD;
    NETW_CHECK_EQ(faults.errors(), int64_t(0));
    NETW_CHECK_EQ(stand.books(p_side).slots_open, int64_t(0));
    NETW_CHECK_EQ(copy_despawned, p_side == SERVER);
    NETW_CHECK_EQ(
        stand.books(other).slots_open,
        int64_t(p_side == SERVER ? 0 : 1)
    );
    NETW_CHECK_EQ(stand.rows_sent(p_side), rows_out);
    NETW_CHECK_EQ(stand.sync_counter(other, "derived_frames_in"), rows_in);
    NETW_CHECK_EQ(ExitRig::commands_received(server_inputs), commands_in);

    stand.api(p_side)->clear_session_state();
    stand.rig.step_ticks(1);
    check_empty(stand.books(p_side));
    memdelete(owner);
}

void freed_off_the_tree_releases_every_book(int p_side) {
    ExitRig stand;
    const int other = p_side == SERVER ? CLIENT : SERVER;
    Node *owner = stand.owner(p_side);
    const FaultWatch faults;
    owner->get_parent()->remove_child(owner);
    memdelete(owner);

    stand.drive(5, Vector2(0.0, 1.0));
    NETW_CHECK_EQ(faults.errors(), int64_t(0));
    check_empty(stand.books(p_side));
    NETW_CHECK_EQ(
        stand.books(other).slots_open,
        int64_t(p_side == SERVER ? 0 : 1)
    );
}

TEST_CASE(
    "[Networked][Predict][TreeExit] a server's predicted owner taken out of "
    "the tree and put back in the same frame keeps its slot and predicts on"
) {
    readded_in_one_frame_keeps_predicting(SERVER);
}

TEST_CASE(
    "[Networked][Predict][TreeExit] a client's predicted owner taken out of "
    "the tree and put back in the same frame keeps its slot and predicts on"
) {
    readded_in_one_frame_keeps_predicting(CLIENT);
}

TEST_CASE(
    "[Networked][Predict][TreeExit] a server's predicted owner left out of "
    "the tree releases its slot and despawns the client's copy without an "
    "error, and a session clear releases the rest"
) {
    left_out_of_the_tree_releases_its_slot(SERVER);
}

TEST_CASE(
    "[Networked][Predict][TreeExit] a client's predicted owner left out of "
    "the tree releases its slot without an error, and a session clear "
    "releases the rest"
) {
    left_out_of_the_tree_releases_its_slot(CLIENT);
}

TEST_CASE(
    "[Networked][Predict][TreeExit] a server's predicted owner freed off the "
    "tree releases every book on the server without an error"
) {
    freed_off_the_tree_releases_every_book(SERVER);
}

TEST_CASE(
    "[Networked][Predict][TreeExit] a client's predicted owner freed off the "
    "tree releases every book on the client without an error"
) {
    freed_off_the_tree_releases_every_book(CLIENT);
}

} // namespace TestNetwPredictTreeExitLaws

#endif
