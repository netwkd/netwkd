#include "netw/api/scene.hpp"

#include "godot/callable.hpp"
#include "godot/class_db.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/entity_options.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/participant.hpp"
#include "netw/scene_core.hpp"

using namespace godot;

namespace netw {

namespace {

const char *SNAME_ENTITY_ENTERED = "entity_entered";
const char *SNAME_ENTITY_EXITED = "entity_exited";
const char *SNAME_PLAYER_ENTITY_ENTERED = "player_entity_entered";
const char *SNAME_PLAYER_ENTITY_LEFT = "player_entity_left";
const char *SNAME_VIEWER_ENTERED = "viewer_entered";
const char *SNAME_VIEWER_EXITED = "viewer_exited";

TypedArray<NetwEntity> views_of(
    NetwMultiplayer *p_session,
    const TypedArray<RID> &p_rows
) {
    TypedArray<NetwEntity> views;
    if (p_session == nullptr) {
        return views;
    }
    views.resize(p_rows.size());
    for (int at = 0; at < p_rows.size(); ++at) {
        views[at] = p_session->entity_get_view(RID(p_rows[at]));
    }
    return views;
}

} // namespace

NetwMultiplayer *NetwScene::core() const {
    return Object::cast_to<NetwMultiplayer>(gd::instance_from_id(session_id));
}

RID NetwScene::get_rid() const {
    return core() != nullptr ? scene : RID();
}

NetwMultiplayer *NetwScene::live_core() const {
    NetwMultiplayer *session = core();
    return session != nullptr && session->scene_is_live(scene) ? session
                                                               : nullptr;
}

Ref<NetwEntity> NetwScene::get_entity() const {
    NetwMultiplayer *session = live_core();
    return session != nullptr ? session->entity_get_view(scene)
                              : Ref<NetwEntity>();
}

Node *NetwScene::get_root() const {
    NetwMultiplayer *session = live_core();
    return session != nullptr ? session->scene_get_root(scene) : nullptr;
}

Node *NetwScene::get_world() const {
    NetwMultiplayer *session = live_core();
    return session != nullptr ? session->scene_get_world(scene) : nullptr;
}

StringName NetwScene::get_label() const {
    NetwMultiplayer *session = core();
    return session != nullptr ? session->scene_get_label(scene) : StringName();
}

TypedArray<NetwEntity> NetwScene::get_player_entities() const {
    NetwMultiplayer *session = core();
    return session != nullptr
        ? views_of(session, session->scene_get_player_entities(scene))
        : TypedArray<NetwEntity>();
}

TypedArray<NetwPlayer> NetwScene::get_viewers() const {
    TypedArray<NetwPlayer> out;
    NetwMultiplayer *session = core();
    if (session == nullptr) {
        return out;
    }
    const PackedInt64Array ids = session->scene_get_viewers(scene);
    for (int at = 0; at < ids.size(); ++at) {
        out.push_back(session->player_view(ids[at]));
    }
    return out;
}

TypedArray<NetwEntity> NetwScene::get_local_player_entities() const {
    NetwMultiplayer *session = core();
    return session != nullptr
        ? views_of(session, session->scene_get_local_player_entities(scene))
        : TypedArray<NetwEntity>();
}

TypedArray<NetwEntity> NetwScene::get_entities() const {
    NetwMultiplayer *session = core();
    return session != nullptr
        ? views_of(session, session->scene_get_entities(scene))
        : TypedArray<NetwEntity>();
}

Error NetwScene::watch(const Ref<NetwPlayer> &p_player) {
    NetwMultiplayer *session = core();
    if (session == nullptr || !session->player_holds(p_player)) {
        return ERR_INVALID_PARAMETER;
    }
    return session->scene_watch(scene, p_player->get_id());
}

Error NetwScene::unwatch(const Ref<NetwPlayer> &p_player) {
    NetwMultiplayer *session = core();
    if (session == nullptr || !session->player_holds(p_player)) {
        return ERR_INVALID_PARAMETER;
    }
    return session->scene_unwatch(scene, p_player->get_id());
}

bool NetwScene::is_watching(const Ref<NetwPlayer> &p_player) const {
    NetwMultiplayer *session = core();
    return session != nullptr && session->player_holds(p_player)
        && session->scene_is_watching(scene, p_player->get_id());
}

void NetwScene::announce_entity(
    const Ref<NetwEntity> &p_entity,
    bool p_present
) {
    emit_signal(
        p_present ? SNAME_ENTITY_ENTERED : SNAME_ENTITY_EXITED,
        p_entity
    );
}

void NetwScene::announce_player_entity(
    const Ref<NetwEntity> &p_entity,
    bool p_present
) {
    emit_signal(
        p_present ? SNAME_PLAYER_ENTITY_ENTERED : SNAME_PLAYER_ENTITY_LEFT,
        p_entity
    );
}

void NetwScene::announce_viewer(
    const Ref<NetwPlayer> &p_player,
    bool p_present
) {
    emit_signal(
        p_present ? SNAME_VIEWER_ENTERED : SNAME_VIEWER_EXITED,
        p_player
    );
}

void NetwScene::_bind_methods() {
    BIND_ENUM_CONSTANT(SCENE_ISOLATION_NONE);
    BIND_ENUM_CONSTANT(SCENE_ISOLATION_OWN_WORLD);
    ClassDB::bind_method(D_METHOD("get_rid"), &NetwScene::get_rid);
    ADD_PROPERTY(PropertyInfo(Variant::RID, "rid"), "", "get_rid");
    ClassDB::bind_method(D_METHOD("get_entity"), &NetwScene::get_entity);
    ADD_PROPERTY(
        PropertyInfo(
            Variant::OBJECT,
            "entity",
            PROPERTY_HINT_RESOURCE_TYPE,
            "NetwEntity"
        ),
        "",
        "get_entity"
    );
    ClassDB::bind_method(D_METHOD("get_root"), &NetwScene::get_root);
    ADD_PROPERTY(
        PropertyInfo(
            Variant::OBJECT,
            "root",
            PROPERTY_HINT_NODE_TYPE,
            "Node",
            PROPERTY_USAGE_DEFAULT,
            "Node"
        ),
        "",
        "get_root"
    );
    ClassDB::bind_method(D_METHOD("get_world"), &NetwScene::get_world);
    ADD_PROPERTY(
        PropertyInfo(
            Variant::OBJECT,
            "world",
            PROPERTY_HINT_NODE_TYPE,
            "SubViewport",
            PROPERTY_USAGE_DEFAULT,
            "SubViewport"
        ),
        "",
        "get_world"
    );
    ClassDB::bind_method(D_METHOD("get_label"), &NetwScene::get_label);
    ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "label"), "", "get_label");
    ClassDB::bind_method(
        D_METHOD("get_player_entities"),
        &NetwScene::get_player_entities
    );
    ADD_PROPERTY(
        PropertyInfo(
            Variant::ARRAY,
            "player_entities",
            PROPERTY_HINT_ARRAY_TYPE,
            "NetwEntity"
        ),
        "",
        "get_player_entities"
    );
    ClassDB::bind_method(D_METHOD("get_viewers"), &NetwScene::get_viewers);
    ADD_PROPERTY(
        PropertyInfo(
            Variant::ARRAY,
            "viewers",
            PROPERTY_HINT_ARRAY_TYPE,
            "NetwPlayer"
        ),
        "",
        "get_viewers"
    );
    ClassDB::bind_method(
        D_METHOD("get_local_player_entities"),
        &NetwScene::get_local_player_entities
    );
    ADD_PROPERTY(
        PropertyInfo(
            Variant::ARRAY,
            "local_player_entities",
            PROPERTY_HINT_ARRAY_TYPE,
            "NetwEntity"
        ),
        "",
        "get_local_player_entities"
    );
    ClassDB::bind_method(D_METHOD("get_entities"), &NetwScene::get_entities);
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
    ClassDB::bind_method(D_METHOD("watch", "player"), &NetwScene::watch);
    ClassDB::bind_method(D_METHOD("unwatch", "player"), &NetwScene::unwatch);
    ClassDB::bind_method(
        D_METHOD("is_watching", "player"),
        &NetwScene::is_watching
    );

    ADD_SIGNAL(MethodInfo(
        SNAME_ENTITY_ENTERED,
        PropertyInfo(
            Variant::OBJECT,
            "entity",
            PROPERTY_HINT_RESOURCE_TYPE,
            "NetwEntity"
        )
    ));
    ADD_SIGNAL(MethodInfo(
        SNAME_ENTITY_EXITED,
        PropertyInfo(
            Variant::OBJECT,
            "entity",
            PROPERTY_HINT_RESOURCE_TYPE,
            "NetwEntity"
        )
    ));
    ADD_SIGNAL(MethodInfo(
        SNAME_PLAYER_ENTITY_ENTERED,
        PropertyInfo(
            Variant::OBJECT,
            "entity",
            PROPERTY_HINT_RESOURCE_TYPE,
            "NetwEntity"
        )
    ));
    ADD_SIGNAL(MethodInfo(
        SNAME_PLAYER_ENTITY_LEFT,
        PropertyInfo(
            Variant::OBJECT,
            "entity",
            PROPERTY_HINT_RESOURCE_TYPE,
            "NetwEntity"
        )
    ));
    ADD_SIGNAL(MethodInfo(
        SNAME_VIEWER_ENTERED,
        PropertyInfo(
            Variant::OBJECT,
            "player",
            PROPERTY_HINT_RESOURCE_TYPE,
            "NetwPlayer"
        )
    ));
    ADD_SIGNAL(MethodInfo(
        SNAME_VIEWER_EXITED,
        PropertyInfo(
            Variant::OBJECT,
            "player",
            PROPERTY_HINT_RESOURCE_TYPE,
            "NetwPlayer"
        )
    ));
}

} // namespace netw
