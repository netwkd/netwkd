#include "netw/api/interest_layer.hpp"

#include "godot/callable.hpp"
#include "godot/class_db.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/fault_book.hpp"

using namespace godot;

namespace netw {

namespace {

constexpr const char *SIG_INTEREST_ENTERED = "interest_entered";
constexpr const char *SIG_INTEREST_EXITED = "interest_exited";
constexpr const char *SIG_VISIBILITY_CHANGED = "visibility_changed";
constexpr const char *SIG_VIEWER_ADDED = "viewer_added";
constexpr const char *SIG_VIEWER_REMOVED = "viewer_removed";
constexpr const char *SIG_ENTITY_ADDED = "entity_added";
constexpr const char *SIG_ENTITY_REMOVED = "entity_removed";

PropertyInfo entity_param() {
    return PropertyInfo(
        Variant::OBJECT,
        "entity",
        PROPERTY_HINT_RESOURCE_TYPE,
        "NetwEntity"
    );
}

id::Entity entity_id(const Ref<NetwEntity> &p_entity) {
    return id::Entity(p_entity.is_valid() ? p_entity->get_rid_handle() : RID());
}

} // namespace

Ref<NetwMultiplayer> NetwInterestLayer::host() const {
    return Ref<NetwMultiplayer>(
        Object::cast_to<NetwMultiplayer>(gd::instance_from_id(session))
    );
}

NetwInterestLayer::LayerPolicy NetwInterestLayer::get_policy() const {
    const Ref<NetwMultiplayer> owner = host();
    return owner.is_valid()
        ? LayerPolicy(int(owner->interest_layer_get_param(
              layer,
              NetwMultiplayer::LAYER_PARAM_POLICY
          )))
        : LAYER_POLICY_HIDE_FROM_OUTSIDERS;
}

bool NetwInterestLayer::set_policy(LayerPolicy p_policy) {
    const Ref<NetwMultiplayer> owner = host();
    return owner.is_valid() && owner->layer_set_policy(layer, p_policy);
}

NetwInterestLayer::LeavePolicy NetwInterestLayer::
    get_default_leave_policy() const {
    const Ref<NetwMultiplayer> owner = host();
    return owner.is_valid()
        ? LeavePolicy(int(owner->interest_layer_get_param(
              layer,
              NetwMultiplayer::LAYER_PARAM_LEAVE_POLICY
          )))
        : LEAVE_POLICY_HIDE;
}

void NetwInterestLayer::set_default_leave_policy(LeavePolicy p_policy) {
    const Ref<NetwMultiplayer> owner = host();
    if (owner.is_valid()) {
        owner->layer_set_leave_policy(layer, p_policy);
    }
}

NetwInterestLayer::PerceptionPolicy NetwInterestLayer::
    get_default_perception_policy() const {
    const Ref<NetwMultiplayer> owner = host();
    return owner.is_valid()
        ? PerceptionPolicy(int(owner->interest_layer_get_param(
              layer,
              NetwMultiplayer::LAYER_PARAM_PERCEPTION_POLICY
          )))
        : PERCEPTION_POLICY_HIDE;
}

void NetwInterestLayer::set_default_perception_policy(
    PerceptionPolicy p_policy
) {
    const Ref<NetwMultiplayer> owner = host();
    if (owner.is_valid()) {
        owner->layer_set_perception_policy(layer, p_policy);
    }
}

PackedInt32Array NetwInterestLayer::get_viewers() const {
    const Ref<NetwMultiplayer> owner = host();
    return owner.is_valid() ? owner->interest_layer_get_viewers(layer)
                            : PackedInt32Array();
}

TypedArray<NetwEntity> NetwInterestLayer::get_entities() {
    const Ref<NetwMultiplayer> owner = host();
    return owner.is_valid() ? owner->layer_get_entities(layer)
                            : TypedArray<NetwEntity>();
}

bool NetwInterestLayer::add_viewer(int64_t p_peer_id) {
    const Ref<NetwMultiplayer> owner = host();
    return owner.is_valid() && owner->layer_add_viewer(layer, p_peer_id);
}

bool NetwInterestLayer::remove_viewer(int64_t p_peer_id) {
    const Ref<NetwMultiplayer> owner = host();
    return owner.is_valid() && owner->layer_remove_viewer(layer, p_peer_id);
}

bool NetwInterestLayer::has_viewer(int64_t p_peer_id) const {
    const Ref<NetwMultiplayer> owner = host();
    return owner.is_valid()
        && owner->interest_layer_has_viewer(layer, p_peer_id);
}

bool NetwInterestLayer::add_entity(const Ref<NetwEntity> &p_entity) {
    const Ref<NetwMultiplayer> owner = host();
    return owner.is_valid()
        && owner->layer_add_entity(entity_id(p_entity), id::Layer(layer));
}

bool NetwInterestLayer::remove_entity(const Ref<NetwEntity> &p_entity) {
    const Ref<NetwMultiplayer> owner = host();
    return owner.is_valid()
        && owner->layer_remove_entity(entity_id(p_entity), id::Layer(layer));
}

bool NetwInterestLayer::has_entity(const Ref<NetwEntity> &p_entity) const {
    const Ref<NetwMultiplayer> owner = host();
    return owner.is_valid()
        && owner->layer_has_entity(entity_id(p_entity), id::Layer(layer));
}

bool NetwInterestLayer::is_visible_to(
    const Ref<NetwEntity> &p_entity,
    int64_t p_peer_id
) {
    const Ref<NetwMultiplayer> owner = host();
    return owner.is_valid() && p_entity.is_valid()
        && owner->interest_is_visible(p_entity->get_rid_handle(), p_peer_id);
}

void NetwInterestLayer::set_driver(const Callable &p_driver) {
    const Ref<NetwMultiplayer> owner = host();
    if (owner.is_null()) {
        fault::raise(
            nullptr,
            NETW_FAULT_NOT_IN_SESSION,
            {String("{NetwInterestLayer.set_driver}"), String(layer_id)},
            uint64_t(get_instance_id())
        );
        return;
    }
    owner->layer_set_driver(layer, p_driver, true);
}

Callable NetwInterestLayer::get_driver() {
    const Ref<NetwMultiplayer> owner = host();
    return owner.is_valid() ? owner->layer_get_driver(layer, true)
                            : Callable();
}

int64_t NetwInterestLayer::stat(
    enums::NetwMultiplayer::LayerStat p_stat
) const {
    const Ref<NetwMultiplayer> owner = host();
    return owner.is_valid() ? owner->interest_layer_get_stat(layer, p_stat)
                            : 0;
}

void NetwInterestLayer::_bind_methods() {
    BIND_ENUM_CONSTANT(LAYER_POLICY_HIDE_FROM_OUTSIDERS);
    BIND_ENUM_CONSTANT(LAYER_POLICY_HIDE_FROM_INSIDERS);
    BIND_ENUM_CONSTANT(LEAVE_POLICY_HIDE);
    BIND_ENUM_CONSTANT(LEAVE_POLICY_RETAIN);
    BIND_ENUM_CONSTANT(LEAVE_POLICY_CUSTOM);
    BIND_ENUM_CONSTANT(PERCEPTION_POLICY_HIDE);
    BIND_ENUM_CONSTANT(PERCEPTION_POLICY_SHOW);
    BIND_ENUM_CONSTANT(PERCEPTION_POLICY_CUSTOM);
    ClassDB::bind_method(
        D_METHOD("get_layer_id"),
        &NetwInterestLayer::get_layer_id
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::STRING_NAME, "layer_id"),
        "",
        "get_layer_id"
    );

    ClassDB::bind_method(
        D_METHOD("set_policy", "value"),
        &NetwInterestLayer::set_policy
    );
    ClassDB::bind_method(
        D_METHOD("get_policy"),
        &NetwInterestLayer::get_policy
    );
    ADD_PROPERTY(
        PropertyInfo(
            Variant::INT,
            "policy",
            PROPERTY_HINT_ENUM,
            "Hide From Outsiders,Hide From Insiders",
            PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_CLASS_IS_ENUM,
            "NetwInterestLayer.LayerPolicy"
        ),
        "set_policy",
        "get_policy"
    );

    ClassDB::bind_method(
        D_METHOD("set_default_leave_policy", "value"),
        &NetwInterestLayer::set_default_leave_policy
    );
    ClassDB::bind_method(
        D_METHOD("get_default_leave_policy"),
        &NetwInterestLayer::get_default_leave_policy
    );
    ADD_PROPERTY(
        PropertyInfo(
            Variant::INT,
            "default_leave_policy",
            PROPERTY_HINT_NONE,
            "",
            PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_CLASS_IS_ENUM,
            "NetwInterestLayer.LeavePolicy"
        ),
        "set_default_leave_policy",
        "get_default_leave_policy"
    );

    ClassDB::bind_method(
        D_METHOD("set_default_perception_policy", "value"),
        &NetwInterestLayer::set_default_perception_policy
    );
    ClassDB::bind_method(
        D_METHOD("get_default_perception_policy"),
        &NetwInterestLayer::get_default_perception_policy
    );
    ADD_PROPERTY(
        PropertyInfo(
            Variant::INT,
            "default_perception_policy",
            PROPERTY_HINT_NONE,
            "",
            PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_CLASS_IS_ENUM,
            "NetwInterestLayer.PerceptionPolicy"
        ),
        "set_default_perception_policy",
        "get_default_perception_policy"
    );

    ClassDB::bind_method(
        D_METHOD("get_viewers"),
        &NetwInterestLayer::get_viewers
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::PACKED_INT32_ARRAY, "viewers"),
        "",
        "get_viewers"
    );

    ClassDB::bind_method(
        D_METHOD("get_entities"),
        &NetwInterestLayer::get_entities
    );
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

    ClassDB::bind_method(
        D_METHOD("add_viewer", "peer_id"),
        &NetwInterestLayer::add_viewer
    );
    ClassDB::bind_method(
        D_METHOD("remove_viewer", "peer_id"),
        &NetwInterestLayer::remove_viewer
    );
    ClassDB::bind_method(
        D_METHOD("has_viewer", "peer_id"),
        &NetwInterestLayer::has_viewer
    );
    ClassDB::bind_method(
        D_METHOD("add_entity", "entity"),
        &NetwInterestLayer::add_entity
    );
    ClassDB::bind_method(
        D_METHOD("remove_entity", "entity"),
        &NetwInterestLayer::remove_entity
    );
    ClassDB::bind_method(
        D_METHOD("has_entity", "entity"),
        &NetwInterestLayer::has_entity
    );
    ClassDB::bind_method(
        D_METHOD("is_visible_to", "entity", "peer_id"),
        &NetwInterestLayer::is_visible_to
    );
    ClassDB::bind_method(
        D_METHOD("stat", "stat"),
        &NetwInterestLayer::stat
    );
    ClassDB::bind_method(
        D_METHOD("set_driver", "driver"),
        &NetwInterestLayer::set_driver
    );
    ClassDB::bind_method(
        D_METHOD("get_driver"),
        &NetwInterestLayer::get_driver
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::CALLABLE, "driver"),
        "set_driver",
        "get_driver"
    );

    ADD_SIGNAL(MethodInfo(
        SIG_INTEREST_ENTERED,
        entity_param(),
        PropertyInfo(Variant::INT, "peer_id")
    ));
    ADD_SIGNAL(MethodInfo(
        SIG_INTEREST_EXITED,
        entity_param(),
        PropertyInfo(Variant::INT, "peer_id")
    ));
    ADD_SIGNAL(MethodInfo(
        SIG_VISIBILITY_CHANGED,
        entity_param(),
        PropertyInfo(Variant::BOOL, "visible")
    ));
    ADD_SIGNAL(
        MethodInfo(SIG_VIEWER_ADDED, PropertyInfo(Variant::INT, "peer_id"))
    );
    ADD_SIGNAL(
        MethodInfo(SIG_VIEWER_REMOVED, PropertyInfo(Variant::INT, "peer_id"))
    );
    ADD_SIGNAL(MethodInfo(SIG_ENTITY_ADDED, entity_param()));
    ADD_SIGNAL(MethodInfo(SIG_ENTITY_REMOVED, entity_param()));
}

} // namespace netw
