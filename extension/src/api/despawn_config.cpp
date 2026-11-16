#include "netw/api/despawn_config.hpp"

#include "godot/class_db.hpp"
#include "netw/fault_book.hpp"
#include "netw/game_call.hpp"

using namespace godot;

namespace netw {

Ref<NetwDespawnConfig> NetwDespawnConfig::before_removal(
    const Callable &p_callable
) {
    if (refused) {
        return Ref<NetwDespawnConfig>(this);
    }
    if (game_call_declared(p_callable, game_calls::DESPAWN_HOOK)
        || p_callable.is_valid()) {
        hook_method = p_callable.get_method();
    }
    return Ref<NetwDespawnConfig>(this);
}

Ref<NetwDespawnConfig> NetwDespawnConfig::linger(double p_seconds) {
    if (refused) {
        return Ref<NetwDespawnConfig>(this);
    }
    if (!(p_seconds >= 0.0)) {
        fault::raise(
            nullptr,
            NETW_FAULT_BAD_VALUE,
            {String("{NetwDespawnConfig.linger}"),
             vformat("%s seconds", String::num(p_seconds)),
             String("a number of seconds from zero")}
        );
        return Ref<NetwDespawnConfig>(this);
    }
    linger_seconds = p_seconds;
    return Ref<NetwDespawnConfig>(this);
}

void NetwDespawnConfig::_bind_methods() {
    ClassDB::bind_method(
        D_METHOD("before_removal", "method"),
        &NetwDespawnConfig::before_removal
    );
    ClassDB::bind_method(
        D_METHOD("linger", "seconds"),
        &NetwDespawnConfig::linger
    );

#define NETW_DESPAWN_CONFIG_PROPERTY(m_type, m_name) \
    ClassDB::bind_method( \
        D_METHOD("set_" #m_name, #m_name), \
        &NetwDespawnConfig::set_##m_name \
    ); \
    ClassDB::bind_method( \
        D_METHOD("get_" #m_name), \
        &NetwDespawnConfig::get_##m_name \
    ); \
    ADD_PROPERTY(PropertyInfo(m_type, #m_name), "set_" #m_name, "get_" #m_name)

    NETW_DESPAWN_CONFIG_PROPERTY(Variant::STRING_NAME, hook_method);
    NETW_DESPAWN_CONFIG_PROPERTY(Variant::FLOAT, linger_seconds);

#undef NETW_DESPAWN_CONFIG_PROPERTY
}

} // namespace netw
