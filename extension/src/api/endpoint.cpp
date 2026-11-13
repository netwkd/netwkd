#include "netw/api/endpoint.hpp"

#include "godot/class_db.hpp"
#include "netw/api/netw_multiplayer.hpp"

using namespace godot;

namespace netw {

enums::NetwEndpoint::Status endpoint_status(int64_t p_flags, int64_t p_error) {
    if ((p_flags & enums::NetwMultiplayer::ENDPOINT_FLAG_AVAILABLE) == 0) {
        return enums::NetwEndpoint::STATUS_UNSUPPORTED;
    }
    if ((p_flags & enums::NetwMultiplayer::ENDPOINT_FLAG_OBSERVED) == 0) {
        return enums::NetwEndpoint::STATUS_UNKNOWN;
    }
    switch (p_error) {
        case OK:
            return enums::NetwEndpoint::STATUS_ONLINE;
        case ERR_BUSY:
            return enums::NetwEndpoint::STATUS_FULL;
        case ERR_UNAUTHORIZED:
            return enums::NetwEndpoint::STATUS_INCOMPATIBLE;
        case ERR_UNAVAILABLE:
            return enums::NetwEndpoint::STATUS_UNKNOWN;
        default:
            return enums::NetwEndpoint::STATUS_UNREACHABLE;
    }
}

NetwMultiplayer *NetwEndpoint::session() const {
    return Object::cast_to<NetwMultiplayer>(gd::object_of(session_id));
}

String NetwEndpoint::text_param(int64_t p_param) const {
    NetwMultiplayer *api = session();
    if (api == nullptr) {
        return String();
    }
    const Variant held = api->endpoint_get_param(
        endpoint,
        NetwMultiplayer::EndpointParam(p_param)
    );
    return held.get_type() == Variant::STRING ? String(held) : String();
}

int64_t NetwEndpoint::flags() const {
    NetwMultiplayer *api = session();
    if (api == nullptr) {
        return -1;
    }
    const Variant held = api->endpoint_get_state(
        endpoint,
        NetwMultiplayer::ENDPOINT_STATE_FLAGS
    );
    return held.get_type() == Variant::INT ? int64_t(held) : -1;
}

RID NetwEndpoint::get_rid() const {
    return endpoint;
}

StringName NetwEndpoint::get_peer_class() const {
    return peer_class;
}

String NetwEndpoint::get_address() const {
    return address;
}

String NetwEndpoint::get_display_name() const {
    return text_param(NetwMultiplayer::ENDPOINT_PARAM_DISPLAY_NAME);
}

String NetwEndpoint::get_source() const {
    return text_param(NetwMultiplayer::ENDPOINT_PARAM_SOURCE);
}

NetwEndpoint::Status NetwEndpoint::get_status() const {
    const int64_t held = flags();
    if (held < 0) {
        return STATUS_UNKNOWN;
    }
    return endpoint_status(
        held,
        session()->endpoint_get_state(
            endpoint,
            NetwMultiplayer::ENDPOINT_STATE_STATUS
        )
    );
}

Ref<NetwServerInfo> NetwEndpoint::get_info() {
    NetwMultiplayer *api = session();
    if (api != nullptr) {
        const Ref<NetwServerInfo> answered = api->endpoint_get_state(
            endpoint,
            NetwMultiplayer::ENDPOINT_STATE_INFO
        );
        if (answered.is_valid()) {
            return answered;
        }
    }
    if (unanswered.is_null()) {
        unanswered.instantiate();
    }
    return unanswered;
}

bool NetwEndpoint::get_is_listed() const {
    NetwMultiplayer *api = session();
    return api != nullptr && api->endpoint_list().has(endpoint);
}

bool NetwEndpoint::get_is_invited() const {
    const int64_t held = flags();
    return held >= 0 && (held & NetwMultiplayer::ENDPOINT_FLAG_INVITED) != 0;
}

String NetwEndpoint::get_invited_by() const {
    return text_param(NetwMultiplayer::ENDPOINT_PARAM_INVITED_BY);
}

void NetwEndpoint::probe() {
    NetwMultiplayer *api = session();
    if (api != nullptr) {
        api->endpoint_probe(endpoint);
    }
}

void NetwEndpoint::_bind_methods() {
    BIND_ENUM_CONSTANT(STATUS_UNKNOWN);
    BIND_ENUM_CONSTANT(STATUS_ONLINE);
    BIND_ENUM_CONSTANT(STATUS_FULL);
    BIND_ENUM_CONSTANT(STATUS_UNREACHABLE);
    BIND_ENUM_CONSTANT(STATUS_INCOMPATIBLE);
    BIND_ENUM_CONSTANT(STATUS_UNSUPPORTED);

    ClassDB::bind_method(D_METHOD("get_rid"), &NetwEndpoint::get_rid);
    ADD_PROPERTY(PropertyInfo(Variant::RID, "rid"), "", "get_rid");
    ClassDB::bind_method(
        D_METHOD("get_peer_class"),
        &NetwEndpoint::get_peer_class
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::STRING_NAME, "peer_class"),
        "",
        "get_peer_class"
    );
    ClassDB::bind_method(D_METHOD("get_address"), &NetwEndpoint::get_address);
    ADD_PROPERTY(PropertyInfo(Variant::STRING, "address"), "", "get_address");
    ClassDB::bind_method(
        D_METHOD("get_display_name"),
        &NetwEndpoint::get_display_name
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::STRING, "display_name"),
        "",
        "get_display_name"
    );
    ClassDB::bind_method(D_METHOD("get_source"), &NetwEndpoint::get_source);
    ADD_PROPERTY(PropertyInfo(Variant::STRING, "source"), "", "get_source");
    ClassDB::bind_method(D_METHOD("get_status"), &NetwEndpoint::get_status);
    ADD_PROPERTY(
        PropertyInfo(
            Variant::INT,
            "status",
            PROPERTY_HINT_NONE,
            "",
            PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_CLASS_IS_ENUM,
            "NetwEndpoint.Status"
        ),
        "",
        "get_status"
    );
    ClassDB::bind_method(D_METHOD("get_info"), &NetwEndpoint::get_info);
    ADD_PROPERTY(
        PropertyInfo(
            Variant::OBJECT,
            "info",
            PROPERTY_HINT_RESOURCE_TYPE,
            "NetwServerInfo",
            PROPERTY_USAGE_NONE
        ),
        "",
        "get_info"
    );
    ClassDB::bind_method(
        D_METHOD("get_is_listed"),
        &NetwEndpoint::get_is_listed
    );
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "is_listed"), "", "get_is_listed");
    ClassDB::bind_method(
        D_METHOD("get_is_invited"),
        &NetwEndpoint::get_is_invited
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::BOOL, "is_invited"),
        "",
        "get_is_invited"
    );
    ClassDB::bind_method(
        D_METHOD("get_invited_by"),
        &NetwEndpoint::get_invited_by
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::STRING, "invited_by"),
        "",
        "get_invited_by"
    );
    ClassDB::bind_method(D_METHOD("probe"), &NetwEndpoint::probe);

    ADD_SIGNAL(MethodInfo("changed"));
}

} // namespace netw
