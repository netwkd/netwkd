#include "support/frame_drive.h"
#include "support/internals.h"
#include "support/netw_test.h"

#include "godot/node.hpp"
#include "godot/physics_body.hpp"
#include "godot/scene_tree.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/entity_options.hpp"
#include "netw/api/entity_record.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/promise.hpp"
#include "netw/liveness_core.hpp"

#if defined(NETW_TIER_HOSTED)

using namespace godot;
using netw_test::Internals;

namespace NetwTests {

namespace {

constexpr int CARRY_FRAME_CAP = 16;

struct AbandonEvidence {
    bool driven = false;
    bool settled_when_the_body_went = false;
    bool settled = false;
    bool failed = false;
    int code = -1;
    int frames_spent = 0;
};

AbandonEvidence &abandon_evidence() {
    static AbandonEvidence evidence;
    return evidence;
}

struct CarryRun {
    Ref<netw::NetwMultiplayer> core;
    Ref<netw::NetwPromise> settling;
    Node *root = nullptr;
    CharacterBody2D *mover = nullptr;
    RID mover_handle;
    RID target_handle;

    RID seat(Node *p_container) {
        const Ref<netw::NetwEntity> entity
            = netw::NetwEntity::ensure(p_container);
        const RID handle = Internals::get_liveness_core(core)->entity_create();
        core->liveness_bind(
            handle,
            Internals::get_liveness_core(core)->reserve_route(),
            entity,
            p_container
        );
        p_container->set_meta(
            netw::NetwMultiplayer::wrapper_meta(),
            Variant(entity)
        );
        return handle;
    }

    Node *level_named(const char *p_name) {
        Node *level = memnew(Node);
        level->set_name(p_name);
        root->add_child(level);
        return level;
    }

    CarryRun() {
        core.instantiate();
        root = memnew(Node);
        root->set_name("CarryStand");
        netw::gd::scene_root()->add_child(root);
        netw::gd::scene_tree()->set_multiplayer(
            Ref<MultiplayerAPI>(core.ptr()),
            root->get_path()
        );
        Node *source = level_named("Source");
        target_handle = seat(level_named("Destination"));
        mover = memnew(CharacterBody2D);
        mover->set_name("Crate");
        source->add_child(mover);
        mover_handle = seat(mover);
    }

    void drop_mover() {
        mover->get_parent()->remove_child(mover);
        memdelete(mover);
        mover = nullptr;
    }

    ~CarryRun() {
        settling.unref();
        if (mover != nullptr) {
            drop_mover();
        }
        core->scene_dispose();
        netw::gd::scene_tree()->set_multiplayer(
            Ref<MultiplayerAPI>(),
            root->get_path()
        );
        core.unref();
        netw::gd::scene_root()->remove_child(root);
        memdelete(root);
    }
};

class CarryScenario final : public netw_test::FrameScenario {
    CarryRun *live = nullptr;
    int step = 0;

public:
    bool advance() override {
        if (netw::gd::scene_root() == nullptr) {
            return false;
        }
        AbandonEvidence &seen = abandon_evidence();
        if (step == 0) {
            live = new CarryRun();
            live->settling
                = live->core->scene_move_entity(
                    live->mover_handle,
                    live->target_handle
                );
            seen.driven = true;
            ++step;
            return true;
        }
        if (step == 1) {
            live->drop_mover();
            seen.settled_when_the_body_went = live->settling->get_is_settled();
            ++step;
            return true;
        }
        if (!live->settling->get_is_settled() && step < CARRY_FRAME_CAP) {
            ++step;
            return true;
        }
        seen.frames_spent = step;
        seen.settled = live->settling->get_is_settled();
        seen.failed = live->settling->get_is_failed();
        seen.code = live->settling->get_code();
        delete live;
        live = nullptr;
        return false;
    }
};

NETW_FRAME_SCENARIO(CarryScenario, carry_scenario);

} // namespace

TEST_CASE(
    "[Networked][Scene][Frame] CF5 a body freed mid-flight abandons its "
    "carry and answers the promise, so nothing dereferences it on the next "
    "frame and no move is left waiting forever"
) {
    const AbandonEvidence &seen = abandon_evidence();
    REQUIRE(seen.driven);
    CHECK_FALSE(seen.settled_when_the_body_went);
    CHECK(seen.settled);
    CHECK(seen.failed);
    NETW_CHECK_EQ(seen.code, int(ERR_UNAVAILABLE));
    NETW_CHECK_LT(seen.frames_spent, CARRY_FRAME_CAP);
}

} // namespace NetwTests

#endif
