#include "netw/api/lifecycle_request.hpp"

#include "godot/callable.hpp"
#include "godot/class_db.hpp"

namespace netw {

using namespace godot;

Ref<NetwLifecycleRequest> NetwLifecycleRequest::create(
    lifecycle::Kind p_kind,
    int64_t p_requester,
    const Ref<NetwEntity> &p_entity,
    Node *p_destination
) {
    Ref<NetwLifecycleRequest> made;
    made.instantiate();
    made->kind = Kind(int(p_kind));
    made->peer_id = p_requester;
    made->entity = p_entity;
    made->destination = p_destination != nullptr
        ? gd::instance_id(p_destination)
        : ObjectID();
    return made;
}

Node *NetwLifecycleRequest::get_destination() const {
    return Object::cast_to<Node>(gd::object_of(destination));
}

void NetwLifecycleRequest::_bind_methods() {
    ClassDB::bind_method(D_METHOD("get_kind"), &NetwLifecycleRequest::get_kind);
    ADD_PROPERTY(
        PropertyInfo(
            Variant::INT,
            "kind",
            PROPERTY_HINT_ENUM,
            "Spawn,Despawn,Reparent",
            PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_CLASS_IS_ENUM,
            "NetwLifecycleRequest.Kind"
        ),
        String(),
        "get_kind"
    );
    ClassDB::bind_method(
        D_METHOD("get_entity"),
        &NetwLifecycleRequest::get_entity
    );
    ADD_PROPERTY(
        PropertyInfo(
            Variant::OBJECT,
            "entity",
            PROPERTY_HINT_RESOURCE_TYPE,
            "NetwEntity"
        ),
        String(),
        "get_entity"
    );
    ClassDB::bind_method(
        D_METHOD("get_destination"),
        &NetwLifecycleRequest::get_destination
    );
    ADD_PROPERTY(
        PropertyInfo(
            Variant::OBJECT,
            "destination",
            PROPERTY_HINT_NODE_TYPE,
            "Node",
            PROPERTY_USAGE_DEFAULT,
            "Node"
        ),
        String(),
        "get_destination"
    );
    BIND_ENUM_CONSTANT(KIND_SPAWN);
    BIND_ENUM_CONSTANT(KIND_DESPAWN);
    BIND_ENUM_CONSTANT(KIND_REPARENT);
}

} // namespace netw
