#include "netw/fault_book.hpp"

using namespace godot;

namespace netw {

namespace {

FaultText prediction_off_text(const FaultValues &v) {
    switch (netw_prediction_off(v.a)) {
        case NETW_OFF_NO_STATE:
            return {
                vformat(
                    "Prediction on '%s' is off, because it has no state "
                    "property.",
                    v.name
                ),
                "Call {NetwPropertyConfig.state} on the properties the "
                "server corrects."
            };
        case NETW_OFF_STATE_LOST:
            return {
                vformat(
                    "Prediction on '%s' is off, because it entered the tree "
                    "again with no state property.",
                    v.name
                ),
                "Keep its {NetwPropertyConfig.state} properties while it is "
                "out of the tree."
            };
        case NETW_OFF_LAGCOMP_FAILED:
            return {
                vformat(
                    "Prediction on '%s' is off, because lag compensation did "
                    "not start when it entered the tree again.",
                    v.name
                ),
                "Fix the error reported before this one."
            };
    }
    return {};
}

FaultText rewind_misuse_text(const FaultValues &v) {
    switch (netw_rewind_misuse(v.a)) {
        case NETW_REWIND_SYNC_TO_PHYSICS:
            return {
                vformat(
                    "A rewind cannot move '%s' for a physics query, because "
                    "it has sync_to_physics on.",
                    v.name
                ),
                "Turn sync_to_physics off and move the node in "
                "_network_process."
            };
        case NETW_REWIND_UNDER_VISUAL_ROOT:
            return {
                vformat(
                    "'%s' is under {NetwDisplay.visual_root}, so a rewind "
                    "leaves it where the body is drawn.",
                    v.name
                ),
                "Move it to a parent outside {NetwDisplay.visual_root}."
            };
        case NETW_REWIND_CHANGED_INSIDE:
            return {
                vformat(
                    "'%s' was changed inside a {Netw.rewind} body, and the "
                    "rewind put it back.",
                    v.name
                ),
                "Change it after {Netw.rewind} returns."
            };
        case NETW_REWIND_AWAITED:
            return {
                vformat(
                    "'%s' awaited inside a {Netw.rewind} body, so the rest of "
                    "it ran after the rewind ended.",
                    v.name
                ),
                "Remove the await from the body."
            };
        case NETW_REWIND_EFFECT_NOT_ARMED:
            return {
                vformat(
                    "{NetwMultiplayer.lagcomp_effect_watch} got '%s', which "
                    "is not an armed effect, so it watches nothing.",
                    v.name
                ),
                "Call {NetwMultiplayer.lagcomp_effect_arm} with that key "
                "first."
            };
    }
    return {};
}

FaultText unknown_name_text(const FaultValues &v) {
    switch (netw_unknown_name(v.b)) {
        case NETW_UNKNOWN_UNDECLARED:
            break;
        case NETW_UNKNOWN_CHILD_NODE:
            return {
                vformat(
                    "'%s' is a child of '%s', not a property.",
                    v.detail,
                    v.owner
                ),
                "Pass the child as the first argument."
            };
        case NETW_UNKNOWN_NO_SCRIPT:
            return {
                vformat(
                    "%s names '%s', and '%s' has no script to declare it.",
                    v.name,
                    v.detail,
                    v.owner
                ),
                vformat(
                    "Attach a script to '%s' and declare it there.",
                    v.owner
                )
            };
        case NETW_UNKNOWN_ENGINE_MEMBER:
            return {
                vformat(
                    "%s names '%s', which the engine declares, and '%s' has "
                    "no script.",
                    v.name,
                    v.detail,
                    v.owner
                ),
                vformat(
                    "Attach a script to '%s' and name something that script "
                    "declares.",
                    v.owner
                )
            };
        case NETW_UNKNOWN_ENGINE_METHOD:
            return {
                vformat(
                    "%s names '%s', which the engine declares and the script "
                    "on '%s' does not.",
                    v.name,
                    v.detail,
                    v.owner
                ),
                "Name a method that script declares."
            };
    }
    return {
        vformat(
            "%s names '%s', which '%s' does not declare.",
            v.name,
            v.detail,
            v.owner
        ),
        vformat("Check the name, or declare it on '%s' first.", v.owner)
    };
}

FaultText interpolation_ignored_text(const FaultValues &v) {
    switch (netw_interpolation_ignored(v.a)) {
        case NETW_INTERPOLATION_TYPE_UNSUPPORTED:
            return {
                vformat(
                    "'%s' is not interpolated, because its {NetwInterpolate} "
                    "cannot blend a %s.",
                    v.name,
                    v.detail
                ),
                "Remove {NetwMemberConfig.interpolate} from it, or give the "
                "property a type the interpolation supports."
            };
        case NETW_INTERPOLATION_SIMULATED_HERE:
            return {
                vformat(
                    "'%s' is not interpolated, because this peer simulates it "
                    "and the interpolation writes the property it reads.",
                    v.name
                ),
                "Set {NetwDisplay.visual_root} or {NetwInterpolate.to}, so it "
                "is drawn on another node or property."
            };
        case NETW_INTERPOLATION_WRITTEN_TWICE:
            return {
                vformat(
                    "'%s' is written by two interpolations, so they overwrite "
                    "each other.",
                    v.name
                ),
                "Keep one interpolation for each property."
            };
        case NETW_INTERPOLATION_VISUAL_ROOT_MISSING:
            return {
                vformat(
                    "'%s' is not interpolated, because "
                    "{NetwDisplay.visual_root} '%s' is not a node under '%s'.",
                    v.name,
                    v.detail,
                    v.owner
                ),
                "Set {NetwDisplay.visual_root} to the path of a child node."
            };
        case NETW_INTERPOLATION_WRITES_BODY:
            return {
                vformat(
                    "'%s' is not interpolated, because it would write a body "
                    "this entity simulates.",
                    v.name
                ),
                "Set {NetwDisplay.visual_root} to draw it on a child node."
            };
        case NETW_INTERPOLATION_LOCAL_SPACE:
            return {
                vformat(
                    "'%s' is written to {NetwDisplay.visual_root} in local "
                    "space, so the body still moves it.",
                    v.name
                ),
                "Turn on top_level on that node, or interpolate position and "
                "rotation."
            };
        case NETW_INTERPOLATION_NO_GLOBAL_SETTER:
            return {
                vformat(
                    "'%s' is written to {NetwDisplay.visual_root} in local "
                    "space, because that node cannot take a %s in global "
                    "space.",
                    v.name,
                    v.detail
                ),
                "Use a Node2D or a Node3D as {NetwDisplay.visual_root}."
            };
        case NETW_INTERPOLATION_NAME_SHARED:
            return {
                vformat(
                    "Two interpolations on one entity both write a property "
                    "named '%s', so a call that names it reaches only one.",
                    v.name
                ),
                "Interpolate one property of that name on each entity."
            };
        case NETW_INTERPOLATION_TYPE_CHANGED:
            return {
                vformat(
                    "'%s' changed its type, so the new value was not "
                    "interpolated.",
                    v.name
                ),
                "Give the property a static type."
            };
    }
    return {};
}

} // namespace

FaultText bug_text(const String &p_where) {
    return {
        vformat(
            "netwkd stopped at %s because of a bug in netwkd, not in "
            "this game.",
            p_where
        ),
        "Report it to netwkd with this log."
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
                "Answer sooner."
            };
        case NETW_FAULT_MALFORMED_DATAGRAM:
            return {
                vformat(
                    "Peer %d sent a packet this peer could not use, so it "
                    "was dropped.",
                    v.peer
                ),
                "Run the same netwkd version on every peer."
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
                    "Handlers the session called kept giving it more work "
                    "for %d passes, so it finishes the rest on its next "
                    "update.",
                    v.a
                ),
                "Defer that work in such a handler with {Node.queue_free} "
                "or {Object.call_deferred}."
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
                    "Call {NetwJoinRequest.admit} for peer 1 in the "
                    "{Netw.configure_admission} handler. The join was refused "
                    "with %s.",
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
                !v.detail.is_empty() ? vformat("Call it from %s.", v.detail)
                    : v.a != 0
                    ? vformat(
                          "Mark '%s' with @rpc(\"any_peer\") if peer %d "
                          "should call it.",
                          v.name,
                          v.peer
                      )
                    : vformat(
                          "Call {NetwMemberConfig.any_peer} or "
                          "{NetwMemberConfig.controller} on '%s' if peer %d "
                          "should call it.",
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
                "the same netwkd version on every peer."
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
            return unknown_name_text(v);
        case NETW_FAULT_DECLARED_TWICE:
            return {
                vformat(
                    "%s was declared twice on '%s', so the second was "
                    "refused.",
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
            if (v.b == NETW_MOVED_DECLARATION) {
                return {
                    vformat(
                        "'%s' declared %s and was moved to another session, "
                        "so this session ignores it.",
                        v.name,
                        v.owner
                    ),
                    "Declare it on a node that stays in this session."
                };
            }
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
        case NETW_FAULT_REQUEST_AS_VALUE:
            return {
                vformat(
                    "'%s' is a request the AnimationTree clears when it "
                    "reads it, so it cannot be sent as a value.",
                    v.name
                ),
                "Send it with {Netw.configure_rpc}."
            };
        case NETW_FAULT_PLAYER_SPAWN_FAILED:
            return {
                vformat(
                    "The server could not spawn a node for this player with "
                    "'%s'.",
                    v.name
                ),
                "Read the server's log for the reason."
            };
        case NETW_FAULT_LOCAL_WRITE_REPLACED:
            return {
                vformat(
                    "'%s' on '%s' was changed on this peer, which does not "
                    "control it.",
                    v.name,
                    v.owner
                ),
                vformat("The value from peer %d replaced it.", v.peer)
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
                    "%s failed, so %s did not happen.",
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
                    "%s got a Callable it cannot use, so it was ignored.",
                    v.name
                ),
                vformat("Pass %s.", v.detail)
            };
        case NETW_FAULT_BAD_SCRIPT:
            return {
                vformat(
                    "%s got %s, which it cannot use.",
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
            return prediction_off_text(v);
        case NETW_FAULT_PREDICTION_DOWNGRADED:
            return {
                vformat(
                    "The physics server moves '%s' at "
                    "{NetwSimulation.SCHEDULE_FRAME}, which cannot replay, so "
                    "{NetwPrediction.MODE_ROLLBACK} runs as "
                    "{NetwPrediction.MODE_OFFSET}.",
                    v.name
                ),
                "Set {NetwSimulation.schedule} to "
                "{NetwSimulation.SCHEDULE_STEPPED}, or {NetwPrediction.mode} "
                "to {NetwPrediction.MODE_OFFSET}."
            };
        case NETW_FAULT_REWIND_MISUSE:
            return rewind_misuse_text(v);
        case NETW_FAULT_INTERPOLATION_IGNORED:
            return interpolation_ignored_text(v);
        case NETW_FAULT_ENGINE_LACKS:
            return {
                v.a == NETW_LACKS_REWIND_MOVE
                    ? vformat(
                          "This Godot build cannot move the %s '%s' during a "
                          "rewind, so a physics query in {Netw.rewind} finds "
                          "it where it is now.",
                          v.detail,
                          v.name
                      )
                    : vformat(
                          "This Godot build cannot restore the %s '%s' in a "
                          "replay, so its entity runs at "
                          "{NetwSimulation.SCHEDULE_FRAME}.",
                          v.detail,
                          v.name
                      ),
                "Use the netwkd build of Godot, which adds "
                "flush_kinematic_transform."
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
                    "waiting packet was dropped.",
                    v.a
                ),
                "Make sure the server sends the entity at that route to this "
                "peer."
            };
        case NETW_FAULT_NO_FREE_PORT:
            return {
                vformat(
                    "{LANLobbyDirectory} found no free port from %d to %d, so "
                    "this game is not listed on the LAN.",
                    v.a,
                    v.b
                ),
                "Set {LANLobbyDirectory.port} or "
                "{LANLobbyDirectory.port_count} to a free range."
            };
        case NETW_FAULT_NO_CRYPTO:
            return {
                vformat(
                    "%s needs Godot's cryptography support, which this Godot "
                    "build lacks, so it did nothing.",
                    v.name
                ),
                "Use a Godot build with cryptography support."
            };
        case NETW_FAULT_PART_TOO_LATE:
            return {
                vformat(
                    "'%s' was declared inside '%s' after '%s' started, so it "
                    "is its own entity.",
                    v.owner,
                    v.detail,
                    v.detail
                ),
                vformat(
                    "Declare it before '%s' enters the tree, or save it as "
                    "its own scene.",
                    v.detail
                )
            };
        case NETW_FAULT_PART_CONFLICT:
            return {
                vformat(
                    "'%s' and '%s' set %s to different values, so '%s' keeps "
                    "its own.",
                    v.owner,
                    v.detail,
                    v.name,
                    v.detail
                ),
                vformat("Set %s on one of them.", v.name)
            };
        case NETW_FAULT_PART_ONLY:
            return {
                vformat(
                    "'%s' is part of '%s', so the %s it set was ignored.",
                    v.owner,
                    v.detail,
                    v.name
                ),
                vformat(
                    "Set it on '%s', or save '%s' as its own scene.",
                    v.detail,
                    v.owner
                )
            };
        case NETW_FAULT_RESOURCE_MISSING:
            return {
                vformat(
                    "'%s' syncs '%s', but '%s' holds no resource, so the "
                    "value was skipped.",
                    v.owner,
                    v.name,
                    v.detail
                ),
                vformat(
                    "Assign a resource to '%s' before '%s' enters the tree, "
                    "and keep one there.",
                    v.detail,
                    v.owner
                )
            };
        case NETW_FAULT_RESOURCE_SHARED:
            return {
                vformat(
                    "'%s' and '%s' sync '%s' from the same resource, so each "
                    "overwrites the other.",
                    v.owner,
                    v.detail,
                    v.name
                ),
                "Turn on {Resource.resource_local_to_scene} for that "
                "resource, so each entity gets its own copy."
            };
        case NETW_FAULT_CHANGED_IN_STEP:
            return {
                "A predicted entity was added, removed or changed while a "
                "_network_process ran, so the change was dropped.",
                "Make the change after the step, such as with "
                "{Object.call_deferred}."
            };
        case NETW_FAULT_ACTION_UNREADY:
            return {
                vformat(
                    "Peer %d requested an action on '%s', but lag "
                    "compensation is off on the server, so the request was "
                    "dropped.",
                    v.peer,
                    v.name
                ),
                "Call {Netw.configure_lagcomp} on the server before the "
                "session starts."
            };
        case NETW_FAULT_FRAME_TOO_LONG:
            return {
                vformat(
                    "A frame took %d ms, which is over "
                    "{NetwClockConfig.stall_threshold} at %d ms, so the ticks "
                    "it missed were skipped.",
                    v.a,
                    v.b
                ),
                "Raise {NetwClockConfig.stall_threshold} if frames this long "
                "are expected."
            };
        case NETW_FAULT_CANNOT_STEP_PHYSICS:
            return {
                vformat(
                    "'%s' sets {NetwSimulation.SCHEDULE_STEPPED}, and this "
                    "Godot build cannot step physics on demand, so it runs at "
                    "{NetwSimulation.SCHEDULE_FRAME}.",
                    v.name
                ),
                "Install a {NetwPhysicsStepper} with "
                "{NetwSession.install_physics_stepper}, or use the netwkd "
                "build of Godot."
            };
        case NETW_FAULT_TOGETHER_REFUSED:
            return {
                vformat(
                    "'%s' sets {NetwPrediction.rollback_together}, but %s "
                    "cannot replay a tick, so each entity is corrected alone.",
                    v.name,
                    v.detail
                ),
                "Give each of them a _network_process at "
                "{NetwSimulation.SCHEDULE_TICK}, or use "
                "{NetwSimulation.SCHEDULE_STEPPED} on a Godot build that can "
                "step physics."
            };
        case NETW_FAULT_COLLISION_UNSIMULATED:
            return {
                vformat(
                    "'%s' called {NetwPrediction.notify_collision}, but it "
                    "simulates no other entity on this peer.",
                    v.name
                ),
                "If it hit an entity, add that entity to one of its "
                "{NetwInterestLayer} layers or set its "
                "{NetwSimulation.remote_mode} to "
                "{NetwSimulation.REMOTE_MODE_ACTIVE}."
            };
        case NETW_FAULT_BROADCAST_UNCORRECTED:
            return {
                vformat(
                    "'%s' is predicted, and the server does not correct its "
                    "{NetwPropertyConfig.broadcast} properties [%s].",
                    v.name,
                    v.detail
                ),
                "Use {NetwPropertyConfig.state} on any of them that changes "
                "how it moves."
            };
        case NETW_FAULT_INPUT_UNTYPED:
            return {
                vformat(
                    "'%s' has {NetwPropertyConfig.input} properties with no "
                    "type, so its input cannot be sent for prediction.",
                    v.name
                ),
                "Give each one a static type, or {NetwMemberConfig.quantize}."
            };
        case NETW_FAULT_STEPPED_UNPREDICTED:
            return {
                vformat(
                    "'%s' sets {NetwSimulation.SCHEDULE_STEPPED} but is not "
                    "predicted, so it runs at {NetwSimulation.SCHEDULE_FRAME}.",
                    v.name
                ),
                "Give it a {NetwPropertyConfig.state} property so it is "
                "predicted, or set another {NetwSimulation.schedule}."
            };
        case NETW_FAULT_INPUT_UNPREDICTED:
            return {
                vformat(
                    "'%s' has {NetwPropertyConfig.state} and "
                    "{NetwPropertyConfig.input} properties but is not "
                    "predicted.",
                    v.name
                ),
                "Move the code that reads its input into "
                "_network_process(delta, tick, is_fresh)."
            };
        case NETW_FAULT_THRESHOLD_SHARED:
            return {
                v.a == NETW_SHARED_TELEPORT
                    ? vformat(
                          "'%s' sets no "
                          "{NetwPropertyConfig.teleport_threshold} on [%s], "
                          "so they share {NetwPrediction.teleport_threshold}.",
                          v.owner,
                          v.name
                      )
                    : vformat(
                          "'%s' sets no "
                          "{NetwPropertyConfig.correction_threshold} on [%s], "
                          "so each uses "
                          "{NetwPrediction.correction_threshold}.",
                          v.owner,
                          v.name
                      ),
                "Set it on each one in that property's own units."
            };
        case NETW_FAULT_ACTION_OFF_ROOT:
            return {
                vformat(
                    "{Netw.action} got a method of '%s', which is not the "
                    "root node of a {NetwEntity}.",
                    v.owner
                ),
                "Declare the action and call {Netw.action} in the script on "
                "the entity's root node."
            };
        case NETW_FAULT_ACTION_NOT_DECLARED:
            return {
                vformat(
                    "{Netw.action} got '%s', which is not a declared action.",
                    v.name
                ),
                "Declare it with {Netw.configure_action} in _init."
            };
        case NETW_FAULT_JOIN_UNANSWERED:
            return {
                "The {Netw.configure_admission} handler finished without an "
                "answer, so the join was refused.",
                "Call {NetwJoinRequest.admit} to let the player join, or "
                "{NetwRequest.deny} to refuse."
            };
        case NETW_FAULT_HANDLER_AWAITED:
            return {
                vformat(
                    "%s awaited, so the request was judged before it "
                    "finished.",
                    v.name
                ),
                "Call {NetwRequest.deny} before the first await, or do not "
                "await in this handler."
            };
        case NETW_FAULT_VALUE_TYPE:
            return {
                vformat(
                    "Peer %d sent %s a value of another type, so the row was "
                    "refused.",
                    v.peer,
                    v.name
                ),
                "Give the property the same type on every peer."
            };
        case NETW_FAULT_QUANTIZER_CHANGED:
            return {
                vformat(
                    "%s was changed after a property was declared with it, "
                    "so peers can read that property wrong.",
                    v.name
                ),
                "Set the quantizer's settings before you declare the "
                "property."
            };
        case NETW_FAULT_VALUE_UNREADABLE:
            return {
                vformat(
                    "Peer %d sent %s a value this peer could not read, so "
                    "null was used.",
                    v.peer,
                    v.name
                ),
                "Run the same netwkd version on every peer."
            };
    }
    return {};
}

} // namespace netw
