#include "support/loopback_rig.h"

#if defined(NETW_TIER_HOSTED)

#include "godot/node.hpp"
#include "godot/object.hpp"
#include "godot/spatial_node.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/promise.hpp"
#include "netw/wire/registry.hpp"
#include "support/fault_log.h"
#include "support/internals.h"
#include "support/minted_script.h"
#include "support/value_flow_stand.h"

namespace TestLifecycleMoveLaws {

using namespace godot;
using netw::NetwEntity;
using netw::NetwMultiplayer;
using netw::NetwPromise;
using netw_test::Internals;
using netw_test::LoopbackRig;

constexpr int TICKRATE = 30;
constexpr int SETTLE_TICKS = 24;
constexpr int64_t SHELF = 2;

const char *PROBE_SOURCE = R"(extends Node3D

var counter := 0
var seats := []
var synced_value: int:
	get:
		return frame_tag() * 1000 + counter
	set(value):
		seats.append(Vector2i(value, frame_tag()))


func _init() -> void:
	Netw.configure_property(self, &"synced_value").broadcast()


func frame_tag() -> int:
	var parent := get_parent()
	if parent == null:
		return 0
	if parent.name == &"Shelf":
		return 2
	if parent.name == &"Hand":
		return 3
	if parent.name == &"Ghost":
		return 9
	return 1
)";

Node *build_avatar(const Variant &p_name) {
    Node3D *made = memnew(Node3D);
    made->set_name(String(p_name));
    made->set_position(Vector3(2.0, 0.0, 1.0));
    Node3D *hand = memnew(Node3D);
    hand->set_name("Hand");
    hand->set_position(Vector3(0.5, 1.0, 0.0));
    made->add_child(hand);
    return made;
}

Node *build_cube(const Variant &p_name, const Variant &p_script) {
    const Ref<Script> script = p_script;
    Node *made
        = Object::cast_to<Node>(netw::gd::live_object(script->call("new")));
    made->set_name(String(p_name));
    const Ref<NetwEntity> entity = NetwEntity::ensure(made);
    entity->set_lifecycle(NetwEntity::LIFECYCLE_CONTROLLER);
    entity->set_authority(NetwEntity::AUTHORITY_CONTROLLER);
    entity->set_transfer(NetwEntity::TRANSFER_REQUESTABLE);
    return made;
}

Array named(const char *p_name) {
    Array out;
    out.push_back(String(p_name));
    return out;
}

Array one_type() {
    Array out;
    out.push_back(int(Variant::STRING));
    return out;
}

Node3D *plain_node(Node *p_parent, const char *p_name) {
    Node3D *made = memnew(Node3D);
    made->set_name(p_name);
    p_parent->add_child(made);
    return made;
}

int64_t frames_in(
    NetwMultiplayer *p_session,
    int64_t p_from,
    const char *p_name
) {
    const Dictionary rows
        = p_session->attribution_snapshot()["frames_in_by_channel"];
    const Dictionary channels = rows.get(p_from, Dictionary());
    return int64_t(channels.get(
        int64_t(netw::wire::builtin_channel(StringName(p_name))),
        0
    ));
}

int64_t counter_of(NetwMultiplayer *p_session, const char *p_name) {
    const Dictionary counted = Internals::replication_plane(p_session)
                                   ->get_spawn_pipeline()
                                   ->counters();
    return int64_t(counted.get(StringName(p_name), -1));
}

struct Seats {
    int64_t seated = 0;
    int64_t from_the_branch = 0;
};

class MoveStage {
public:
    LoopbackRig rig;
    Ref<Script> probe;
    Node *arenas[3] = {nullptr, nullptr, nullptr};
    int avatar = 0;
    int cube = 0;

    MoveStage() : rig(2) {
        rig.mount();
        netw_test::flow_clocks(rig, TICKRATE);
        rig.mirror_child("Arena");
        for (int side = -1; side < 2; ++side) {
            arenas[side + 1]
                = rig.branch(side)->get_node_or_null(NodePath("Arena"));
            REQUIRE(arenas[side + 1] != nullptr);
            plain_node(arenas[side + 1], "Shelf");
        }
        probe = netw_test::minted_script(PROBE_SOURCE);
        REQUIRE(probe.is_valid());
        avatar = rig.spawn_registered(
            StringName("lifecycle_avatar"),
            callable_mp_static(&build_avatar),
            named("Avatar"),
            one_type(),
            arenas[0]
        );
        cube = rig.spawn_registered(
            StringName("lifecycle_cube"),
            callable_mp_static(&build_cube).bind(probe),
            named("Cube"),
            one_type(),
            arenas[0]
        );
        NetwEntity::of(rig.route_node(cube))->set_controller(rig.peer_id(0));
        rig.step_ticks(6);
    }

    NetwMultiplayer *side(int p_side) const {
        return p_side < 0 ? rig.server() : rig.client(p_side);
    }

    Node *cube_on(int p_side) const {
        return rig.route_node(cube, p_side);
    }

    Node *hand_on(int p_side) const {
        Node *held = rig.route_node(avatar, p_side);
        return held != nullptr ? held->get_node_or_null(NodePath("Hand"))
                               : nullptr;
    }

    Node *named_on(int p_side, const char *p_name) const {
        return arenas[p_side + 1]->get_node_or_null(NodePath(p_name));
    }

    int64_t anchor_on(int p_side) const {
        return int64_t(side(p_side)->liveness_route_anchor(cube));
    }

    Ref<NetwPromise> moves_on(int p_side, Node *p_parent) {
        NetwMultiplayer *author = side(p_side);
        return author->entity_reparent(
            Internals::entity_at(author, cube),
            p_parent
        );
    }

    void write_ticks(int p_ticks) {
        Node *writer = cube_on(0);
        for (int step = 0; step < p_ticks; ++step) {
            if (writer != nullptr) {
                writer->set(
                    StringName("counter"),
                    int64_t(writer->get(StringName("counter"))) + 1
                );
            }
            rig.step_ticks(1);
        }
    }

    void clear_seats() {
        for (int at = -1; at < 2; ++at) {
            if (Node *held = cube_on(at)) {
                held->set(StringName("seats"), Array());
            }
        }
    }

    Seats seats_on(int p_side, int64_t p_branch) const {
        Seats seen;
        Node *held = cube_on(p_side);
        if (held == nullptr) {
            return seen;
        }
        const Array seats = held->get(StringName("seats"));
        for (int at = 0; at < seats.size(); ++at) {
            const Vector2i seat = seats[at];
            seen.seated += 1;
            if (int64_t(seat.x) / 1000 == p_branch) {
                seen.from_the_branch += 1;
            }
        }
        return seen;
    }

    bool under(int p_side, Node *p_parent) const {
        Node *held = cube_on(p_side);
        return held != nullptr && p_parent != nullptr
            && held->get_parent() == p_parent;
    }
};

PackedByteArray move_op(
    NetwMultiplayer *p_author,
    int64_t p_route,
    Node *p_parent,
    const NetwMultiplayer::AnchorRevision &p_base
) {
    netw::wire::WriteStream stream;
    uint64_t revision = p_base.revision;
    uint64_t author = p_base.author;
    REQUIRE(p_author->verb_head_write(stream, p_route));
    REQUIRE(p_author->anchor_encode(stream, p_parent));
    REQUIRE(stream.varuint(revision, 5));
    REQUIRE(stream.varuint(author, 5));
    REQUIRE(stream.align_verify());
    return stream.to_bytes();
}

TEST_CASE(
    "[Netw][Lifecycle][Spawn][SceneTree] arbiter, a controller's lane "
    "opened at the revision number the session already installed for its own "
    "move names another author, so it seats nowhere even when it reaches the "
    "session before the controller's move does"
) {
    netw_test::FaultWatch expected_faults;
    expected_faults.expect_warning("*(error 4)*[NW1000]*", 1);

    MoveStage stage;
    REQUIRE(stage.cube_on(0) != nullptr);
    stage.write_ticks(6);
    stage.clear_seats();

    NetwMultiplayer *session = stage.rig.server();
    NetwMultiplayer *author = stage.rig.client(0);
    const int author_peer = stage.rig.peer_id(0);
    const NetwMultiplayer::AnchorRevision base
        = author->anchor_installed(stage.cube);

    stage.rig.hold(0);
    const Ref<NetwPromise> ruled = session->entity_reparent(
        Internals::entity_at(session, stage.cube),
        stage.hand_on(-1)
    );
    stage.write_ticks(2);
    NETW_REQUIRE_EQ(stage.anchor_on(-1), int64_t(2));

    stage.rig.hold(-1);
    const Ref<NetwPromise> refused
        = stage.moves_on(0, stage.named_on(0, "Shelf"));
    stage.write_ticks(2);
    NETW_REQUIRE_EQ(stage.anchor_on(0), int64_t(2));
    stage.rig.session()->purge_packets_from(author_peer);
    stage.rig.release(-1);
    stage.rig.release(0);
    const int64_t control_in = frames_in(session, author_peer, "ROW_CONTROL");
    stage.write_ticks(SETTLE_TICKS);

    NETW_CHECK_GT(
        frames_in(session, author_peer, "ROW_CONTROL") - control_in,
        int64_t(0)
    );
    CHECK(NetwEntity::of(stage.cube_on(0))->has_structure_ops());
    CHECK(stage.under(0, stage.named_on(0, "Shelf")));
    NETW_CHECK_EQ(counter_of(session, "lifecycle_ops_refused"), int64_t(0));

    session->spawn_handle_reparent_frame(
        move_op(author, stage.cube, stage.named_on(0, "Shelf"), base),
        author_peer
    );
    stage.write_ticks(SETTLE_TICKS);

    CHECK(ruled->get_is_settled());
    CHECK_FALSE(ruled->get_is_failed());
    CHECK(refused->get_is_failed());
    NETW_CHECK_EQ(refused->get_code(), ERR_UNAUTHORIZED);
    CHECK(stage.under(-1, stage.hand_on(-1)));
    CHECK(stage.under(0, stage.hand_on(0)));
    CHECK(stage.under(1, stage.hand_on(1)));
    NETW_CHECK_EQ(stage.anchor_on(0), stage.anchor_on(-1));
    NETW_CHECK_EQ(stage.anchor_on(1), stage.anchor_on(-1));
    NETW_CHECK_EQ(
        int64_t(author->anchor_installed(stage.cube).author),
        int64_t(session->anchor_installed(stage.cube).author)
    );

    const Seats at_session = stage.seats_on(-1, SHELF);
    const Seats at_observer = stage.seats_on(1, SHELF);
    NETW_CHECK_EQ(at_session.from_the_branch, int64_t(0));
    NETW_CHECK_EQ(at_observer.from_the_branch, int64_t(0));
    NETW_CHECK_GT(at_session.seated, int64_t(0));
}

} // namespace TestLifecycleMoveLaws

#endif
