#include "netw/api/action_request.hpp"

#include "godot/class_db.hpp"
#include "godot/utility.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/fault_book.hpp"

using namespace godot;

namespace netw {

NetwMultiplayer *NetwActionRequest::session() const {
    return Object::cast_to<NetwMultiplayer>(gd::object_of(session_id));
}

void NetwActionRequest::open(
    NetwMultiplayer *p_session,
    int64_t p_requester,
    int64_t p_view_tick,
    int64_t p_requested_tick,
    int64_t p_execution_tick,
    const StringName &p_key
) {
    session_id = gd::instance_id(p_session);
    peer_id = p_requester;
    view_tick = p_view_tick;
    requested_tick = p_requested_tick;
    execution_tick = p_execution_tick;
    key = p_key;
}

namespace {

bool built_async(Node *p_node) {
    const Ref<NetwEntity> entity = NetwEntity::of(p_node);
    if (entity.is_null()) {
        return false;
    }
    const Ref<NetwMultiplayer> session = entity->get_multiplayer();
    return session.is_valid() && session->spawn_fn_waits(entity->get_route());
}

} // namespace

void NetwActionRequest::bind_result(Node *p_node) {
    if (p_node != nullptr && built_async(p_node)) {
        fault::raise(
            session(),
            NETW_FAULT_SPAWN_REFUSED,
            {vformat("'%s'", String(p_node->get_name())),
             String("a node built by {Netw.spawn_async}"),
             String("{Netw.spawn} inside the action")},
            gd::instance_id(p_node)
        );
        return;
    }
    bound = true;
    NetwMultiplayer::wrapper_stamp_identity(p_node, key, 0);
    const Ref<NetwEntity> entity = NetwEntity::ensure(p_node);
    if (entity.is_valid()) {
        entity->set_action_spawn_tick(view_tick);
        entity->set_action_requester(peer_id);
    }
}

void NetwActionRequest::denied() {
    if (NetwMultiplayer *core = session()) {
        core->lagcomp_action_deny(peer_id, key, code, reason);
    }
}

void NetwActionRequest::confirm_unanswered() {
    if (answered()) {
        return;
    }
    if (NetwMultiplayer *core = session()) {
        core->lagcomp_action_confirm(peer_id, key);
    }
}

void NetwActionRequest::_bind_methods() {
    ClassDB::bind_method(
        D_METHOD("get_view_tick"),
        &NetwActionRequest::get_view_tick
    );
    ADD_PROPERTY(
        PropertyInfo(
            Variant::INT,
            "view_tick",
            PROPERTY_HINT_NONE,
            "",
            PROPERTY_USAGE_NONE
        ),
        "",
        "get_view_tick"
    );
    ClassDB::bind_method(
        D_METHOD("get_requested_tick"),
        &NetwActionRequest::get_requested_tick
    );
    ADD_PROPERTY(
        PropertyInfo(
            Variant::INT,
            "requested_tick",
            PROPERTY_HINT_NONE,
            "",
            PROPERTY_USAGE_NONE
        ),
        "",
        "get_requested_tick"
    );
    ClassDB::bind_method(
        D_METHOD("get_execution_tick"),
        &NetwActionRequest::get_execution_tick
    );
    ADD_PROPERTY(
        PropertyInfo(
            Variant::INT,
            "execution_tick",
            PROPERTY_HINT_NONE,
            "",
            PROPERTY_USAGE_NONE
        ),
        "",
        "get_execution_tick"
    );
    ClassDB::bind_method(
        D_METHOD("bind", "node"),
        &NetwActionRequest::bind_result
    );
}

} // namespace netw
