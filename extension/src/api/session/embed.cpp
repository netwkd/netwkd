#include "netw/api/netw_multiplayer.hpp"

#include "godot/node.hpp"
#include "godot/object.hpp"
#include "netw/api/replication_core.hpp"
#include "netw/colors.hpp"
#include "netw/log.hpp"
#include "netw/profile.hpp"
#include "netw/scene_core.hpp"
#include "netw/stage.hpp"
#include "netw/subsystems.hpp"

using namespace godot;
using namespace netw;

namespace netw {

void NetwMultiplayer::embed_set_phase(EmbedPhase p_phase) {
    embed_phase_value = p_phase;
}

bool NetwMultiplayer::embed_claim_dispose() {
    if (embed_disposing) {
        return false;
    }
    embed_disposing = true;
    return true;
}

void NetwMultiplayer::embed_settle() {
    NETW_ZONE_NC("embed_settle", colors::SCENE);
    if (embed_phase_value != EMBED_PHASE_DECLARING) {
        return;
    }
    embed_set_phase(EMBED_PHASE_SETTLING);
    embed_set_phase(EMBED_PHASE_LIVE);
}

Error NetwMultiplayer::embed_poll_transport() {
    NETW_STAGE(TRANSPORT, "Transport receive", colors::TRANSPORT);
    if (inner.is_null()) {
        NETW_TRACE(
            sys::TRANSPORT,
            "The transport was polled before the session existed."
        );
        return ERR_UNCONFIGURED;
    }
    const Error err = inner->poll();
    session_announce_transfer();
    transfer_send_resync();
    transfer_watch_resync();
    liveness_poll_now();
    rpc_sweep_deferred_calls();
    rpc_sweep_transactions(receive_tick());
    return err;
}

} // namespace netw
