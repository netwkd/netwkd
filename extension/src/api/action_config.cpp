#include "netw/api/action_config.hpp"

#include "godot/class_db.hpp"
#include "netw/game_call.hpp"
#include "netw/session_decl.hpp"

using namespace godot;

namespace netw {

void NetwActionConfig::_bind_methods() {
    ClassDB::bind_method(
        D_METHOD("predict", "handler"),
        &NetwActionConfig::predict
    );
    ClassDB::bind_method(
        D_METHOD("confirm", "handler"),
        &NetwActionConfig::confirm
    );
    ClassDB::bind_method(
        D_METHOD("revert", "handler"),
        &NetwActionConfig::revert
    );
    ClassDB::bind_method(D_METHOD("timing", "mode"), &NetwActionConfig::timing);
    ClassDB::bind_method(
        D_METHOD("timeout_ticks", "ticks"),
        &NetwActionConfig::timeout_ticks
    );

    BIND_ENUM_CONSTANT(TIMING_TICK_ALIGNED);
    BIND_ENUM_CONSTANT(TIMING_TICK_ALIGNED_STATE_READY);
    BIND_ENUM_CONSTANT(TIMING_IMMEDIATE);
}

Ref<NetwActionConfig> NetwActionConfig::predict(const Callable &p_handler) {
    game_call_declared(p_handler, game_calls::ACTION_PREDICT);
    predict_handler = p_handler;
    return Ref<NetwActionConfig>(this);
}

Ref<NetwActionConfig> NetwActionConfig::confirm(const Callable &p_handler) {
    game_call_declared(p_handler, game_calls::ACTION_CONFIRM);
    confirm_handler = p_handler;
    return Ref<NetwActionConfig>(this);
}

Ref<NetwActionConfig> NetwActionConfig::revert(const Callable &p_handler) {
    game_call_declared(p_handler, game_calls::ACTION_REVERT);
    revert_handler = p_handler;
    return Ref<NetwActionConfig>(this);
}

Ref<NetwActionConfig> NetwActionConfig::timing(Timing p_mode) {
    timing_mode = p_mode;
    return Ref<NetwActionConfig>(this);
}

Ref<NetwActionConfig> NetwActionConfig::timeout_ticks(int64_t p_ticks) {
    timeout = p_ticks;
    return Ref<NetwActionConfig>(this);
}

Ref<NetwActionConfig> action_declared_on(
    Node *p_node,
    const StringName &p_method
) {
    const Dictionary declared = session_decl::actions_on(p_node);
    if (!declared.has(p_method)) {
        return Ref<NetwActionConfig>();
    }
    return declared[p_method];
}

} // namespace netw
