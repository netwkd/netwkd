#include "netw/api/request.hpp"

#include "godot/class_db.hpp"
#include "netw/fault_book.hpp"

using namespace godot;

namespace netw {

void NetwRequest::deny(const String &p_reason, Error p_code) {
    if (is_denied()) {
        return;
    }
    if (p_code == OK) {
        fault::raise(
            nullptr,
            NETW_FAULT_BAD_VALUE,
            {"{NetwRequest.deny}", "OK", "the Error to deny with"}
        );
        return;
    }
    code = p_code;
    reason = p_reason;
    denied();
}

void NetwRequest::_bind_methods() {
    ClassDB::bind_method(
        D_METHOD("deny", "reason", "code"),
        &NetwRequest::deny,
        DEFVAL(String()),
        DEFVAL(ERR_UNAUTHORIZED)
    );
    ClassDB::bind_method(D_METHOD("get_peer_id"), &NetwRequest::get_peer_id);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "peer_id"), "", "get_peer_id");
    ClassDB::bind_method(D_METHOD("get_code"), &NetwRequest::get_code);
    ADD_PROPERTY(
        PropertyInfo(
            Variant::INT,
            "code",
            PROPERTY_HINT_NONE,
            "",
            PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_CLASS_IS_ENUM,
            "Error"
        ),
        "",
        "get_code"
    );
    ClassDB::bind_method(D_METHOD("get_reason"), &NetwRequest::get_reason);
    ADD_PROPERTY(PropertyInfo(Variant::STRING, "reason"), "", "get_reason");
}

} // namespace netw
