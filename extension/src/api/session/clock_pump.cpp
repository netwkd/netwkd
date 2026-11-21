#include "godot/callable.hpp"
#include "godot/engine.hpp"
#include "godot/multiplayer.hpp"
#include "godot/object.hpp"
#include "godot/scene_tree.hpp"
#include "godot/utility.hpp"
#include "netw/api/clock_config.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/log.hpp"
#include "netw/session_decl.hpp"

using namespace godot;

namespace netw {

namespace {

constexpr const char *SIG_PHYSICS_FRAME = "physics_frame";
constexpr const char *SIG_SERVER_DISCONNECTED = "server_disconnected";
constexpr const char *SIG_CONNECTION_FAILED = "connection_failed";
constexpr const char *SIG_CONNECTED_TO_SERVER = "connected_to_server";

bool authoring_in_editor() {
    Engine *engine = Engine::get_singleton();
    return engine != nullptr && engine->is_editor_hint();
}

} // namespace

Error NetwMultiplayer::clock_initialize(const Ref<NetwClockConfig> &p_from) {
    return clock_commit(p_from);
}

Error NetwMultiplayer::clock_commit(const Ref<NetwClockConfig> &p_from) {
    const int slot = int(session_decl::KIND_CLOCK_CONFIG)
        - int(session_decl::KIND_SESSION_CONFIG);
    if (config_consumption[slot].consumed) {
        fault::raise(
            this,
            NETW_FAULT_DECLARED_ONLINE,
            {"{Netw.configure_clock}", String(), "the clock"}
        );
        return ERR_ALREADY_IN_USE;
    }
    NETW_BUG_COND_V(p_from.is_null(), ERR_INVALID_PARAMETER);
    config_consumption[slot].consumed = true;
    const Error configured = clock_configure(p_from);
    if (configured != OK) {
        config_consumption[slot].consumed = false;
        return configured;
    }
    clock_set_mismatch_action(
        MismatchAction(p_from->get_tickrate_mismatch_action())
    );
    connect_once(
        Signal(this, SIG_SERVER_DISCONNECTED),
        callable_mp(this, &NetwMultiplayer::clock_drop_synchronization)
    );
    connect_once(
        Signal(this, SIG_CONNECTION_FAILED),
        callable_mp(this, &NetwMultiplayer::clock_drop_synchronization)
    );
    clock_engine().set_handshakes_on_enter(true);
    connect_once(
        Signal(this, SIG_CONNECTED_TO_SERVER),
        callable_mp(this, &NetwMultiplayer::clock_start_handshake)
    );
    clock_attach_pump();
    clock_start_handshake();
    return OK;
}

void NetwMultiplayer::clock_offer_fallback(
    const Ref<NetwClockConfig> &p_draft,
    Object *p_source
) {
    const ObjectID source = gd::instance_id(p_source);
    if (clock_fallback.is_valid() && clock_fallback_source != source
        && gd::object_of(clock_fallback_source) != nullptr) {
        fault::raise(
            this,
            NETW_FAULT_DECLARED_TWICE,
            {"{Netw.configure_clock}", "this session"},
            source
        );
        return;
    }
    clock_fallback = p_draft;
    clock_fallback_source = source;
    declarations_changed();
}

void NetwMultiplayer::clock_attach_pump() {
    if (clock_pump_attached) {
        return;
    }
    SceneTree *tree = gd::scene_tree();
    if (tree == nullptr || session_get_root() == nullptr) {
        return;
    }
    clock_pump_attached = true;
    connect_once(
        Signal(tree, SIG_PHYSICS_FRAME),
        callable_mp(this, &NetwMultiplayer::clock_on_physics_frame)
    );
}

void NetwMultiplayer::clock_detach_pump() {
    if (!clock_pump_attached) {
        return;
    }
    clock_pump_attached = false;
    SceneTree *tree = gd::scene_tree();
    if (tree == nullptr) {
        return;
    }
    disconnect_once(
        Signal(tree, SIG_PHYSICS_FRAME),
        callable_mp(this, &NetwMultiplayer::clock_on_physics_frame)
    );
}

void NetwMultiplayer::clock_on_physics_frame() {
    if (authoring_in_editor() || !clock_engine().get_configured()) {
        return;
    }
    const Ref<NetwMultiplayer> keep(this);
    SceneTree *tree = gd::scene_tree();
    if (tree == nullptr || session_paused || session_get_root() == nullptr) {
        return;
    }
    if (clock_engine().get_manual_tick()) {
        return;
    }
    const Ref<MultiplayerPeer> peer = NETW_API_VIRTUAL(get_multiplayer_peer)();
    if (peer.is_null()
        || peer->get_connection_status()
            != MultiplayerPeer::CONNECTION_CONNECTED) {
        return;
    }
    Node *root = tree->get_root();
    if (root == nullptr) {
        return;
    }
    const double delta = root->get_physics_process_delta_time();
    const bool hosting = is_host();
    ++step_depth;
    if (delta > clock_engine().get_stall_threshold() && !hosting) {
        clock_request_handshake();
    }
    clock_physics_step(delta);
    clock_step_auto_config(delta);
    if (!hosting && clock_consume_ping_due(delta)) {
        clock_owe_ping(!clock_engine().is_settled());
    }
    --step_depth;
    announce_flush();
    effect_drain();
    predict_free_retired();
}

void NetwMultiplayer::clock_start_handshake() {
    if (is_host() || !clock_engine().get_configured()) {
        return;
    }
    const Ref<MultiplayerPeer> peer = NETW_API_VIRTUAL(get_multiplayer_peer)();
    const bool connected = peer.is_valid()
        && peer->get_connection_status()
            == MultiplayerPeer::CONNECTION_CONNECTED;
    if (!connected) {
        return;
    }
    const uint64_t generation = uint64_t(gd::instance_id(peer.ptr()));
    if (!clock_engine().handshake_opens(generation)) {
        return;
    }
    clock_request_handshake();
}

void NetwMultiplayer::clock_drop_synchronization() {
    clock_set_synchronized(false);
    clock_engine().auto_config_cancel();
}

void NetwMultiplayer::clock_auto_configure_offset() {
    if (!clock_engine().get_configured()) {
        fault::raise(
            this,
            NETW_FAULT_NOT_RUNNING,
            {"The display delay measurement", String(), "the clock"}
        );
        return;
    }
    if (is_host()) {
        return;
    }
    NETW_INFO(
        sys::CLOCK,
        "The clock started measuring its display offset for 5 seconds."
    );
    clock_engine().auto_config_start();
}

void NetwMultiplayer::clock_step_auto_config(double p_delta) {
    const int64_t measured = clock_engine().auto_config_step(p_delta);
    if (measured == Clock::NO_MEASUREMENT) {
        return;
    }
    NETW_INFO(
        sys::CLOCK,
        "The clock measured a display offset of %d.",
        int(measured)
    );
}

void NetwMultiplayer::clock_release() {
    clock_detach_pump();
    clock_engine().release();
    clock_fallback = Ref<NetwClockConfig>();
}

} // namespace netw
