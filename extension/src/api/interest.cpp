#include "netw/api/interest.hpp"

#include "godot/callable.hpp"
#include "godot/class_db.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/interest_layer.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/fault_book.hpp"

using namespace godot;

namespace netw {

void NetwInterest::bind(NetwEntity *p_entity) {
    if (p_entity == nullptr || entity_id == gd::instance_id(p_entity)) {
        return;
    }
    entity_id = gd::instance_id(p_entity);
    Node *root = p_entity->get_owner();
    if (root == nullptr) {
        return;
    }
    if (!root->is_inside_tree()) {
        return;
    }
    NetwMultiplayer *session = core();
    NetwEntityRecord *held
        = session != nullptr ? p_entity->record_in(session) : nullptr;
    if (held != nullptr) {
        session->interest_activate(p_entity->get_rid_handle(), *held);
    }
}

Ref<NetwEntity> NetwInterest::entity() const {
    return Ref<NetwEntity>(
        Object::cast_to<NetwEntity>(gd::object_of(entity_id))
    );
}

NetwMultiplayer *NetwInterest::core() const {
    const Ref<NetwEntity> bound = entity();
    return bound.is_valid()
        ? NetwMultiplayer::session_holding(bound->get_owner())
        : nullptr;
}

NetwMultiplayer *NetwInterest::attached_core() const {
    NetwMultiplayer *session = core();
    const Ref<NetwEntity> bound = entity();
    if (session == nullptr || bound.is_null()) {
        return nullptr;
    }
    bound->adopt_in(session);
    return session->entity_get_view(bound->get_rid_handle()).is_valid()
        ? session
        : nullptr;
}

interest::Facet *NetwInterest::facet() const {
    const Ref<NetwEntity> bound = entity();
    NetwEntityRecord *const record
        = bound.is_valid() ? bound->get_record() : nullptr;
    return record != nullptr ? &record->get_interest_facet() : nullptr;
}

interest::Decl *NetwInterest::declaration() {
    interest::Facet *held = facet();
    return held != nullptr ? held->declaration() : nullptr;
}

Ref<NetwInterest> NetwInterest::of(Node *p_node) {
    const Ref<NetwEntity> entity = NetwEntity::ensure(p_node);
    if (entity.is_null()) {
        fault::raise(
            nullptr,
            NETW_FAULT_NOT_ENTITY,
            {String("{NetwInterest.of}"),
             p_node != nullptr ? String(p_node->get_name()) : String("<null>")},
            p_node != nullptr ? uint64_t(gd::instance_id(p_node)) : 0
        );
        return Ref<NetwInterest>();
    }
    return entity->get_interest();
}

Ref<NetwInterest> NetwInterest::join(
    const StringName &p_layer_id,
    NetwMultiplayer::LeavePolicy p_leave_policy,
    NetwMultiplayer::PerceptionPolicy p_perception_policy
) {
    NetwMultiplayer *session = attached_core();
    const Ref<NetwEntity> bound = entity();
    interest::Facet *held = facet();
    if (session != nullptr && bound.is_valid()) {
        session->interest_join_label(bound->get_rid_handle(), p_layer_id);
    } else if (held != nullptr) {
        held->join(p_layer_id);
    }
    if (int64_t(p_leave_policy) >= 0) {
        on_leave_policy(p_layer_id, p_leave_policy, Callable());
    }
    if (int64_t(p_perception_policy) >= 0) {
        on_perception_policy(p_layer_id, p_perception_policy, Callable());
    }
    return Ref<NetwInterest>(this);
}

Ref<NetwInterest> NetwInterest::leave(const StringName &p_layer_id) {
    NetwMultiplayer *session = attached_core();
    const Ref<NetwEntity> bound = entity();
    interest::Facet *held = facet();
    if (session != nullptr && bound.is_valid()) {
        session->interest_leave_label(bound->get_rid_handle(), p_layer_id);
    } else if (held != nullptr) {
        held->leave(p_layer_id);
    }
    return Ref<NetwInterest>(this);
}

TypedArray<StringName> NetwInterest::layer_ids() const {
    interest::Facet *held = facet();
    return held != nullptr ? held->layer_ids() : TypedArray<StringName>();
}

Ref<NetwInterest> NetwInterest::bind_layer_callback(
    const Callable &p_callback,
    const StringName &p_layer_id,
    bool p_on_enter
) {
    NetwMultiplayer *session = attached_core();
    const Ref<NetwEntity> bound = entity();
    interest::Facet *held = facet();
    if (session != nullptr && bound.is_valid()) {
        if (p_on_enter) {
            session->interest_on_enter_label(
                bound->get_rid_handle(),
                p_layer_id,
                p_callback
            );
        } else {
            session->interest_on_leave_label(
                bound->get_rid_handle(),
                p_layer_id,
                p_callback
            );
        }
    } else if (held != nullptr) {
        if (p_on_enter) {
            held->on_enter(p_layer_id, p_callback);
        } else {
            held->on_leave(p_layer_id, p_callback);
        }
    }
    return Ref<NetwInterest>(this);
}

Ref<NetwInterest> NetwInterest::on_enter(
    const Callable &p_callback,
    const StringName &p_layer_id
) {
    if (!String(p_layer_id).is_empty()) {
        return bind_layer_callback(p_callback, p_layer_id, true);
    }
    const TypedArray<StringName> joined = layer_ids();
    for (int at = 0; at < joined.size(); ++at) {
        bind_layer_callback(p_callback, StringName(joined[at]), true);
    }
    return Ref<NetwInterest>(this);
}

Ref<NetwInterest> NetwInterest::on_leave(
    const Callable &p_callback,
    const StringName &p_layer_id
) {
    if (!String(p_layer_id).is_empty()) {
        return bind_layer_callback(p_callback, p_layer_id, false);
    }
    const TypedArray<StringName> joined = layer_ids();
    for (int at = 0; at < joined.size(); ++at) {
        bind_layer_callback(p_callback, StringName(joined[at]), false);
    }
    return Ref<NetwInterest>(this);
}

Ref<NetwInterest> NetwInterest::on_observed(const Callable &p_callback) {
    interest::Facet *held = facet();
    if (held != nullptr) {
        held->on_observed(p_callback);
    }
    return Ref<NetwInterest>(this);
}

Ref<NetwInterest> NetwInterest::on_unobserved(const Callable &p_callback) {
    interest::Facet *held = facet();
    if (held != nullptr) {
        held->on_unobserved(p_callback);
    }
    return Ref<NetwInterest>(this);
}

Ref<NetwInterest> NetwInterest::on_leave_policy(
    const StringName &p_layer_id,
    NetwMultiplayer::LeavePolicy p_policy,
    const Callable &p_custom
) {
    NetwMultiplayer *session = attached_core();
    const Ref<NetwEntity> bound = entity();
    interest::Facet *held = facet();
    if (session != nullptr && bound.is_valid()) {
        session->interest_leave_policy_label(
            bound->get_rid_handle(),
            p_layer_id,
            p_policy,
            p_custom
        );
    } else if (held != nullptr) {
        held->set_leave_policy(p_layer_id, int(p_policy), p_custom);
    }
    return Ref<NetwInterest>(this);
}

Ref<NetwInterest> NetwInterest::on_perception_policy(
    const StringName &p_layer_id,
    NetwMultiplayer::PerceptionPolicy p_policy,
    const Callable &p_custom
) {
    interest::Facet *held = facet();
    const Ref<NetwEntity> bound = entity();
    NetwMultiplayer *session = attached_core();
    if (session != nullptr && bound.is_valid()) {
        session->interest_perception_policy_label(
            bound->get_rid_handle(),
            p_layer_id,
            p_policy,
            p_custom
        );
    } else if (held != nullptr) {
        held->set_perception_policy(p_layer_id, int(p_policy), p_custom);
    }
    return Ref<NetwInterest>(this);
}

int64_t NetwInterest::leave_policy_for(
    const StringName &p_layer_id,
    int64_t p_fallback
) const {
    interest::Facet *held = facet();
    return held != nullptr
        ? held->declaration()->leave_policy_for(p_layer_id, int(p_fallback))
        : p_fallback;
}

Callable NetwInterest::custom_leave_for(const StringName &p_layer_id) const {
    interest::Facet *held = facet();
    return held != nullptr ? held->declaration()->custom_leave_for(p_layer_id)
                           : Callable();
}

int64_t NetwInterest::perception_policy_for(
    const StringName &p_layer_id,
    int64_t p_fallback
) const {
    interest::Facet *held = facet();
    return held != nullptr ? held->declaration()->perception_policy_for(
                                 p_layer_id,
                                 int(p_fallback)
                             )
                           : p_fallback;
}

Callable NetwInterest::custom_perception_for(
    const StringName &p_layer_id
) const {
    interest::Facet *held = facet();
    return held != nullptr
        ? held->declaration()->custom_perception_for(p_layer_id)
        : Callable();
}

void NetwInterest::dispatch_enter(
    const StringName &p_layer_id,
    int64_t p_peer
) {
    interest::Facet *held = facet();
    if (held != nullptr) {
        held->dispatch_enter(p_layer_id, p_peer);
    }
}

void NetwInterest::dispatch_leave(
    const StringName &p_layer_id,
    int64_t p_peer
) {
    interest::Facet *held = facet();
    if (held != nullptr) {
        held->dispatch_leave(p_layer_id, p_peer);
    }
}

void NetwInterest::_bind_methods() {
    ClassDB::bind_static_method(
        "NetwInterest",
        D_METHOD("of", "node"),
        &NetwInterest::of
    );
    ClassDB::bind_method(
        D_METHOD("join", "layer_id", "leave_policy", "perception_policy"),
        &NetwInterest::join,
        DEFVAL(-1),
        DEFVAL(-1)
    );
    ClassDB::bind_method(D_METHOD("leave", "layer_id"), &NetwInterest::leave);
    ClassDB::bind_method(D_METHOD("layer_ids"), &NetwInterest::layer_ids);
    ClassDB::bind_method(
        D_METHOD("on_enter", "handler", "layer_id"),
        &NetwInterest::on_enter,
        DEFVAL(StringName())
    );
    ClassDB::bind_method(
        D_METHOD("on_leave", "handler", "layer_id"),
        &NetwInterest::on_leave,
        DEFVAL(StringName())
    );
    ClassDB::bind_method(
        D_METHOD("on_leave_policy", "layer_id", "policy", "handler"),
        &NetwInterest::on_leave_policy,
        DEFVAL(Callable())
    );
    ClassDB::bind_method(
        D_METHOD("on_perception_policy", "layer_id", "policy", "handler"),
        &NetwInterest::on_perception_policy,
        DEFVAL(Callable())
    );
    ClassDB::bind_method(D_METHOD("entity"), &NetwInterest::entity);

    ADD_SIGNAL(
        MethodInfo("interest_entered", PropertyInfo(Variant::INT, "peer_id"))
    );
    ADD_SIGNAL(
        MethodInfo("interest_exited", PropertyInfo(Variant::INT, "peer_id"))
    );
    ADD_SIGNAL(MethodInfo(
        "observer_entered",
        PropertyInfo(Variant::STRING_NAME, "layer_id"),
        PropertyInfo(Variant::INT, "peer_id")
    ));
    ADD_SIGNAL(MethodInfo(
        "observer_exited",
        PropertyInfo(Variant::STRING_NAME, "layer_id"),
        PropertyInfo(Variant::INT, "peer_id")
    ));
}

Ref<NetwInterest> build_interest_handle(Object *p_entity) {
    Ref<NetwInterest> made;
    made.instantiate();
    made->bind(Object::cast_to<NetwEntity>(p_entity));
    return made;
}

} // namespace netw
