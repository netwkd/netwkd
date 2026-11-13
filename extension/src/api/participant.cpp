#include "netw/api/participant.hpp"

#include "godot/callable.hpp"
#include "godot/class_db.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/colors.hpp"
#include "netw/log.hpp"
#include "netw/profile.hpp"

using namespace godot;

namespace netw {

NetwMultiplayer *NetwPlayer::core() const {
    return row_dropped
        ? nullptr
        : Object::cast_to<NetwMultiplayer>(gd::instance_from_id(session_id));
}

int64_t NetwPlayer::get_id() const {
    return core() != nullptr ? incarnation : 0;
}

bool NetwPlayer::belongs_to(const NetwMultiplayer *p_session) const {
    return p_session != nullptr && session_id == gd::instance_id(p_session);
}

bool NetwPlayer::get_is_active() const {
    NetwMultiplayer *held = core();
    return held != nullptr && held->player_is_active(incarnation);
}

String NetwPlayer::get_account() const {
    NetwMultiplayer *held = core();
    return held != nullptr ? held->player_get_account(incarnation) : String();
}

String NetwPlayer::get_account_name() const {
    NetwMultiplayer *held = core();
    return held != nullptr ? held->player_get_account_name(incarnation)
                           : String();
}

int64_t NetwPlayer::get_peer_id() const {
    NetwMultiplayer *held = core();
    return held != nullptr ? held->player_get_peer(incarnation) : 0;
}

StringName NetwPlayer::get_username() const {
    NetwMultiplayer *held = core();
    return held != nullptr ? held->player_get_username(incarnation)
                           : StringName();
}

TypedArray<NetwEntity> NetwPlayer::get_entities() const {
    NETW_ZONE_NC("NetwPlayer entities", colors::SESSION);
    TypedArray<NetwEntity> views;
    NetwMultiplayer *held = core();
    if (held == nullptr) {
        return views;
    }
    const TypedArray<RID> rows = held->player_get_entities(incarnation);
    for (int at = 0; at < rows.size(); ++at) {
        views.push_back(held->wrapper_for_id(int64_t(RID(rows[at]).get_id())));
    }
    return views;
}

Ref<NetwScene> NetwPlayer::get_scene() const {
    NETW_ZONE_NC("NetwPlayer scene", colors::SESSION);
    NetwMultiplayer *held = core();
    return held != nullptr
        ? held->scene_handle_of(held->player_get_scene(incarnation))
        : Ref<NetwScene>();
}

void NetwPlayer::announce_scene_changed(
    const Ref<NetwScene> &p_from,
    const Ref<NetwScene> &p_to
) {
    emit_signal(StringName("scene_changed"), p_from, p_to);
}

void NetwPlayer::_bind_methods() {
    ClassDB::bind_method(D_METHOD("get_id"), &NetwPlayer::get_id);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "id"), "", "get_id");
    ClassDB::bind_method(D_METHOD("get_peer_id"), &NetwPlayer::get_peer_id);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "peer_id"), "", "get_peer_id");
    ClassDB::bind_method(D_METHOD("get_is_active"), &NetwPlayer::get_is_active);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "is_active"), "", "get_is_active");
    ClassDB::bind_method(D_METHOD("get_username"), &NetwPlayer::get_username);
    ADD_PROPERTY(
        PropertyInfo(Variant::STRING_NAME, "username"),
        "",
        "get_username"
    );
    ClassDB::bind_method(D_METHOD("get_account"), &NetwPlayer::get_account);
    ADD_PROPERTY(PropertyInfo(Variant::STRING, "account"), "", "get_account");
    ClassDB::bind_method(
        D_METHOD("get_account_name"),
        &NetwPlayer::get_account_name
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::STRING, "account_name"),
        "",
        "get_account_name"
    );
    ClassDB::bind_method(D_METHOD("get_entities"), &NetwPlayer::get_entities);
    ADD_PROPERTY(
        PropertyInfo(
            Variant::ARRAY,
            "entities",
            PROPERTY_HINT_ARRAY_TYPE,
            "NetwEntity"
        ),
        "",
        "get_entities"
    );
    ClassDB::bind_method(D_METHOD("get_scene"), &NetwPlayer::get_scene);
    ADD_PROPERTY(
        PropertyInfo(
            Variant::OBJECT,
            "scene",
            PROPERTY_HINT_RESOURCE_TYPE,
            "NetwScene",
            PROPERTY_USAGE_NONE
        ),
        "",
        "get_scene"
    );
    ADD_SIGNAL(MethodInfo(
        "scene_changed",
        PropertyInfo(
            Variant::OBJECT,
            "from",
            PROPERTY_HINT_RESOURCE_TYPE,
            "NetwScene"
        ),
        PropertyInfo(
            Variant::OBJECT,
            "to",
            PROPERTY_HINT_RESOURCE_TYPE,
            "NetwScene"
        )
    ));
    BIND_ENUM_CONSTANT(LEAVE_QUIT);
    BIND_ENUM_CONSTANT(LEAVE_STEPPED_AWAY);
    BIND_ENUM_CONSTANT(LEAVE_KICKED);
    BIND_ENUM_CONSTANT(LEAVE_DROPPED);
}

} // namespace netw
