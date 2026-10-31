#include "netw/fault_book.hpp"

using namespace godot;

namespace netw {

FaultText bug_text(const String &p_where) {
    return {
        vformat(
            "Networked stopped at %s because of a bug in Networked, not in "
            "this game.",
            p_where
        ),
        "Report it to Networked with this log."
    };
}

FaultText fault_text(netw_fault_code p_code, const FaultValues &v) {
    switch (p_code) {
        case NETW_FAULT_STALE_HANDLE:
        case NETW_FAULT_STALE_REF:
        case NETW_FAULT_LATE_ANSWER:
        case NETW_FAULT_UNANSWERED_ASK:
        case NETW_FAULT_SCRATCH_FULL:
        case NETW_FAULT_CALL_ORDER:
        case NETW_FAULT_ROUTE_STATE:
            return bug_text(v.name);
        case NETW_FAULT_REQUEST_EXPIRED:
            return {
                vformat(
                    "The %s handler did not answer within %d ticks, so peer "
                    "%d was refused with %s.",
                    v.name,
                    v.a,
                    v.peer,
                    v.detail
                ),
                "Answer sooner, or raise "
                "{NetwMemberConfig.spawn_request_deadline_ticks}."
            };
        case NETW_FAULT_MALFORMED_DATAGRAM:
            return {
                vformat(
                    "Peer %d sent a packet this peer could not read, so it "
                    "was dropped.",
                    v.peer
                ),
                "Run the same Networked version on every peer."
            };
        case NETW_FAULT_DUPLICATE_PEER:
            return {
                vformat(
                    "A client connected as peer %d, which another peer "
                    "already holds, so the new connection was refused.",
                    v.peer
                ),
                "Call {NetwConnection.join} again on that client, which picks "
                "a new peer id with {ENetMultiplayerPeer}."
            };
        case NETW_FAULT_SETTLE_BOUND:
            return {
                vformat(
                    "Calls queued with {NetwMultiplayer.session_defer} kept "
                    "queuing more for %d passes, so the calls under '%s' "
                    "were dropped.",
                    v.a,
                    v.name
                ),
                vformat(
                    "Stop the call under '%s' from queuing itself again with "
                    "{NetwMultiplayer.session_defer}.",
                    v.name
                )
            };
        case NETW_FAULT_ROWS_WITHOUT_CLOCK:
            return {
                "This session replicates properties but has no clock, so "
                "none of their values is ever sent.",
                "Call {Netw.configure_clock} on every peer before the session "
                "starts."
            };
        case NETW_FAULT_SERVER_ONLY:
            return {
                vformat(
                    "%s was called on a client, so it did nothing.",
                    v.name
                ),
                "Call it on the server."
            };
        case NETW_FAULT_WRITE_REFUSED:
            return {
                vformat(
                    "Peer %d may not write '%s' on '%s', so its write was "
                    "dropped.",
                    v.peer,
                    v.name,
                    v.owner
                ),
                vformat(
                    "Call {NetwMemberConfig.any_peer} or "
                    "{NetwMemberConfig.controller} on '%s' if peer %d should "
                    "write it.",
                    v.name,
                    v.peer
                )
            };
        case NETW_FAULT_TICK_SPLITS_PHYSICS:
            return {
                vformat(
                    "Physics runs at %d steps per second and the tickrate is "
                    "%d, so peers drift apart.",
                    v.a,
                    v.b
                ),
                "Make physics_ticks_per_second a whole multiple of "
                "{NetwClockConfig.tickrate}."
            };
        case NETW_FAULT_UNKNOWN_PEER:
            return {
                vformat(
                    "%s got peer %d, which is not in this session.",
                    v.name,
                    v.peer
                ),
                "Pass the id of a connected peer, such as one from "
                "{NetwSession.players}."
            };
        case NETW_FAULT_DECLARED_ONLINE:
            return {
                vformat(
                    "%s came after the session started, so the running value "
                    "of %s is unchanged.",
                    v.name,
                    v.detail
                ),
                "Call it before the session starts, and finish the chain "
                "without await."
            };
        case NETW_FAULT_HOST_REFUSED:
            return {
                "The host could not join its own session, so it has no "
                "{NetwPlayer}.",
                vformat(
                    "Admit peer 1 in the {Netw.configure_admission} handler, "
                    "which refused it with %s.",
                    v.detail
                )
            };
        case NETW_FAULT_RPC_REFUSED:
            return {
                vformat(
                    "Peer %d called '%s' on '%s', which it may not call, so "
                    "the call was dropped.",
                    v.peer,
                    v.name,
                    v.owner
                ),
                vformat(
                    "Call {NetwMemberConfig.any_peer} or "
                    "{NetwMemberConfig.controller} on '%s' if peer %d should "
                    "call it.",
                    v.name,
                    v.peer
                )
            };
        case NETW_FAULT_SPAWN_NOT_PLACED:
            return {
                vformat("'%s' was spawned but never entered the tree.", v.name),
                "Add it to the tree before the next tick."
            };
        case NETW_FAULT_SPAWN_STILL_WAITING:
            return {
                vformat(
                    "Spawn function '%s' has not returned after %s seconds.",
                    v.name,
                    v.detail
                ),
                "Check that what it awaits can finish."
            };
        case NETW_FAULT_CONTROL_REFUSED:
            return {
                vformat(
                    "Peer %d asked for control of '%s', and "
                    "{NetwEntity.transfer} does not allow it, so it was "
                    "refused.",
                    v.peer,
                    v.name
                ),
                "Set {NetwEntity.transfer} to "
                "{NetwEntity.TRANSFER_REQUESTABLE} to allow requests."
            };
        case NETW_FAULT_PEER_MISMATCH:
            return {
                vformat(
                    "Peer %d and this peer differ in %s, so %s was refused.",
                    v.peer,
                    v.detail,
                    v.name
                ),
                vformat("Set up %s the same way on every peer.", v.detail)
            };
        case NETW_FAULT_SPAWN_MISMATCH:
            return {
                vformat(
                    "A spawn for route %d was dropped, because %s does not "
                    "exist on this peer.",
                    v.a,
                    v.detail
                ),
                vformat("Create %s the same way on every peer.", v.detail)
            };
        case NETW_FAULT_JOIN_ARGS_MISMATCH:
            return {
                vformat(
                    "Peer %d joined with arguments the {Netw.configure_join} "
                    "handler does not take, so it was refused.",
                    v.peer
                ),
                "Pass the arguments the handler declares to "
                "{Netw.prepare_join} on the client."
            };
        case NETW_FAULT_JOIN_FLOOD:
            return {
                vformat(
                    "Peer %d sent more join requests than one join needs, so "
                    "the extra ones were dropped.",
                    v.peer
                ),
                "Call {Netw.prepare_join} once for each join on the client."
            };
        case NETW_FAULT_TRANSFER_TIMEOUT:
            return {
                vformat(
                    "Peer %d did not finish its world report within %d ticks "
                    "of the server moving, so it was disconnected.",
                    v.peer,
                    v.a
                ),
                "Raise {NetwSessionConfig.transfer_deadline_ticks}, or run "
                "the same Networked version on every peer."
            };
        case NETW_FAULT_SERVER_MOVE_REFUSED:
            return {
                vformat("%s made %s, so the session ended.", v.name, v.owner),
                vformat("Give the transport %s.", v.detail)
            };
        case NETW_FAULT_EMPTY_ARGUMENT:
            return {
                vformat("%s got no %s, so it did nothing.", v.name, v.detail),
                vformat("Pass %s to %s.", v.detail, v.name)
            };
        case NETW_FAULT_BAD_VALUE:
            return {
                vformat(
                    "%s got %s, which it does not accept, so it was ignored.",
                    v.name,
                    v.owner
                ),
                vformat("Pass %s.", v.detail)
            };
        case NETW_FAULT_UNKNOWN_NAME:
            return {
                vformat(
                    "%s names '%s', which '%s' does not declare.",
                    v.name,
                    v.detail,
                    v.owner
                ),
                vformat(
                    "Check the name, or declare it on '%s' first.",
                    v.owner
                )
            };
        case NETW_FAULT_DECLARED_TWICE:
            return {
                vformat(
                    "%s was declared twice on '%s' with different values, so "
                    "the second was refused.",
                    v.name,
                    v.owner
                ),
                "Declare it once, and keep what the first call returned."
            };
        case NETW_FAULT_DECLARED_AGAIN:
            return {
                vformat(
                    "%s of '%s' was declared twice, and the last one wins.",
                    v.name,
                    v.owner
                ),
                "Declare it once."
            };
        case NETW_FAULT_TOO_LATE:
            return {
                vformat(
                    "%s came too late for %s, so it was ignored.",
                    v.name,
                    v.owner
                ),
                vformat("Call %s before %s.", v.name, v.detail)
            };
        case NETW_FAULT_NOT_RUNNING:
            return {
                vformat(
                    "%s did nothing, because %s is not running on this peer.",
                    v.name,
                    v.detail
                ),
                "Call it after the session starts."
            };
        case NETW_FAULT_CONFLICTING_SETTINGS:
            return {
                vformat(
                    "'%s' sets %s, which has no effect with %s.",
                    v.owner,
                    v.name,
                    v.detail
                ),
                vformat("Remove %s, or change %s.", v.name, v.detail)
            };
        case NETW_FAULT_SPAWN_REFUSED:
            return {
                vformat("%s was not spawned, because of %s.", v.name, v.owner),
                vformat("Use %s.", v.detail)
            };
        case NETW_FAULT_SPAWN_RETURN:
            return {
                vformat(
                    "A spawn by %s was dropped, because of %s.",
                    v.name,
                    v.owner
                ),
                vformat("Use %s.", v.detail)
            };
        case NETW_FAULT_ENTITY_MOVED:
            return {
                vformat(
                    "'%s' was moved to %s, which the session does not follow.",
                    v.name,
                    v.owner
                ),
                vformat("Use %s.", v.detail)
            };
        case NETW_FAULT_PERSIST_UNDECLARED:
            return {
                vformat("'%s' cannot save, because of %s.", v.name, v.owner),
                vformat("Use %s.", v.detail)
            };
        case NETW_FAULT_SETTING_REFUSED:
            return {
                vformat(
                    "%s cannot be used with %s, so it was refused.",
                    v.name,
                    v.owner
                ),
                vformat("Use %s.", v.detail)
            };
        case NETW_FAULT_LINK_FAILED:
            return {
                vformat(
                    "The link to %s failed, so %s did not happen.",
                    v.detail,
                    v.name
                ),
                "Check the network and the transport, and connect again."
            };
        case NETW_FAULT_STORAGE_FAILED:
            return {
                vformat(
                    "Saving to %s failed, so %s did not happen.",
                    v.name,
                    v.owner
                ),
                vformat("Check %s.", v.detail)
            };
        case NETW_FAULT_HANDLER_UNCALLABLE:
            return {
                vformat(
                    "%s takes the wrong arguments, so it will not run.",
                    v.name
                ),
                vformat("Declare it as %s.", v.detail)
            };
        case NETW_FAULT_NOT_IN_SESSION:
            return {
                vformat(
                    "%s got '%s', which is not inside a session.",
                    v.name,
                    v.owner
                ),
                "Call it from a node in the scene tree under a "
                "{MultiplayerViewport}."
            };
        case NETW_FAULT_SESSION_NOT_FOUND:
            return {
                vformat(
                    "%s found %d running sessions and needs exactly one.",
                    v.name,
                    v.a
                ),
                "Start a session first, or reach the one you mean with "
                "{Netw.of}."
            };
        case NETW_FAULT_BAD_CALLABLE:
            return {
                vformat(
                    "%s got a Callable it cannot call, so it was ignored.",
                    v.name
                ),
                vformat("Pass %s.", v.detail)
            };
        case NETW_FAULT_BAD_SCRIPT:
            return {
                vformat(
                    "%s got %s, whose script it cannot use.",
                    v.name,
                    v.owner
                ),
                vformat("Use %s.", v.detail)
            };
        case NETW_FAULT_BAD_RETURN:
            return {
                vformat(
                    "%s returned %s, so its result was ignored.",
                    v.name,
                    v.owner
                ),
                vformat("Return %s.", v.detail)
            };
        case NETW_FAULT_BUSY_APPLYING:
            return {
                vformat(
                    "%s ran while the session applied its {Netw} settings, so "
                    "it did nothing.",
                    v.name
                ),
                "Call it outside the callbacks that react to those settings."
            };
        case NETW_FAULT_PEER_REFUSED:
            return {
                vformat(
                    "This session cannot use the assigned {MultiplayerPeer}, "
                    "because of %s.",
                    v.owner
                ),
                vformat("Assign %s.", v.detail)
            };
        case NETW_FAULT_NOT_ENTITY:
            return {
                vformat(
                    "%s got '%s', which is not part of a {NetwEntity}.",
                    v.name,
                    v.owner
                ),
                "Pass the entity's root node, or call "
                "{Netw.configure_entity} on it."
            };
        case NETW_FAULT_UNHANDLED_PROMISE:
            return {
                vformat(
                    "A %s failed with error %d (%s) and nothing handled it.",
                    v.name,
                    v.a,
                    v.detail
                ),
                "Attach {NetwPromise.catch_error} or await "
                "{NetwPromise.wait}."
            };
        case NETW_FAULT_AUTHORITY_OVERRIDDEN:
            return {
                vformat(
                    "'%s' changed its multiplayer authority to %d outside its "
                    "entity, and the entity set it back to %d.",
                    v.name,
                    v.a,
                    v.b
                ),
                "Choose who holds it with {NetwEntity.authority}."
            };
        case NETW_FAULT_PREDICTION_OFF:
            return {
                vformat(
                    "Prediction on '%s' is off, because of %s.",
                    v.name,
                    v.owner
                ),
                vformat("Declare %s.", v.detail)
            };
        case NETW_FAULT_PREDICTION_DOWNGRADED:
            return {
                vformat(
                    "'%s' runs a weaker prediction mode than it declares, "
                    "because of %s.",
                    v.name,
                    v.owner
                ),
                vformat("Use %s.", v.detail)
            };
        case NETW_FAULT_REWIND_MISUSE:
            return {
                vformat(
                    "A rewind does not handle '%s' as declared, because of %s.",
                    v.name,
                    v.owner
                ),
                vformat("Use %s.", v.detail)
            };
        case NETW_FAULT_INTERPOLATION_IGNORED:
            return {
                vformat(
                    "'%s' is not interpolated as declared, because of %s.",
                    v.name,
                    v.owner
                ),
                vformat("Use %s.", v.detail)
            };
        case NETW_FAULT_ENGINE_LACKS:
            return {
                vformat(
                    "'%s' needs %s, which this Godot build cannot do, so it "
                    "runs a simpler mode.",
                    v.name,
                    v.detail
                ),
                "Install a {NetwPhysicsStepper} with "
                "{NetwSession.install_physics_stepper}, or use the Networked "
                "engine build."
            };
        case NETW_FAULT_LIMIT_EXCEEDED:
            return {
                vformat(
                    "%s needs %d and the limit is %d, so %s was dropped.",
                    v.name,
                    v.a,
                    v.b,
                    v.owner
                ),
                vformat("Use %s.", v.detail)
            };
        case NETW_FAULT_UNSENDABLE_VALUE:
            return {
                vformat(
                    "%s holds %s, which cannot be sent, so null or nothing was "
                    "sent.",
                    v.name,
                    v.owner
                ),
                vformat("Use %s.", v.detail)
            };
        case NETW_FAULT_UNSUPPORTED:
            return {
                vformat(
                    "%s is not supported for %s, so it was ignored.",
                    v.name,
                    v.owner
                ),
                vformat("Use %s.", v.detail)
            };
        case NETW_FAULT_LIVENESS_TIMEOUT:
            return {
                vformat(
                    "Route %d did not become live within its timeout, so its "
                    "{NetwMultiplayer.liveness_when_live} callback was "
                    "dropped.",
                    v.a
                ),
                "Pass a longer timeout to "
                "{NetwMultiplayer.liveness_when_live}, or wait only on a "
                "route this peer will receive."
            };
    }
    return {};
}

} // namespace netw
