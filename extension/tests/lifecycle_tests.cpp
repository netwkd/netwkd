#include "support/netw_test.h"

#if defined(NETW_TIER_HOSTED)

#include "support/fault_log.h"
#include "support/internals.h"
#include "support/loopback_rig.h"
#include "support/minted_script.h"
#include "support/netw_call_log.h"
#include "support/netw_cells.h"
#include "support/value_flow_stand.h"

#include "godot/multiplayer_synchronizer.hpp"
#include "godot/node.hpp"
#include "godot/object.hpp"
#include "godot/scene_tree.hpp"
#include "godot/script.hpp"
#include "godot/spatial_node.hpp"
#include "godot/variant.hpp"
#include "netw/api/context.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/entity_options.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/prediction.hpp"
#include "netw/api/promise.hpp"
#include "netw/lifecycle/rule.hpp"

namespace TestLifecycle {

using namespace godot;
using netw::Netw;
using netw::NetwEntity;
using netw::NetwMultiplayer;
using netw::NetwPromise;
using netw::lifecycle::Claim;
using netw::lifecycle::Destination;
using netw::lifecycle::Facts;
using netw::lifecycle::Kind;
using netw::lifecycle::Ruling;
using netw::lifecycle::Verdict;
using netw_test::CallLog;
using netw_test::FaultWatch;
using netw_test::Internals;
using netw_test::law_broken;
using netw_test::law_held;
using netw_test::LawRowFor;
using netw_test::LawVerdict;
using netw_test::LoopbackRig;

enum PeerClass {
    SESSION,
    CONFIRMED_CONTROLLER,
    IMMEDIATE_CLAIMANT,
    REQUESTABLE_CLAIMANT,
    OTHER_PEER,
};

enum Base {
    BASE_CURRENT,
    BASE_STALE,
};

const Kind KINDS[] = {Kind::SPAWN, Kind::DESPAWN, Kind::REPARENT};
const char *KIND_NAMES[] = {"spawn", "despawn", "reparent"};

const PeerClass PEERS[] = {
    SESSION,
    CONFIRMED_CONTROLLER,
    IMMEDIATE_CLAIMANT,
    REQUESTABLE_CLAIMANT,
    OTHER_PEER,
};
const char *PEER_NAMES[] = {
    "session",
    "confirmed-controller",
    "immediate-claimant",
    "requestable-claimant",
    "other",
};

const Destination DESTINATIONS[] = {
    Destination::OK,
    Destination::OUTSIDE,
    Destination::INSIDE_MOVER,
    Destination::DYING,
    Destination::UNRESOLVABLE,
};
const char *DESTINATION_NAMES[] = {
    "ok",
    "outside",
    "inside-mover",
    "dying",
    "unresolvable",
};

const Base BASES[] = {BASE_CURRENT, BASE_STALE};
const char *BASE_NAMES[] = {"current", "stale"};

struct RuleCell {
    String label;
    int kind = 0;
    PeerClass peer = SESSION;
    bool declared = false;
    bool predicted = false;
    bool native = false;
    bool player_bound = false;
    bool load_on_spawn = false;
    bool replaying = false;
    bool halted = false;
    int destination = 0;
    Base base = BASE_CURRENT;

    Facts facts() const {
        Facts out;
        out.kind = KINDS[kind];
        out.peer_is_server = peer == SESSION;
        out.peer_is_controller = peer == CONFIRMED_CONTROLLER;
        out.peer_claim = peer == IMMEDIATE_CLAIMANT ? Claim::IMMEDIATE
            : peer == REQUESTABLE_CLAIMANT          ? Claim::REQUESTABLE
                                                    : Claim::NONE;
        out.declared = declared;
        out.predicted = predicted;
        out.native = native;
        out.player_bound = player_bound;
        out.load_on_spawn = load_on_spawn;
        out.replaying = replaying;
        out.minting_halted = halted;
        out.destination = DESTINATIONS[destination];
        out.revision = 3;
        out.base = base == BASE_STALE ? 2 : 3;
        return out;
    }

    bool off_session() const {
        return peer != SESSION;
    }

    bool places() const {
        return KINDS[kind] != Kind::DESPAWN;
    }
};

bool halted(const RuleCell &p_cell) {
    return p_cell.halted;
}

bool replaying(const RuleCell &p_cell) {
    return p_cell.replaying;
}

bool unauthorized(const RuleCell &p_cell) {
    if (!p_cell.off_session()) {
        return false;
    }
    if (!p_cell.declared) {
        return true;
    }
    if (KINDS[p_cell.kind] == Kind::SPAWN) {
        return false;
    }
    return p_cell.peer != CONFIRMED_CONTROLLER
        && p_cell.peer != IMMEDIATE_CLAIMANT;
}

bool unavailable(const RuleCell &p_cell) {
    return p_cell.off_session()
        && (p_cell.predicted || p_cell.native || p_cell.player_bound
            || p_cell.load_on_spawn);
}

bool misplaced_by_the_caller(const RuleCell &p_cell) {
    const Destination at = DESTINATIONS[p_cell.destination];
    return p_cell.places()
        && (at == Destination::OUTSIDE || at == Destination::INSIDE_MOVER);
}

bool misplaced_in_flight(const RuleCell &p_cell) {
    const Destination at = DESTINATIONS[p_cell.destination];
    return p_cell.off_session() && p_cell.places()
        && (at == Destination::DYING || at == Destination::UNRESOLVABLE);
}

bool stale(const RuleCell &p_cell) {
    return p_cell.off_session() && KINDS[p_cell.kind] != Kind::SPAWN
        && p_cell.base == BASE_STALE;
}

struct RuleRefusal {
    const char *reason;
    bool (*applies)(const RuleCell &);
    Verdict verdict;
    Error code;
};

const RuleRefusal RULE_REFUSALS[] = {
    {"minting is halted for a session transfer",
     &halted,
     Verdict::STATIC,
     ERR_UNAVAILABLE},
    {"the verb was issued during a prediction replay",
     &replaying,
     Verdict::STATIC,
     ERR_BUSY},
    {"the peer is not the session, and neither the confirmed controller of a "
     "declared entity nor its immediate claimant",
     &unauthorized,
     Verdict::STATIC,
     ERR_UNAUTHORIZED},
    {"a controller op on a predicted, native, player-bound or load_on_spawn "
     "entity",
     &unavailable,
     Verdict::STATIC,
     ERR_UNAVAILABLE},
    {"a destination outside the session root or inside the moved entity",
     &misplaced_by_the_caller,
     Verdict::STATIC,
     ERR_INVALID_PARAMETER},
    {"a controller op whose destination is dying or unresolvable at the "
     "session",
     &misplaced_in_flight,
     Verdict::DYNAMIC,
     ERR_INVALID_PARAMETER},
    {"a controller op built on a stale anchor revision",
     &stale,
     Verdict::DYNAMIC,
     ERR_UNAUTHORIZED},
};

const RuleRefusal *first_refusal(const RuleCell &p_cell) {
    for (const RuleRefusal &refusal : RULE_REFUSALS) {
        if (refusal.applies(p_cell)) {
            return &refusal;
        }
    }
    return nullptr;
}

struct RuleRun {
    const RuleCell &cell;
    Ruling ruling;
    const RuleRefusal *expected = nullptr;

    explicit RuleRun(const RuleCell &p_cell)
        : cell(p_cell), ruling(netw::lifecycle::rule(p_cell.facts())),
          expected(first_refusal(p_cell)) {
    }
};

typedef LawRowFor<RuleRun> RuleLaw;

LawVerdict law_code(const RuleRun &p_run) {
    const Error wanted = p_run.expected != nullptr ? p_run.expected->code : OK;
    if (p_run.ruling.code != wanted) {
        return law_broken(
            "the rule answered %d where %s names %d",
            int(p_run.ruling.code),
            p_run.expected != nullptr ? p_run.expected->reason : "admission",
            int(wanted)
        );
    }
    return law_held();
}

LawVerdict law_kind(const RuleRun &p_run) {
    const Verdict wanted
        = p_run.expected != nullptr ? p_run.expected->verdict : Verdict::ADMIT;
    if (p_run.ruling.verdict != wanted) {
        return law_broken(
            "the rule ruled kind %d where %s names %d",
            int(p_run.ruling.verdict),
            p_run.expected != nullptr ? p_run.expected->reason : "admission",
            int(wanted)
        );
    }
    return law_held();
}

LawVerdict law_session_arbitrates(const RuleRun &p_run) {
    if (!p_run.cell.off_session() && p_run.ruling.verdict == Verdict::DYNAMIC) {
        return law_broken("the session's own op was refused dynamically");
    }
    return law_held();
}

const RuleLaw RULE_LAWS[] = {
    {"code", "every cell answers the first refusal it earns, or OK", &law_code},
    {"kind",
     "a refusal the author could have known is static, one only the "
     "session can know is dynamic",
     &law_kind},
    {"session-arbitrates",
     "the session's own op is never refused as a race",
     &law_session_arbitrates},
};

RuleCell cell_at(int p_index) {
    RuleCell cell;
    int at = p_index;
    cell.kind = at % 3;
    at /= 3;
    cell.peer = PEERS[at % 5];
    at /= 5;
    cell.declared = at % 2 == 1;
    at /= 2;
    cell.predicted = at % 2 == 1;
    at /= 2;
    cell.native = at % 2 == 1;
    at /= 2;
    cell.player_bound = at % 2 == 1;
    at /= 2;
    cell.load_on_spawn = at % 2 == 1;
    at /= 2;
    cell.replaying = at % 2 == 1;
    at /= 2;
    cell.halted = at % 2 == 1;
    at /= 2;
    cell.destination = at % 5;
    at /= 5;
    cell.base = BASES[at % 2];
    cell.label = vformat(
        "%s/%s/%s%s%s%s%s%s%s/%s/%s",
        KIND_NAMES[cell.kind],
        PEER_NAMES[cell.peer],
        cell.declared ? "declared" : "undeclared",
        cell.predicted ? "+predicted" : "",
        cell.native ? "+native" : "",
        cell.player_bound ? "+player-bound" : "",
        cell.load_on_spawn ? "+load-on-spawn" : "",
        cell.replaying ? "+replaying" : "",
        cell.halted ? "+halted" : "",
        DESTINATION_NAMES[cell.destination],
        BASE_NAMES[cell.base]
    );
    return cell;
}

constexpr int CELL_COUNT = 3 * 5 * 2 * 2 * 2 * 2 * 2 * 2 * 2 * 5 * 2;

TEST_CASE(
    "[Networked][Lifecycle][Law] every spawn, move and despawn a peer asks "
    "for answers the code its facts earn, and only the session can refuse "
    "it as a race"
) {
    int refused_dynamically = 0;
    for (int index = 0; index < CELL_COUNT; index++) {
        const RuleCell cell = cell_at(index);
        const RuleRun run(cell);
        if (run.ruling.verdict == Verdict::DYNAMIC) {
            refused_dynamically += 1;
        }
        for (const RuleLaw &law : RULE_LAWS) {
            NETW_CELL(law, cell);
            NETW_LAW_HOLDS(law, run);
        }
    }
    CHECK(refused_dynamically > 0);
}

constexpr int TICKRATE = 30;
constexpr int SETTLE_TICKS = 24;
constexpr int64_t STAMPED = 99;
constexpr int64_t SHELF = 2;
constexpr int64_t HAND = 3;
constexpr int64_t GHOST = 9;

constexpr const char *CRATE_SOURCE = R"(extends Node3D

var seen := []
var notes := []
var hp := 0:
	set(value):
		hp = value
		seen.append(value)

func _init() -> void:
	Netw.configure_property(self, &"hp").broadcast().on_spawn()

@rpc("any_peer", "call_remote", "reliable")
func note(tag: int) -> void:
	notes.append(tag)
)";

constexpr const char *PROBE_SOURCE = R"(extends Node3D

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

Node3D *spatial(const String &p_name, const Vector3 &p_at) {
    Node3D *made = memnew(Node3D);
    made->set_name(p_name);
    made->set_position(p_at);
    made->set_rotation(Vector3(0.0, real_t(Math_PI * 0.5), 0.0));
    return made;
}

Node *scripted(const Variant &p_script, const Variant &p_name) {
    const Ref<Script> script = p_script;
    Node *made
        = Object::cast_to<Node>(netw::gd::live_object(script->call("new")));
    made->set_name(String(p_name));
    return made;
}

Node *build_crate(const Variant &p_name, const Variant &p_script) {
    Node *made = scripted(p_script, p_name);
    Netw::configure_entity(made)->set_lifecycle(
        NetwEntity::LIFECYCLE_CONTROLLER
    );
    return made;
}

Node *build_tethered(
    const Variant &p_name,
    const Variant &p_tether,
    const Variant &p_script
) {
    Node *made = build_crate(p_name, p_script);
    made->set_meta(StringName("tether"), p_tether);
    return made;
}

Node *build_served(
    const Variant &p_name,
    const Variant &p_script,
    const Variant &p_spawned
) {
    Node3D *made = Object::cast_to<Node3D>(scripted(p_script, p_name));
    made->set_position(Vector3(2.0, 0.0, 1.0));
    const Ref<NetwEntity> entity = Netw::configure_entity(made);
    entity->set_lifecycle(NetwEntity::LIFECYCLE_CONTROLLER);
    entity->set_transfer(NetwEntity::TRANSFER_REQUESTABLE);
    made->connect(StringName("ready"), Callable(p_spawned));
    return made;
}

Node *build_anchor(const Variant &p_name) {
    Node3D *made = memnew(Node3D);
    made->set_name(String(p_name));
    return made;
}

Node *build_board(const Variant &p_name) {
    Node3D *made = memnew(Node3D);
    made->set_name(String(p_name));
    MultiplayerSynchronizer *sync = memnew(MultiplayerSynchronizer);
    sync->set_name("Sync");
    sync->set_root_path(NodePath(".."));
    sync->set_visibility_public(false);
    made->add_child(sync);
    return made;
}

Node *build_holder(const Variant &p_name) {
    Node3D *made = spatial(String(p_name), Vector3(4.0, 0.0, 0.0));
    Netw::configure_entity(made)->set_lifecycle(
        NetwEntity::LIFECYCLE_CONTROLLER
    );
    return made;
}

Node *build_detaching(const Variant &p_name) {
    Node3D *made = spatial(String(p_name), Vector3(1.0, 2.0, 0.0));
    Netw::configure_entity(made)->set_on_parent_despawn(
        NetwEntity::PARENT_DESPAWN_DETACH
    );
    return made;
}

Node *build_cascading(const Variant &p_name) {
    return spatial(String(p_name), Vector3(0.0, 1.0, 3.0));
}

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

Node *build_cube(
    const Variant &p_name,
    const Variant &p_script,
    const Variant &p_transfer
) {
    Node *made = scripted(p_script, p_name);
    const Ref<NetwEntity> entity = Netw::configure_entity(made);
    entity->set_lifecycle(NetwEntity::LIFECYCLE_CONTROLLER);
    entity->set_authority(NetwEntity::AUTHORITY_CONTROLLER);
    entity->set_transfer(NetwEntity::Transfer(int(p_transfer)));
    return made;
}

Transform3D world_of(Node *p_node) {
    Node3D *held = Object::cast_to<Node3D>(p_node);
    return held != nullptr ? held->get_global_transform() : Transform3D();
}

bool stands_at(const Transform3D &p_seen, const Transform3D &p_before) {
    return p_seen.origin.distance_to(p_before.origin) < 1e-4
        && p_seen.basis.is_equal_approx(p_before.basis);
}

struct Stage {
    LoopbackRig rig;
    Ref<Script> crate_script;
    Node *arenas[3] = {nullptr, nullptr, nullptr};

    Stage() : rig(2) {
        rig.mount();
        netw_test::flow_clocks(rig, TICKRATE);
        rig.mirror_child("Arena");
        for (int side = -1; side < 2; ++side) {
            arenas[side + 1]
                = rig.branch(side)->get_node_or_null(NodePath("Arena"));
            REQUIRE(arenas[side + 1] != nullptr);
            plain_node(arenas[side + 1], "Shelf");
        }
        plain_node(arenas[1], "Ghost");
        crate_script = netw_test::minted_script(CRATE_SOURCE);
        REQUIRE(crate_script.is_valid());
    }

    NetwMultiplayer *side(int p_side) const {
        return p_side < 0 ? rig.server() : rig.client(p_side);
    }

    Node *arena_on(int p_side) const {
        return arenas[p_side + 1];
    }

    Node *named_on(int p_side, const char *p_name) const {
        return arena_on(p_side)->get_node_or_null(NodePath(p_name));
    }

    Node *at(int p_route, int p_side) const {
        return rig.route_node(p_route, p_side);
    }

    bool holds(int p_route, int p_side) const {
        return at(p_route, p_side) != nullptr;
    }

    bool under(int p_route, int p_side, Node *p_parent) const {
        Node *held = at(p_route, p_side);
        return held != nullptr && p_parent != nullptr
            && held->get_parent() == p_parent;
    }

    int held_nowhere(int p_route) const {
        for (int at_side = -1; at_side < 2; ++at_side) {
            if (holds(p_route, at_side)) {
                return at_side;
            }
        }
        return -2;
    }

    int64_t controller_on(int p_route, int p_side) const {
        Node *held = at(p_route, p_side);
        return held != nullptr ? NetwEntity::of(held)->get_controller() : 0;
    }

    void teach_crates(NetwMultiplayer *p_api) {
        rig.register_constructor(
            p_api,
            StringName("authored_crate"),
            callable_mp_static(&build_crate).bind(crate_script),
            one_type()
        );
        Array tethered = one_type();
        tethered.push_back(int(Variant::OBJECT));
        rig.register_constructor(
            p_api,
            StringName("authored_tethered"),
            callable_mp_static(&build_tethered).bind(crate_script),
            tethered
        );
    }

    void teach_crates() {
        for (int at_side = -1; at_side < 2; ++at_side) {
            teach_crates(side(at_side));
        }
        rig.step_ticks(4);
    }

    Node *author_spawns(const char *p_id, const Array &p_args, Node *p_under) {
        NetwMultiplayer *author = rig.client(0);
        const RID entity
            = author->spawn_registered(StringName(p_id), p_args, 0);
        Node *node = author->entity_get_node(entity);
        if (node != nullptr && p_under != nullptr) {
            node->set(StringName("hp"), int64_t(1));
            p_under->add_child(node);
        }
        return node;
    }

    int route_of(Node *p_node, int p_side = 0) const {
        if (p_node == nullptr) {
            return 0;
        }
        NetwMultiplayer *api = side(p_side);
        return int(api->entity_get_route(Internals::entity_of(api, p_node)));
    }

    int serve(
        const char *p_id,
        const Callable &p_build,
        const char *p_name,
        Node *p_under
    ) {
        return rig.spawn_registered(
            StringName(p_id),
            p_build,
            named(p_name),
            one_type(),
            p_under
        );
    }

    void author_despawns(int p_route) {
        NetwMultiplayer *author = rig.client(0);
        REQUIRE(at(p_route, 0) != nullptr);
        NETW_REQUIRE_EQ(
            author->entity_despawn(
                Internals::entity_at(author, p_route),
                netw::NetwDespawnOpts::create(StringName("scrapped"))
            ),
            OK
        );
    }
};

struct World {
    String label;
};

typedef LawRowFor<World> Row;

LawVerdict client_spawns(const World &) {
    Stage stage;
    stage.teach_crates();
    const int64_t author = stage.rig.peer_id(0);
    Node *made = stage.author_spawns(
        "authored_crate",
        named("Crate"),
        stage.arena_on(0)
    );
    if (made == nullptr) {
        return law_broken("the client could not spawn the crate");
    }
    const int route = stage.route_of(made);
    stage.rig.step_ticks(SETTLE_TICKS);

    const StringName id = NetwEntity::of(made)->get_name();
    for (int side = -1; side < 2; ++side) {
        if (!stage.under(route, side, stage.arena_on(side))) {
            return law_broken("peer %d does not hold it in the arena", side);
        }
        if (NetwEntity::of(stage.at(route, side))->get_name() != id) {
            return law_broken("peer %d names it another entity id", side);
        }
    }
    if (stage.at(route, 0) != made) {
        return law_broken("the author's own node was replaced");
    }
    if (stage.controller_on(route, -1) != author
        || stage.controller_on(route, 1) != author) {
        return law_broken("the author does not control what it spawned");
    }

    const int late = stage.rig.late_client(StringName("late"));
    stage.teach_crates(stage.rig.client(late));
    stage.rig.mirror_late(late, "Arena");
    stage.rig.step_ticks(SETTLE_TICKS);
    Node *late_arena
        = stage.rig.branch(late)->get_node_or_null(NodePath("Arena"));
    if (!stage.under(route, late, late_arena)) {
        return law_broken("a peer joining later never received it");
    }
    if (NetwEntity::of(stage.at(route, late))->get_name() != id) {
        return law_broken("the late peer names it another entity id");
    }
    return law_held();
}

LawVerdict spawn_under_unknown_parent(const World &) {
    FaultWatch faults;
    faults.expect_warning("*its parent '*'*[NW0026]*", 1);
    Stage stage;
    stage.teach_crates();
    Node *made = stage.author_spawns(
        "authored_crate",
        named("Lost"),
        stage.named_on(0, "Ghost")
    );
    const int route = stage.route_of(made);
    stage.rig.step_ticks(SETTLE_TICKS);

    const int holder = stage.held_nowhere(route);
    if (holder != -2) {
        return law_broken("peer %d holds a spawn the server refused", holder);
    }
    return law_held();
}

LawVerdict spawn_naming_a_despawned_entity(const World &) {
    Stage stage;
    stage.teach_crates();
    const int anchor = stage.serve(
        "authored_anchor",
        callable_mp_static(&build_anchor),
        "Anchor",
        stage.arena_on(-1)
    );
    stage.rig.step_ticks(4);
    Node *tether = stage.at(anchor, 0);
    if (tether == nullptr) {
        return law_broken("the client never held the anchor");
    }

    stage.rig.hold(0);
    Node *gone = stage.at(anchor, -1);
    NetwEntity::of(gone)->despawn(Ref<netw::NetwDespawnOpts>());
    gone->get_parent()->remove_child(gone);
    stage.rig.step_ticks(4);
    Array args = named("Tethered");
    args.push_back(tether);
    Node *made
        = stage.author_spawns("authored_tethered", args, stage.arena_on(0));
    const int route = stage.route_of(made);
    stage.rig.step_ticks(4);
    stage.rig.release(0);
    stage.rig.step_ticks(SETTLE_TICKS);

    const int holder = stage.held_nowhere(route);
    if (holder != -2) {
        return law_broken("peer %d holds a spawn naming a dead entity", holder);
    }
    return law_held();
}

void free_link(Object *p_link) {
    memdelete(p_link);
}

LawVerdict freed_chain(const World &) {
    constexpr int LINKS = 20;
    FaultWatch faults;
    faults.expect_warning("*[NW0010]*", 1);
    Stage stage;
    int routes[LINKS];
    for (int link = 0; link < LINKS; ++link) {
        routes[link] = stage.serve(
            "chain_link",
            callable_mp_static(&build_anchor),
            "Link",
            stage.arena_on(-1)
        );
    }
    stage.rig.step_ticks(4);
    for (int link = 0; link + 1 < LINKS; ++link) {
        NetwEntity::of(stage.at(routes[link], -1))
            ->connect(
                StringName("despawned"),
                callable_mp_static(&free_link)
                    .bind(stage.at(routes[link + 1], -1))
            );
    }

    memdelete(stage.at(routes[0], -1));
    stage.rig.step_ticks(SETTLE_TICKS);

    for (const int route : routes) {
        const int holder = stage.held_nowhere(route);
        if (holder != -2) {
            return law_broken("peer %d kept a link of the chain", holder);
        }
    }
    return law_held();
}

void stamp_arrival(Node *p_node, Node *p_arena) {
    if (p_node->get_parent() == p_arena) {
        p_node->set(StringName("hp"), STAMPED);
    }
}

LawVerdict first_values_are_the_servers(const World &) {
    Stage stage;
    stage.teach_crates();
    SceneTree *tree = netw::gd::scene_tree();
    const Callable stamp
        = callable_mp_static(&stamp_arrival).bind(stage.arena_on(-1));
    tree->connect(StringName("node_added"), stamp);
    Node *made = stage.author_spawns(
        "authored_crate",
        named("Crate"),
        stage.arena_on(0)
    );
    const int route = stage.route_of(made);
    stage.rig.step_ticks(2);
    tree->disconnect(StringName("node_added"), stamp);
    stage.rig.step_ticks(SETTLE_TICKS);

    Node *observed = stage.at(route, 1);
    const Array seen
        = observed != nullptr ? Array(observed->get("seen")) : Array();
    if (seen.is_empty() || int64_t(seen[0]) != STAMPED) {
        return law_broken("the observer's first hp was not the server's");
    }
    if (int64_t(made->get(StringName("hp"))) != 1) {
        return law_broken("the author's own hp was overwritten");
    }
    return law_held();
}

LawVerdict call_in_the_spawn_frame(const World &) {
    Stage stage;
    stage.teach_crates();
    Node *made = stage.author_spawns(
        "authored_crate",
        named("Crate"),
        stage.arena_on(0)
    );
    const int route = stage.route_of(made);
    made->rpc(StringName("note"), int64_t(7));
    stage.rig.step_ticks(SETTLE_TICKS);

    for (int side = -1; side < 2; side += 2) {
        Node *held = stage.at(route, side);
        const Array notes
            = held != nullptr ? Array(held->get("notes")) : Array();
        if (notes.size() != 1 || int64_t(notes[0]) != 7) {
            return law_broken(
                "peer %d ran the call %d times",
                side,
                int(notes.size())
            );
        }
    }
    if (Array(made->get(StringName("notes"))).size() != 0) {
        return law_broken("the author ran its own remote call");
    }
    return law_held();
}

LawVerdict author_leaves(const World &) {
    Stage stage;
    stage.teach_crates();
    Node *made = stage.author_spawns(
        "authored_crate",
        named("Left"),
        stage.arena_on(0)
    );
    const int route = stage.route_of(made);
    stage.rig.step_ticks(SETTLE_TICKS);

    stage.rig.drop_client(0);
    stage.rig.step_ticks(SETTLE_TICKS);
    if (!stage.holds(route, -1) || !stage.holds(route, 1)) {
        return law_broken("the entity left with its author");
    }
    if (stage.controller_on(route, -1) != 1
        || stage.controller_on(route, 1) != 1) {
        return law_broken("control did not revert to the server");
    }

    NetwMultiplayer *session = stage.rig.server();
    Node *freed = stage.at(route, -1);
    if (session->entity_despawn(
            Internals::entity_at(session, route),
            Ref<netw::NetwDespawnOpts>()
        )
        != OK) {
        return law_broken("the server could not despawn it");
    }
    freed->get_parent()->remove_child(freed);
    stage.rig.step_ticks(SETTLE_TICKS);
    if (stage.holds(route, -1) || stage.holds(route, 1)) {
        return law_broken("the server's despawn did not reach every peer");
    }
    return law_held();
}

LawVerdict spawn_under_a_hidden_parent(const World &) {
    Stage stage;
    stage.teach_crates();
    const int board = stage.serve(
        "authored_board",
        callable_mp_static(&build_board),
        "Board",
        stage.arena_on(-1)
    );
    MultiplayerSynchronizer *sync = Object::cast_to<MultiplayerSynchronizer>(
        stage.at(board, -1)->get_node_or_null(NodePath("Sync"))
    );
    sync->set_visibility_for(stage.rig.peer_id(0), true);
    sync->update_visibility(0);
    stage.rig.step_ticks(SETTLE_TICKS);
    if (!stage.holds(board, 0) || stage.holds(board, 1)) {
        return law_broken("the board is not shown to client 0 alone");
    }

    Node *made = stage.author_spawns(
        "authored_crate",
        named("Stroke"),
        stage.at(board, 0)
    );
    const int route = stage.route_of(made);
    stage.rig.step_ticks(SETTLE_TICKS);
    if (!stage.under(route, -1, stage.at(board, -1))
        || !stage.under(route, 0, stage.at(board, 0))) {
        return law_broken("the stroke is not under the board");
    }
    if (stage.holds(route, 1)) {
        return law_broken("a peer that cannot see the board got the stroke");
    }
    return law_held();
}

struct Served {
    Stage stage;
    CallLog spawned;
    int crate = 0;

    Served() {
        crate = stage.serve(
            "lifecycle_crate",
            callable_mp_static(&build_served)
                .bind(stage.crate_script, spawned.callable("spawned")),
            "Crate",
            stage.arena_on(-1)
        );
        NetwEntity::of(stage.at(crate, -1))
            ->set_controller(stage.rig.peer_id(0));
        stage.rig.step_ticks(6);
    }
};

LawVerdict client_despawns(const World &) {
    Served served;
    Stage &stage = served.stage;
    CallLog despawning[3];
    for (int side = -1; side < 2; ++side) {
        const Ref<NetwEntity> crate
            = NetwEntity::of(stage.at(served.crate, side));
        for (const char *signal : {"despawning", "despawned"}) {
            crate->connect(
                StringName(signal),
                despawning[side + 1].callable(signal)
            );
        }
    }

    Node *mine = stage.at(served.crate, 0);
    stage.author_despawns(served.crate);
    if (mine->is_inside_tree()) {
        return law_broken("the author still holds what it despawned");
    }
    stage.rig.step_ticks(SETTLE_TICKS);
    for (int side = -1; side < 2; ++side) {
        if (stage.holds(served.crate, side)) {
            return law_broken("peer %d still holds the crate", side);
        }
        const Vector<StringName> heard = despawning[side + 1].order();
        if (heard.size() != 2 || heard[0] != StringName("despawning")
            || heard[1] != StringName("despawned")) {
            return law_broken(
                "peer %d heard %d signal(s), not despawning then despawned",
                side,
                int(heard.size())
            );
        }
        if (despawning[side + 1].args("despawning")
            != Array::make(StringName("scrapped"))) {
            return law_broken("peer %d heard another reason", side);
        }
    }
    return law_held();
}

LawVerdict refused_despawn_serves_back(const World &) {
    FaultWatch faults;
    faults.expect_warning("*(error 4)*[NW1000]*", 1);
    Served served;
    Stage &stage = served.stage;
    Node *written = stage.at(served.crate, 0);
    for (int step = 0; step < SETTLE_TICKS; ++step) {
        written->set(StringName("hp"), int64_t(step < 4 ? step : 5));
        stage.rig.step_ticks(1);
    }
    Node *observed = stage.at(served.crate, 1);
    const int built = served.spawned.count("spawned");

    stage.rig.hold(-1);
    stage.author_despawns(served.crate);
    stage.rig.step_ticks(2);
    Netw::reparent(stage.at(served.crate, -1), stage.named_on(-1, "Shelf"));
    stage.rig.step_ticks(2);
    stage.rig.release(-1);
    stage.rig.step_ticks(SETTLE_TICKS);

    Node *back = stage.at(served.crate, 0);
    if (back == nullptr || back != written) {
        return law_broken("the author did not get the same crate back");
    }
    if (int64_t(back->get(StringName("hp"))) != 5) {
        return law_broken("the crate came back without the server's hp");
    }
    if (!stage.under(served.crate, 0, stage.named_on(0, "Shelf"))) {
        return law_broken("the crate came back away from the server's place");
    }
    if (served.spawned.count("spawned") - built != 0) {
        return law_broken(
            "ready fired %d times",
            served.spawned.count("spawned") - built
        );
    }
    if (stage.controller_on(served.crate, 0) != stage.rig.peer_id(0)) {
        return law_broken("the author lost control");
    }
    if (stage.at(served.crate, 1) != observed) {
        return law_broken("the observer's crate was touched");
    }
    return law_held();
}

struct HolderTree {
    int holder = 0;
    int cube = 0;
    int satchel = 0;
    int coin = 0;
};

HolderTree spawn_holder_tree(Stage &p_stage) {
    HolderTree tree;
    tree.holder = p_stage.serve(
        "lifecycle_holder",
        callable_mp_static(&build_holder),
        "Holder",
        p_stage.arena_on(-1)
    );
    tree.cube = p_stage.serve(
        "lifecycle_detaching",
        callable_mp_static(&build_detaching),
        "Cube",
        p_stage.at(tree.holder, -1)
    );
    tree.satchel = p_stage.serve(
        "lifecycle_cascading",
        callable_mp_static(&build_cascading),
        "Satchel",
        p_stage.at(tree.holder, -1)
    );
    tree.coin = p_stage.serve(
        "lifecycle_detaching",
        callable_mp_static(&build_detaching),
        "Coin",
        p_stage.at(tree.satchel, -1)
    );
    NetwEntity::of(p_stage.at(tree.holder, -1))
        ->set_controller(p_stage.rig.peer_id(0));
    p_stage.rig.step_ticks(6);
    return tree;
}

LawVerdict despawn_cascades(const World &) {
    Stage stage;
    const HolderTree tree = spawn_holder_tree(stage);
    const int moved[2] = {tree.cube, tree.coin};
    Transform3D before[3][2];
    for (int side = -1; side < 2; ++side) {
        for (int at = 0; at < 2; ++at) {
            before[side + 1][at] = world_of(stage.at(moved[at], side));
        }
    }

    stage.author_despawns(tree.holder);
    stage.rig.step_ticks(SETTLE_TICKS);
    for (int side = -1; side < 2; ++side) {
        if (stage.holds(tree.holder, side) || stage.holds(tree.satchel, side)) {
            return law_broken("peer %d kept a despawned entity", side);
        }
        for (int at = 0; at < 2; ++at) {
            if (!stage.under(moved[at], side, stage.arena_on(side))) {
                return law_broken("peer %d did not detach entity %d", side, at);
            }
            if (!stands_at(
                    world_of(stage.at(moved[at], side)),
                    before[side + 1][at]
                )) {
                return law_broken("peer %d moved entity %d", side, at);
            }
        }
    }
    return law_held();
}

LawVerdict refused_despawn_reattaches(const World &) {
    FaultWatch faults;
    faults.expect_warning("*(error 4)*[NW1000]*", 1);
    Stage stage;
    const HolderTree tree = spawn_holder_tree(stage);
    const int moved[2] = {tree.cube, tree.coin};
    const int held_by[2] = {tree.holder, tree.satchel};

    stage.rig.hold(-1);
    stage.author_despawns(tree.holder);
    stage.rig.step_ticks(2);
    if (!stage.under(tree.cube, 0, stage.arena_on(0))) {
        return law_broken("the author did not detach the cube at once");
    }
    Netw::reparent(stage.at(tree.holder, -1), stage.named_on(-1, "Shelf"));
    stage.rig.step_ticks(2);
    stage.rig.release(-1);
    stage.rig.step_ticks(SETTLE_TICKS);

    if (!stage.holds(tree.holder, 0) || !stage.holds(tree.satchel, 0)) {
        return law_broken("the author did not get the holder back");
    }
    for (int at = 0; at < 2; ++at) {
        if (!stage.under(moved[at], 0, stage.at(held_by[at], 0))) {
            return law_broken(
                "entity %d was not put back under its parent",
                at
            );
        }
    }
    return law_held();
}

struct Seats {
    int64_t seated = 0;
    int64_t misplaced = 0;
    int64_t from_the_branch = 0;
};

struct Moving {
    Stage stage;
    Ref<Script> probe;
    int avatar = 0;
    int cube = 0;

    explicit Moving(
        NetwEntity::Transfer p_transfer = NetwEntity::TRANSFER_REQUESTABLE
    ) {
        probe = netw_test::minted_script(PROBE_SOURCE);
        avatar = stage.serve(
            "lifecycle_avatar",
            callable_mp_static(&build_avatar),
            "Avatar",
            stage.arena_on(-1)
        );
        cube = stage.serve(
            "lifecycle_cube",
            callable_mp_static(&build_cube).bind(probe, int(p_transfer)),
            "Cube",
            stage.arena_on(-1)
        );
        NetwEntity::of(cube_on(-1))->set_controller(stage.rig.peer_id(0));
        stage.rig.step_ticks(6);
        write_ticks(6);
        clear_seats();
    }

    Node *cube_on(int p_side) const {
        return stage.at(cube, p_side);
    }

    Node *hand_on(int p_side) const {
        Node *held = stage.at(avatar, p_side);
        return held != nullptr ? held->get_node_or_null(NodePath("Hand"))
                               : nullptr;
    }

    Ref<NetwPromise> moves(int p_side, Node *p_parent) {
        return Netw::reparent(cube_on(p_side), p_parent);
    }

    bool all_under(Node *const p_parents[3]) const {
        for (int side = -1; side < 2; ++side) {
            if (!stage.under(cube, side, p_parents[side + 1])) {
                return false;
            }
        }
        return true;
    }

    bool all_under_hand() const {
        Node *const hands[3] = {hand_on(-1), hand_on(0), hand_on(1)};
        return all_under(hands);
    }

    bool all_under(const char *p_name) const {
        Node *const parents[3] = {
            stage.named_on(-1, p_name),
            stage.named_on(0, p_name),
            stage.named_on(1, p_name),
        };
        return all_under(parents);
    }

    bool all_under_arena() const {
        Node *const parents[3] = {
            stage.arena_on(-1),
            stage.arena_on(0),
            stage.arena_on(1),
        };
        return all_under(parents);
    }

    void write_ticks(int p_ticks) {
        for (int step = 0; step < p_ticks; ++step) {
            if (Node *writer = cube_on(0)) {
                writer->set(
                    StringName("counter"),
                    int64_t(writer->get(StringName("counter"))) + 1
                );
            }
            stage.rig.step_ticks(1);
        }
    }

    void clear_seats() {
        for (int side = -1; side < 2; ++side) {
            if (Node *held = cube_on(side)) {
                held->set(StringName("seats"), Array());
            }
        }
    }

    Seats seats_on(int p_side, int64_t p_branch_a, int64_t p_branch_b) const {
        Seats seen;
        Node *held = cube_on(p_side);
        if (held == nullptr) {
            return seen;
        }
        const Array seats = held->get(StringName("seats"));
        for (int at = 0; at < seats.size(); ++at) {
            const Vector2i seat = seats[at];
            const int64_t written = int64_t(seat.x) / 1000;
            const int64_t holding = int64_t(seat.y);
            seen.seated += 1;
            if (written != holding && holding != 0) {
                seen.misplaced += 1;
            }
            if (written == p_branch_a || written == p_branch_b) {
                seen.from_the_branch += 1;
            }
        }
        return seen;
    }
};

LawVerdict client_moves(const World &) {
    Moving moving;
    const Ref<NetwPromise> moved = moving.moves(0, moving.hand_on(0));
    moving.write_ticks(SETTLE_TICKS);

    if (!moved->get_is_settled() || moved->get_is_failed()) {
        return law_broken("the move did not settle as done");
    }
    if (!moving.all_under_hand()) {
        return law_broken("a peer does not hold the cube in the hand");
    }
    for (int side = -1; side < 2; side += 2) {
        const Seats seen = moving.seats_on(side, HAND, HAND);
        if (seen.misplaced != 0 || seen.from_the_branch == 0) {
            return law_broken(
                "peer %d seated values from the wrong place",
                side
            );
        }
    }
    return law_held();
}

LawVerdict refused_move_chain(const World &) {
    FaultWatch faults;
    faults.expect_warning("*(error 4)*[NW1000]*", 1);
    faults.expect_warning("*(error 30)*", 1);
    Moving moving;
    moving.stage.rig.hold(0);
    const Ref<NetwPromise> first
        = moving.moves(0, moving.stage.named_on(0, "Ghost"));
    moving.write_ticks(2);
    const Ref<NetwPromise> second
        = moving.moves(0, moving.stage.named_on(0, "Shelf"));
    moving.write_ticks(6);
    moving.stage.rig.release(0);
    moving.write_ticks(SETTLE_TICKS);

    if (!first->get_is_failed() || first->get_code() != ERR_INVALID_PARAMETER) {
        return law_broken(
            "the move to a place the server lacks was not refused"
        );
    }
    if (!second->get_is_failed()) {
        return law_broken("a move built on a refused move went through");
    }
    if (!moving.all_under_arena()) {
        return law_broken(
            "a peer does not hold the cube where the server does"
        );
    }
    for (int side = -1; side < 2; side += 2) {
        const Seats seen = moving.seats_on(side, GHOST, SHELF);
        if (seen.from_the_branch != 0 || seen.misplaced != 0) {
            return law_broken(
                "peer %d seated values from the refused moves",
                side
            );
        }
    }
    if (moving.seats_on(-1, GHOST, SHELF).seated == 0) {
        return law_broken("the server seated nothing at all");
    }
    return law_held();
}

LawVerdict server_move_wins(const World &) {
    FaultWatch faults;
    faults.expect_warning("*(error 4)*[NW1000]*", 1);
    Moving moving;
    moving.stage.rig.hold(-1);
    const Ref<NetwPromise> refused
        = moving.moves(0, moving.stage.named_on(0, "Shelf"));
    moving.write_ticks(2);
    const Ref<NetwPromise> ruled = moving.moves(-1, moving.hand_on(-1));
    moving.write_ticks(2);
    moving.stage.rig.release(-1);
    moving.write_ticks(SETTLE_TICKS);

    if (!ruled->get_is_settled() || ruled->get_is_failed()) {
        return law_broken("the server's own move did not go through");
    }
    if (!refused->get_is_failed() || refused->get_code() != ERR_UNAUTHORIZED) {
        return law_broken("the client's move in flight was not refused");
    }
    if (!moving.all_under_hand()) {
        return law_broken(
            "a peer does not hold the cube where the server put it"
        );
    }
    return law_held();
}

LawVerdict drop_then_release(const World &) {
    Moving moving;
    Node *held = moving.cube_on(0);
    held->reparent(moving.stage.named_on(0, "Shelf"));
    const Ref<NetwPromise> released = NetwEntity::of(held)->release_control();
    moving.write_ticks(SETTLE_TICKS);

    if (!released->get_is_settled() || released->get_is_failed()) {
        return law_broken("the release did not settle as done");
    }
    if (!moving.all_under("Shelf")) {
        return law_broken("a peer does not hold the cube on the shelf");
    }
    if (moving.stage.controller_on(moving.cube, -1) != 1) {
        return law_broken("the server did not take control back");
    }
    return law_held();
}

void deny_control(const Ref<netw::NetwControlRequest> &p_request) {
    p_request->deny();
}

LawVerdict immediate_claim_refused(const World &) {
    FaultWatch faults;
    faults.expect_warning("*(error 4)*[NW1000]*", 1);
    Moving moving(NetwEntity::TRANSFER_IMMEDIATE);
    const Ref<NetwEntity> claimant = NetwEntity::of(moving.cube_on(1));
    NetwEntity::of(moving.cube_on(-1))
        ->set_control_handler(callable_mp_static(&deny_control));

    const Ref<NetwPromise> claimed = claimant->claim_control();
    const Ref<NetwPromise> moved
        = moving.moves(1, moving.stage.named_on(1, "Shelf"));
    if (moved->get_is_failed()) {
        return law_broken("a waiting immediate claim could not move at once");
    }
    moving.write_ticks(SETTLE_TICKS);

    if (!claimed->get_is_failed() || !moved->get_is_failed()) {
        return law_broken("the refused claim left its move standing");
    }
    if (claimant->get_controller() != moving.stage.rig.peer_id(0)
        || claimant->get_is_controlled_locally()) {
        return law_broken("the refused claimant kept control");
    }
    if (!moving.all_under_arena()) {
        return law_broken("a peer kept the refused claimant's move");
    }
    return law_held();
}

LawVerdict immediate_claim_granted(const World &) {
    Moving moving(NetwEntity::TRANSFER_IMMEDIATE);
    const Ref<NetwEntity> claimant = NetwEntity::of(moving.cube_on(1));
    const Ref<NetwPromise> claimed = claimant->claim_control();
    const Ref<NetwPromise> moved
        = moving.moves(1, moving.stage.named_on(1, "Shelf"));
    moving.write_ticks(SETTLE_TICKS);

    if (claimed->get_is_failed() || moved->get_is_failed()) {
        return law_broken("the granted claim or its move failed");
    }
    if (claimant->get_controller() != moving.stage.rig.peer_id(1)) {
        return law_broken("the claimant does not control the cube");
    }
    if (!moving.all_under("Shelf")) {
        return law_broken("a peer does not hold the cube on the shelf");
    }
    return law_held();
}

const Row ROWS[] = {
    {"a client spawns a LIFECYCLE_CONTROLLER crate, then a peer joins",
     "every peer and the late one hold it under one entity id, controlled "
     "by the author",
     &client_spawns},
    {"a client spawns under a parent the server does not have",
     "it is refused with a warning and no peer holds it",
     &spawn_under_unknown_parent},
    {"a client spawns naming an entity the server just despawned",
     "it is refused and no peer holds it",
     &spawn_naming_a_despawned_entity},
    {"the server frees a chain whose despawned handler frees the next link",
     "every link is gone on every peer a few frames later",
     &freed_chain},
    {"the server writes a client's new crate as it arrives",
     "the observer's first value is the server's, the author keeps its own",
     &first_values_are_the_servers},
    {"a client calls an RPC on a crate in the frame it spawns it",
     "it runs once on the server and on the observer",
     &call_in_the_spawn_frame},
    {"the client that spawned a crate leaves",
     "the crate stays under the server's control, and its despawn reaches "
     "every peer",
     &author_leaves},
    {"a client spawns under a board only it can see",
     "the server and the client hold it under the board and the other "
     "client never does",
     &spawn_under_a_hidden_parent},
    {"a client despawns the crate it controls",
     "it is gone on every peer and each hears despawning once with the "
     "client's reason, then despawned",
     &client_despawns},
    {"a client despawns while the server moves the crate",
     "the despawn is refused and the author gets the same crate back with "
     "the server's values and place, the observer untouched",
     &refused_despawn_serves_back},
    {"a client despawns a parent with detaching children",
     "the children stay under the grandparent where they stood and the "
     "rest go, on every peer",
     &despawn_cascades},
    {"a client despawns that parent while the server moves it",
     "the despawn is refused and the author puts each child back under its "
     "parent",
     &refused_despawn_reattaches},
    {"a client moves its cube into the avatar's hand",
     "every peer holds it in the hand and seats only values written there",
     &client_moves},
    {"a client moves to a place the server lacks, then moves again",
     "both are refused, every peer holds it where the server does, and no "
     "value from either move is seated",
     &refused_move_chain},
    {"the server moves the cube while the controller's move is in flight",
     "the client's move is refused and every peer follows the server",
     &server_move_wins},
    {"a controller drops the cube on the shelf and releases it",
     "every peer holds it on the shelf and the server controls it",
     &drop_then_release},
    {"a client claims a TRANSFER_IMMEDIATE cube, moves it, and is denied",
     "the claim and the move are both undone on every peer",
     &immediate_claim_refused},
    {"a client claims a TRANSFER_IMMEDIATE cube, moves it, and is granted",
     "the client controls it and every peer holds it where it moved",
     &immediate_claim_granted},
};

TEST_CASE(
    "[Networked][Lifecycle][SceneTree] every spawn, move and despawn "
    "reaches every peer once, and one the server refuses is undone "
    "on the author alone"
) {
    const World world = {String("two clients and a server on a loopback")};
    for (const Row &row : ROWS) {
        NETW_CELL(row, world);
        NETW_LAW_HOLDS(row, world);
    }
}

const char *BODY_ID = "lifecycle_body";

Node *build_body(
    const Variant &p_name,
    const Variant &p_lifecycle,
    const Variant &p_transfer
) {
    Node3D *made = memnew(Node3D);
    made->set_name(String(p_name));
    Node3D *pocket = memnew(Node3D);
    pocket->set_name("Pocket");
    made->add_child(pocket);
    const Ref<NetwEntity> entity = Netw::configure_entity(made);
    entity->set_lifecycle(NetwEntity::Lifecycle(int(p_lifecycle)));
    entity->set_authority(NetwEntity::AUTHORITY_CONTROLLER);
    entity->set_transfer(NetwEntity::Transfer(int(p_transfer)));
    return made;
}

Array body_args(const char *p_name, bool p_declared, bool p_requestable) {
    Array out;
    out.push_back(String(p_name));
    out.push_back(
        int(p_declared ? NetwEntity::LIFECYCLE_CONTROLLER
                       : NetwEntity::LIFECYCLE_SERVER)
    );
    out.push_back(
        int(p_requestable ? NetwEntity::TRANSFER_REQUESTABLE
                          : NetwEntity::TRANSFER_FIXED)
    );
    return out;
}

Array body_types() {
    Array out;
    out.push_back(int(Variant::STRING));
    out.push_back(int(Variant::INT));
    out.push_back(int(Variant::INT));
    return out;
}

enum Verb {
    VERB_REPARENT,
    VERB_DESPAWN,
    VERB_SPAWN,
};

enum Standing {
    STANDING_NONE,
    STANDING_CONTROLLER,
    STANDING_CLAIM_WAITING,
    STANDING_PREDICTED,
    STANDING_NATIVE,
    STANDING_PLAYER,
};

enum Target {
    TARGET_SHELF,
    TARGET_OUTSIDE,
    TARGET_POCKET,
};

struct Refusal {
    String label;
    Verb verb = VERB_REPARENT;
    bool declared = false;
    Standing standing = STANDING_NONE;
    Target target = TARGET_SHELF;
    Error code = ERR_UNAUTHORIZED;
};

struct RefusalSeen {
    bool materialized = false;
    bool answered = false;
    Error code = OK;
    int64_t errors = 0;
    int64_t coded = 0;
    int64_t named = 0;
    bool local_unchanged = false;
    bool session_unchanged = false;
};

void take_standing(LoopbackRig &p_rig, const Refusal &p_row, int p_route) {
    const Ref<NetwEntity> host = NetwEntity::of(p_rig.route_node(p_route));
    Node *body = p_rig.route_node(p_route, 0);
    const Ref<NetwEntity> here = NetwEntity::of(body);
    if (p_row.standing == STANDING_NONE) {
        return;
    }
    if (p_row.standing == STANDING_CLAIM_WAITING) {
        p_rig.hold(0);
        here->claim_control();
        p_rig.pump(4);
        return;
    }
    host->set_controller(p_rig.peer_id(0));
    p_rig.pump(6);
    if (p_row.standing == STANDING_PREDICTED) {
        p_rig.client(0)->predict_declare(here->get_rid_handle());
    }
    if (p_row.standing == STANDING_NATIVE) {
        MultiplayerSynchronizer *native = memnew(MultiplayerSynchronizer);
        native->set_name("Native");
        body->add_child(native);
        native->set_owner(body);
    }
}

Node *target_of(
    const Refusal &p_row,
    Node *p_body,
    Node *p_shelf,
    Node *p_outside
) {
    if (p_row.target == TARGET_OUTSIDE) {
        return p_outside;
    }
    if (p_row.target == TARGET_POCKET) {
        return p_body->get_node_or_null(NodePath("Pocket"));
    }
    return p_shelf;
}

void act(
    LoopbackRig &p_rig,
    const Refusal &p_row,
    int p_route,
    Node *p_target,
    RefusalSeen &r_seen
) {
    NetwMultiplayer *client = p_rig.client(0);
    Node *body = p_rig.route_node(p_route, 0);
    if (p_row.verb == VERB_REPARENT) {
        const Ref<NetwPromise> answered = Netw::reparent(body, p_target);
        r_seen.answered = answered.is_valid() && answered->get_is_failed();
        r_seen.code = answered.is_valid() ? answered->get_code() : Error(OK);
        return;
    }
    if (p_row.verb == VERB_DESPAWN) {
        r_seen.code = client->entity_despawn(
            Internals::entity_at(client, p_route),
            Ref<netw::NetwDespawnOpts>()
        );
        r_seen.answered = r_seen.code != OK;
        return;
    }
    r_seen.answered = !client
                           ->spawn_registered(
                               StringName(BODY_ID),
                               body_args("Spawned", p_row.declared, false),
                               0
                           )
                           .is_valid();
    r_seen.code = p_row.code;
}

RefusalSeen run_refusal(const Refusal &p_row) {
    RefusalSeen seen;
    LoopbackRig rig(1);
    rig.mount();
    Node *arena = rig.mirror_child("Arena");
    Node *shelves[2] = {
        plain_node(arena, "Shelf"),
        plain_node(rig.branch(0)->get_node_or_null(NodePath("Arena")), "Shelf"),
    };
    Node3D *outside = plain_node(netw::gd::scene_root(), "OutsideTheSession");

    const int route = rig.spawn_registered(
        StringName(BODY_ID),
        callable_mp_static(&build_body),
        body_args(
            "Crate",
            p_row.declared,
            p_row.standing == STANDING_CLAIM_WAITING
        ),
        body_types(),
        arena,
        p_row.standing == STANDING_PLAYER ? rig.player(0)->get_id() : 0
    );
    Node *body = rig.route_node(route, 0);
    Node *held = rig.route_node(route);
    seen.materialized = body != nullptr && held != nullptr;
    if (seen.materialized) {
        take_standing(rig, p_row, route);
        Node *before_local = body->get_parent();
        Node *before_session = held->get_parent();
        const int shelf_children = shelves[1]->get_child_count();
        const String root = p_row.verb == VERB_SPAWN ? String("Spawned")
                                                     : String(body->get_name());
        {
            const FaultWatch faults;
            act(rig,
                p_row,
                route,
                target_of(p_row, body, shelves[1], outside),
                seen);
            seen.errors = faults.errors();
            seen.coded = faults.errors_containing("[NW0041]");
            seen.named = faults.errors_containing(root);
        }
        rig.release(0);
        rig.pump(8);
        seen.local_unchanged = rig.route_node(route, 0) == body
            && body->get_parent() == before_local
            && shelves[1]->get_child_count() == shelf_children
            && outside->get_child_count() == 0;
        seen.session_unchanged = rig.route_node(route) == held
            && held->get_parent() == before_session
            && shelves[0]->get_child_count() == 0;
    }
    outside->get_parent()->remove_child(outside);
    memdelete(outside);
    return seen;
}

struct RefusalRun {
    const Refusal &row;
    RefusalSeen seen;

    explicit RefusalRun(const Refusal &p_row)
        : row(p_row), seen(run_refusal(p_row)) {
    }
};

typedef LawRowFor<RefusalRun> RefusalLaw;

LawVerdict law_names_the_setting(const RefusalRun &p_run) {
    if (!p_run.seen.materialized) {
        return law_broken("the client never held the crate");
    }
    if (p_run.seen.errors != 1) {
        return law_broken(
            "%d errors were raised, not one",
            int(p_run.seen.errors)
        );
    }
    if (p_run.seen.coded != 1 || p_run.seen.named != 1) {
        return law_broken("the error names neither the code nor the root");
    }
    return law_held();
}

LawVerdict law_answers_the_code(const RefusalRun &p_run) {
    if (!p_run.seen.answered) {
        return law_broken("the verb answered as if it went through");
    }
    if (p_run.seen.code != p_run.row.code) {
        return law_broken(
            "the verb answered %d, not %d",
            int(p_run.seen.code),
            int(p_run.row.code)
        );
    }
    return law_held();
}

LawVerdict law_changes_nothing(const RefusalRun &p_run) {
    if (!p_run.seen.local_unchanged) {
        return law_broken("the refused verb changed the client's tree");
    }
    if (!p_run.seen.session_unchanged) {
        return law_broken("the refused verb changed the server's tree");
    }
    return law_held();
}

const RefusalLaw REFUSAL_LAWS[] = {
    {"names-the-setting",
     "a refusal raises one error naming the root and NW0041",
     &law_names_the_setting},
    {"answers-the-code",
     "the verb answers its own failure value with the refusal's code",
     &law_answers_the_code},
    {"changes-nothing",
     "a refused verb changes no peer's tree",
     &law_changes_nothing},
};

TEST_CASE(
    "[Networked][Lifecycle][SceneTree] a spawn, move or despawn a client "
    "may not make is refused at the call, reported once, and changes "
    "nothing"
) {
    const Refusal REFUSALS[] = {
        {"a move of an entity not declared LIFECYCLE_CONTROLLER",
         VERB_REPARENT,
         false,
         STANDING_NONE,
         TARGET_SHELF,
         ERR_UNAUTHORIZED},
        {"a move by a client that does not control it",
         VERB_REPARENT,
         true,
         STANDING_NONE,
         TARGET_SHELF,
         ERR_UNAUTHORIZED},
        {"a move while a requested claim waits",
         VERB_REPARENT,
         true,
         STANDING_CLAIM_WAITING,
         TARGET_SHELF,
         ERR_UNAUTHORIZED},
        {"a move of a predicted entity",
         VERB_REPARENT,
         true,
         STANDING_PREDICTED,
         TARGET_SHELF,
         ERR_UNAVAILABLE},
        {"a move of an entity with a MultiplayerSynchronizer",
         VERB_REPARENT,
         true,
         STANDING_NATIVE,
         TARGET_SHELF,
         ERR_UNAVAILABLE},
        {"a move of a player's entity",
         VERB_REPARENT,
         true,
         STANDING_PLAYER,
         TARGET_SHELF,
         ERR_UNAVAILABLE},
        {"a move outside the session",
         VERB_REPARENT,
         true,
         STANDING_CONTROLLER,
         TARGET_OUTSIDE,
         ERR_INVALID_PARAMETER},
        {"a move into the entity itself",
         VERB_REPARENT,
         true,
         STANDING_CONTROLLER,
         TARGET_POCKET,
         ERR_INVALID_PARAMETER},
        {"a despawn of an entity not declared LIFECYCLE_CONTROLLER",
         VERB_DESPAWN,
         false,
         STANDING_NONE,
         TARGET_SHELF,
         ERR_UNAUTHORIZED},
        {"a despawn by a client that does not control it",
         VERB_DESPAWN,
         true,
         STANDING_NONE,
         TARGET_SHELF,
         ERR_UNAUTHORIZED},
        {"a spawn of an entity not declared LIFECYCLE_CONTROLLER",
         VERB_SPAWN,
         false,
         STANDING_NONE,
         TARGET_SHELF,
         ERR_UNAUTHORIZED},
    };
    for (const Refusal &row : REFUSALS) {
        const RefusalRun run(row);
        for (const RefusalLaw &law : REFUSAL_LAWS) {
            NETW_CELL(law, row);
            NETW_LAW_HOLDS(law, run);
        }
    }
}

TEST_CASE(
    "[Networked][Lifecycle][SceneTree] a raw Node.reparent by a client that "
    "may not move the entity is reported once and the server's copy stays"
) {
    FaultWatch faults;
    faults.expect_error("*'Crate'*[NW0041]*", 1);
    LoopbackRig rig(1);
    rig.mount();
    Node *arena = rig.mirror_child("Arena");
    Node *local_arena = rig.branch(0)->get_node_or_null(NodePath("Arena"));
    plain_node(arena, "Shelf");
    Node *shelf = plain_node(local_arena, "Shelf");
    const int route = rig.spawn_registered(
        StringName(BODY_ID),
        callable_mp_static(&build_body),
        body_args("Crate", false, false),
        body_types(),
        arena
    );
    Node *body = rig.route_node(route, 0);
    Node *held = rig.route_node(route);
    REQUIRE(body != nullptr);
    REQUIRE(held != nullptr);

    body->reparent(shelf);
    rig.pump(6);
    body->reparent(local_arena);
    rig.pump(6);
    body->reparent(shelf);
    rig.pump(6);

    CHECK(body->get_parent() == shelf);
    CHECK(held->get_parent() == arena);
}

} // namespace TestLifecycle

#endif
