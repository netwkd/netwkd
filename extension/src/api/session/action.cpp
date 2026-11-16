#include "godot/class_db.hpp"
#include "godot/multiplayer.hpp"
#include "godot/node.hpp"
#include "godot/script.hpp"
#include "godot/utility.hpp"
#include "netw/api/action_config.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/predict_slot_engine.hpp"
#include "netw/api/refusal.hpp"
#include "netw/api/replication_core.hpp"
#include "netw/colors.hpp"
#include "netw/decl_sites.hpp"
#include "netw/game_call.hpp"
#include "netw/log.hpp"
#include "netw/profile.hpp"
#include "netw/session/frames.hpp"
#include "netw/session_decl.hpp"
#include "netw/subsystems.hpp"
#include "netw/wire/registry.hpp"

using namespace godot;

namespace netw {

namespace {

constexpr int TIMING_TICK_ALIGNED = 0;
constexpr int TIMING_TICK_ALIGNED_STATE_READY = 1;
constexpr int TIMING_IMMEDIATE = 2;

void confirm_when_returned(const Variant &, const Variant &p_context) {
    NetwActionRequest *context
        = Object::cast_to<NetwActionRequest>(gd::live_object(p_context));
    if (context != nullptr) {
        context->confirm_unanswered();
    }
}

} // namespace

Node *NetwMultiplayer::node_from_tree_path(const NodePath &p_path) const {
    if (!lagcomp_configured) {
        return nullptr;
    }
    Node *anchor = session_get_root();
    return anchor != nullptr ? anchor->get_node_or_null(p_path) : nullptr;
}

bool NetwMultiplayer::can_resolve_action(const PendingAction &p_request) const {
    Node *target = node_from_tree_path(p_request.target_path);
    return action_declared_on(target, p_request.method).is_valid();
}

int NetwMultiplayer::send_lead_ticks() const {
    return clock_engine().get_send_interval() - 1;
}

int NetwMultiplayer::input_readiness(
    const PendingAction &p_request,
    int64_t p_tick
) {
    if (p_tick - p_request.queued_at_tick
        >= input_gate_deadline_ticks + send_lead_ticks()) {
        return ACTION_READY_BY_DEADLINE;
    }
    Node *target = node_from_tree_path(p_request.target_path);
    const Ref<NetwEntity> entity
        = target != nullptr ? NetwEntity::of(target) : Ref<NetwEntity>();
    if (entity.is_null()) {
        return ACTION_NOT_READY;
    }
    NetwPredictSlotEngine *engine
        = predict_engine_for(entity->get_rid_handle());
    if (engine != nullptr
        && !engine->has_consumed_state_tick(p_request.view_tick)) {
        return ACTION_NOT_READY;
    }
    if (!lagcomp_core.timeline_recorded_at(
            lagcomp_core.timeline_slot_of(entity),
            p_request.view_tick
        )) {
        return ACTION_NOT_READY;
    }
    return ACTION_READY;
}

int NetwMultiplayer::action_readiness(
    const PendingAction &p_request,
    int64_t p_tick
) {
    switch (p_request.timing_mode) {
        case TIMING_IMMEDIATE:
            return ACTION_READY;
        case TIMING_TICK_ALIGNED:
            return p_request.view_tick <= p_tick ? ACTION_READY
                                                 : ACTION_NOT_READY;
        case TIMING_TICK_ALIGNED_STATE_READY:
            if (p_request.view_tick > p_tick) {
                return ACTION_NOT_READY;
            }
            return input_readiness(p_request, p_tick);
        default:
            break;
    }
    return ACTION_READY;
}

void NetwMultiplayer::execute_ready_action(
    const PendingAction &p_request,
    int64_t p_tick,
    int p_readiness
) {
    if (p_readiness == ACTION_READY_BY_DEADLINE) {
        gate_fallbacks += 1;
        NETW_TRACE(
            sys::PREDICTION,
            "Action %s waited as long as allowed and now runs on what has "
            "arrived.",
            String(p_request.key).utf8().get_data()
        );
    }
    execute_action(p_request, p_tick);
}

void NetwMultiplayer::execute_action(
    const PendingAction &p_request,
    int64_t p_tick
) {
    const Ref<NetwMultiplayer> keep(this);
    NETW_ZONE_NC("action execute", colors::PREDICTION);
    const Ref<NetwActionConfig> declared = action_declared_on(
        node_from_tree_path(p_request.target_path),
        p_request.method
    );
    if (declared.is_null()) {
        effect_deny_action(p_request.requester, p_request.key);
        return;
    }
    const int64_t clamped
        = p_request.view_tick < p_tick ? p_request.view_tick : p_tick;
    Ref<NetwActionRequest> context;
    context.instantiate();
    context->open(
        this,
        p_request.requester,
        clamped,
        p_request.view_tick,
        p_tick,
        p_request.key
    );
    Array arguments;
    arguments.resize(p_request.args.size() + 1);
    arguments[0] = context;
    for (int at = 0; at < p_request.args.size(); ++at) {
        arguments[at + 1] = p_request.args[at];
    }
    ++action_handler_depth;
    bool called = false;
    const Variant ran = game_call(
        declared->get_authority(),
        arguments,
        game_calls::ACTION_AUTHORITY,
        &called
    );
    --action_handler_depth;
    if (!called) {
        effect_deny_action(p_request.requester, p_request.key);
        return;
    }
    if (context->answered()) {
        return;
    }
    if (!is_coroutine(ran)) {
        effect_confirm_action(p_request.requester, p_request.key);
        return;
    }
    Object *waiting = gd::live_object(ran);
    waiting->connect(
        StringName("completed"),
        callable_mp_static(&confirm_when_returned).bind(context),
        Object::CONNECT_ONE_SHOT
    );
}

Ref<NetwActionConfig> NetwMultiplayer::lagcomp_action_declare(
    Node *p_root,
    const StringName &p_method,
    const Callable &p_handler
) {
    if (p_root == nullptr || p_method.is_empty() || !p_handler.is_valid()) {
        fault::raise(
            nullptr,
            NETW_FAULT_BAD_CALLABLE,
            {String("{NetwMultiplayer.lagcomp_action_declare}"),
             String(),
             String("the entity's root node, a method name and a handler")},
            p_root != nullptr ? uint64_t(gd::instance_id(p_root)) : 0
        );
        return refused<NetwActionConfig>();
    }
    game_call_declared(p_handler, game_calls::ACTION_AUTHORITY);
    decl_sites::record_handler(p_handler);
    Ref<NetwActionConfig> config;
    config.instantiate();
    config->set_authority(p_handler);
    if (session_decl::declare_action(p_root, p_method, config) != OK) {
        return refused<NetwActionConfig>();
    }
    return config;
}

Error NetwMultiplayer::lagcomp_action_request(
    const NodePath &p_target,
    const StringName &p_method,
    int64_t p_view_tick,
    const Array &p_args
) {
    const Error ready = lagcomp_require("the action request");
    if (ready == ERR_BUSY) {
        fault::raise(
            this,
            NETW_FAULT_BUSY_APPLYING,
            {"NetwMultiplayer.lagcomp_action_request"}
        );
        return ERR_BUSY;
    }
    if (ready != OK) {
        return ready;
    }
    const Ref<NetwMultiplayer> keep(this);
    action_send_request(p_target, p_method, p_view_tick, p_args);
    effect_drain();
    return OK;
}

Error NetwMultiplayer::action_ask(
    const Callable &p_method,
    int64_t p_view_tick,
    const Array &p_args
) {
    const Ref<NetwMultiplayer> keep(this);
    Node *target = Object::cast_to<Node>(p_method.get_object());
    const StringName method = p_method.get_method();
    const String shown
        = target != nullptr ? String(target->get_name()) : String(method);
    const uint64_t subject
        = target != nullptr ? uint64_t(gd::instance_id(target)) : 0;
    const Ref<NetwEntity> entity = NetwEntity::of(target);
    if (entity.is_null()) {
        fault::raise(
            this,
            NETW_FAULT_NOT_ENTITY,
            {"{Netw.action}", shown},
            subject
        );
        return ERR_UNCONFIGURED;
    }
    if (entity->get_owner() != target) {
        fault::raise(
            this,
            NETW_FAULT_ACTION_OFF_ROOT,
            {String(), shown},
            subject
        );
        return ERR_UNCONFIGURED;
    }
    const Ref<NetwActionConfig> declared = action_declared_on(target, method);
    if (declared.is_null()) {
        fault::raise(
            this,
            NETW_FAULT_ACTION_NOT_DECLARED,
            {String(method), shown},
            subject
        );
        return ERR_UNCONFIGURED;
    }
    if (!entity->get_is_controlled_locally()) {
        return ERR_UNAUTHORIZED;
    }
    Node *root = session_get_root();
    const NodePath target_path = root != nullptr && target->is_inside_tree()
        ? root->get_path_to(target)
        : NodePath();
    if (target_path.is_empty()) {
        fault::raise(
            this,
            NETW_FAULT_NOT_IN_SESSION,
            {"{Netw.action}", shown},
            subject
        );
        return ERR_UNCONFIGURED;
    }
    const Error ready = lagcomp_require("the action request");
    if (ready == ERR_BUSY) {
        fault::raise(
            this,
            NETW_FAULT_BUSY_APPLYING,
            {"{Netw.action}"},
            subject
        );
        return ERR_BUSY;
    }
    if (ready != OK || !entity->get_is_controlled_locally()) {
        effect_drain();
        return ready != OK ? ready : ERR_UNAUTHORIZED;
    }
    const bool is_remote = has_multiplayer_peer() && !is_host();
    if (!is_remote && liveness_route_of(entity.ptr()) <= 0) {
        liveness_allocate_route(entity.ptr());
    }
    const StringName key
        = lagcomp_action_key(entity->get_rid_handle(), p_view_tick, method);
    const Callable predict = declared->get_predict();
    Node *predicted = predict.is_valid()
        ? Object::cast_to<Node>(
              gd::live_object(
                  game_call(predict, p_args, game_calls::ACTION_PREDICT)
              )
          )
        : nullptr;
    const int64_t predicted_id = predicted != nullptr
        ? int64_t(uint64_t(gd::instance_id(predicted)))
        : 0;
    lagcomp_effect_arm(
        key,
        callable_mp(this, &NetwMultiplayer::action_run_revert)
            .bind(key, declared, predicted_id),
        int(declared->get_timeout_ticks())
    );
    lagcomp_effect_watch(
        key,
        callable_mp(this, &NetwMultiplayer::action_run_confirm)
            .bind(declared, predicted_id),
        Callable()
    );
    action_send_request(target_path, method, p_view_tick, p_args);
    effect_drain();
    return OK;
}

void NetwMultiplayer::action_run_revert(
    const StringName &p_key,
    const Ref<NetwActionConfig> &p_declared,
    int64_t p_predicted
) {
    ActionDenial denial{ERR_TIMEOUT, String()};
    if (const ActionDenial *found = action_denials.getptr(p_key)) {
        denial = *found;
        action_denials.erase(p_key);
    }
    Node *predicted = Object::cast_to<Node>(
        gd::object_of(ObjectID(uint64_t(p_predicted)))
    );
    const Callable revert
        = p_declared.is_valid() ? p_declared->get_revert() : Callable();
    if (revert.is_valid()) {
        Array arguments;
        arguments.push_back(predicted);
        arguments.push_back(int64_t(denial.code));
        arguments.push_back(denial.reason);
        game_call(revert, arguments, game_calls::ACTION_REVERT);
        return;
    }
    if (predicted != nullptr) {
        predicted->queue_free();
    }
}

void NetwMultiplayer::action_run_confirm(
    const Ref<NetwActionConfig> &p_declared,
    int64_t p_predicted
) {
    Node *predicted = Object::cast_to<Node>(
        gd::object_of(ObjectID(uint64_t(p_predicted)))
    );
    const Callable confirm
        = p_declared.is_valid() ? p_declared->get_confirm() : Callable();
    if (confirm.is_valid()) {
        Array arguments;
        arguments.push_back(predicted);
        game_call(confirm, arguments, game_calls::ACTION_CONFIRM);
        return;
    }
    if (predicted != nullptr) {
        predicted->queue_free();
    }
}

void NetwMultiplayer::lagcomp_action_install_from(Node *p_scope) {
    if (!session_decl::actions_on(p_scope).is_empty()) {
        lagcomp_want();
    }
}

void NetwMultiplayer::action_send_request(
    const NodePath &p_target_path,
    const StringName &p_method,
    int64_t p_view_tick,
    const Array &p_args
) {
    const bool is_remote = has_multiplayer_peer() && !is_host();
    const int64_t destination = SERVER_PEER;
    const Ref<NetwEntity> entity
        = NetwEntity::of(node_from_tree_path(p_target_path));
    int64_t route = 0;
    if (entity.is_valid()) {
        route = liveness_route_of(entity.ptr());
        if (route <= 0 && !is_remote) {
            route = liveness_allocate_route(entity.ptr());
        }
    }
    if (route <= 0) {
        Node *target = node_from_tree_path(p_target_path);
        fault::raise(
            this,
            NETW_FAULT_NOT_ENTITY,
            {"{Netw.action}", String(p_target_path)},
            target != nullptr ? uint64_t(gd::instance_id(target)) : 0
        );
    }
    if (!is_remote) {
        submit_action(route, p_method, p_view_tick, p_args, 0);
        return;
    }
    ReplicationCore *plane = (get_replication_plane());
    if (plane == nullptr) {
        NETW_TRACE(
            sys::LAGCOMP,
            "A remote action request was dropped, because replication is not "
            "running."
        );
        return;
    }
    session::ActionRequest body;
    body.method = p_method;
    body.view_tick = p_view_tick;
    body.data = gd::var_to_bytes(p_args);
    NETW_NETWORK_PROFILE(
        wire::ProfileSubjectScope observation(
            profiler_outgoing,
            profiler_member(
                route,
                wire::builtin_channel("ACTION"),
                node_from_tree_path(p_target_path),
                p_method
            )
        )
    );
    plane->send_to(
        destination,
        route,
        wire::builtin_channel("ACTION"),
        session::frame_write(body),
        true,
        0,
        String()
    );
}

void NetwMultiplayer::submit_action(
    int64_t p_route,
    const StringName &p_method,
    int64_t p_view_tick,
    const Array &p_args,
    int64_t p_requester
) {
    NETW_ZONE_NC("action submit", colors::PREDICTION);
    int64_t requester = p_requester;
    if (requester == 0 && lagcomp_configured
        && NETW_API_VIRTUAL(get_multiplayer_peer)().is_valid()) {
        requester = int64_t(NETW_API_VIRTUAL(get_unique_id)());
    }

    const Ref<NetwEntity> entity
        = Object::cast_to<NetwEntity>(wrapper_for_route(p_route).ptr());
    const StringName key = lagcomp_action_key(
        entity.is_valid() ? entity->get_rid_handle() : RID(),
        p_view_tick,
        p_method
    );

    PendingAction request;
    request.method = p_method;
    request.args = p_args;
    request.key = key;
    request.view_tick = p_view_tick;
    request.requester = requester;
    request.queued_at_tick = clock_engine().get_tick();

    Node *anchor = lagcomp_configured ? session_get_root() : nullptr;
    if (anchor != nullptr) {
        Node *owner = entity.is_valid() ? entity->get_owner() : nullptr;
        if (owner != nullptr) {
            request.target_path = anchor->get_path_to(owner);
        }
    }

    Node *target = node_from_tree_path(request.target_path);
    const Ref<NetwActionConfig> declared
        = action_declared_on(target, p_method);
    if (declared.is_null()) {
        if (target != nullptr) {
            fault::raise(
                this,
                NETW_FAULT_ACTION_NOT_DECLARED,
                {String(p_method), String(target->get_name())},
                uint64_t(gd::instance_id(target))
            );
        }
        NETW_TRACE(
            sys::PREDICTION,
            "Action %s names a method this session cannot reach.",
            String(key).utf8().get_data()
        );
        effect_deny_action(requester, key);
        return;
    }
    request.timing_mode = int(declared->get_timing());
    const int64_t now = clock_engine().get_tick();
    const int readiness = action_readiness(request, now);
    if (readiness != ACTION_NOT_READY) {
        execute_ready_action(request, now, readiness);
        return;
    }
    const int64_t latest = now + max_future_action_ticks + send_lead_ticks();
    if (p_view_tick > latest) {
        NETW_TRACE(
            sys::PREDICTION,
            "Action %s asks for tick %d, later than the latest allowed %d.",
            String(key).utf8().get_data(),
            int(p_view_tick),
            int(latest)
        );
        effect_deny_action(requester, key);
        return;
    }
    pending_actions.push_back(request);
}

void NetwMultiplayer::drain_pending_actions(int64_t p_tick) {
    if (pending_actions.is_empty()) {
        return;
    }
    NETW_ZONE_NC("action drain", colors::PREDICTION);
    actions_still_waiting.clear();
    const LocalVector<PendingAction> queued(pending_actions);
    for (uint32_t at = 0; at < queued.size(); ++at) {
        const PendingAction &request = queued[at];
        if (!can_resolve_action(request)) {
            NETW_TRACE(
                sys::PREDICTION,
                "Queued action %s lost the method it names.",
                String(request.key).utf8().get_data()
            );
            effect_deny_action(request.requester, request.key);
            continue;
        }
        const int readiness = action_readiness(request, p_tick);
        if (readiness == ACTION_NOT_READY) {
            actions_still_waiting.push_back(request);
            continue;
        }
        execute_ready_action(request, p_tick, readiness);
    }
    pending_actions = actions_still_waiting;
    actions_still_waiting.clear();
}

int64_t NetwMultiplayer::pending_action_count() const {
    return int64_t(pending_actions.size());
}

int64_t NetwMultiplayer::action_gate_fallbacks() const {
    return gate_fallbacks;
}

} // namespace netw
