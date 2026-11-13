#include "netw/api/netw_transport.hpp"

#include "godot/class_db.hpp"
#include "godot/object.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/connect/browse_list.hpp"
#include "netw/connect/transport.hpp"

using namespace godot;

namespace netw {

void NetwTransport::_bind_methods() {
    ClassDB::bind_method(D_METHOD("peer_class"), &NetwTransport::peer_class);
    ClassDB::bind_method(
        D_METHOD("recognizes_peer", "peer"),
        &NetwTransport::recognizes_peer
    );
    ClassDB::bind_method(
        D_METHOD("display_name"),
        &NetwTransport::display_name
    );
    ClassDB::bind_method(
        D_METHOD("is_available"),
        &NetwTransport::is_available
    );
    ClassDB::bind_method(
        D_METHOD("can_host_here"),
        &NetwTransport::can_host_here
    );
    ClassDB::bind_method(D_METHOD("can_probe"), &NetwTransport::can_probe);
    ClassDB::bind_method(
        D_METHOD("address_label"),
        &NetwTransport::address_label
    );
    ClassDB::bind_method(
        D_METHOD("address_placeholder"),
        &NetwTransport::address_placeholder
    );
    ClassDB::bind_method(
        D_METHOD("address_help"),
        &NetwTransport::address_help
    );
    ClassDB::bind_method(
        D_METHOD("accepts_empty_address"),
        &NetwTransport::accepts_empty_address
    );
    ClassDB::bind_method(
        D_METHOD("host_settings"),
        &NetwTransport::host_settings
    );
    ClassDB::bind_method(
        D_METHOD("client_settings"),
        &NetwTransport::client_settings
    );
    ClassDB::bind_method(
        D_METHOD("make_probe_peer", "address"),
        &NetwTransport::make_probe_peer
    );
    ClassDB::bind_method(
        D_METHOD("join_address"),
        &NetwTransport::join_address
    );
    ClassDB::bind_method(
        D_METHOD("diagnostics", "peer_id"),
        &NetwTransport::diagnostics
    );
    ClassDB::bind_method(
        D_METHOD("timeout_hint"),
        &NetwTransport::timeout_hint
    );

    ClassDB::bind_method(
        D_METHOD("report", "ticket", "step", "message", "ratio"),
        &NetwTransport::report
    );
    ClassDB::bind_method(
        D_METHOD("deliver", "ticket", "peer"),
        &NetwTransport::deliver
    );
    ClassDB::bind_method(
        D_METHOD("publish_targets", "addresses", "names", "infos"),
        &NetwTransport::publish_targets,
        DEFVAL(PackedStringArray()),
        DEFVAL(Array())
    );
    ClassDB::bind_method(D_METHOD("can_browse"), &NetwTransport::can_browse);
    ClassDB::bind_method(
        D_METHOD("deliver_probe", "ticket", "info"),
        &NetwTransport::deliver_probe
    );
    ClassDB::bind_method(
        D_METHOD("fail", "ticket", "error", "message"),
        &NetwTransport::fail,
        DEFVAL(String())
    );
    ClassDB::bind_method(D_METHOD("get_session"), &NetwTransport::get_session);

    ClassDB::bind_static_method(
        "NetwTransport",
        D_METHOD("peer_class_of", "peer"),
        &NetwTransport::peer_class_of
    );

    ADD_PROPERTY(
        PropertyInfo(
            Variant::OBJECT,
            "session",
            PROPERTY_HINT_RESOURCE_TYPE,
            "NetwSession"
        ),
        "",
        "get_session"
    );

    GDVIRTUAL_BIND(_peer_class);
    GDVIRTUAL_BIND(_recognizes_peer, "peer");
    GDVIRTUAL_BIND(_display_name);
    GDVIRTUAL_BIND(_is_available);
    GDVIRTUAL_BIND(_can_host_here);
    GDVIRTUAL_BIND(_can_probe);
    GDVIRTUAL_BIND(_address_label);
    GDVIRTUAL_BIND(_address_placeholder);
    GDVIRTUAL_BIND(_address_help);
    GDVIRTUAL_BIND(_accepts_empty_address);
    GDVIRTUAL_BIND(_host_settings);
    GDVIRTUAL_BIND(_client_settings);
    GDVIRTUAL_BIND(_make_peer, "ticket", "mode", "address", "settings");
    GDVIRTUAL_BIND(_cancel_peer_creation, "ticket");
    GDVIRTUAL_BIND(_probe, "ticket", "address");
    GDVIRTUAL_BIND(_can_browse);
    GDVIRTUAL_BIND(_browse);
    GDVIRTUAL_BIND(_make_probe_peer, "address");
    GDVIRTUAL_BIND(_adopt, "peer");
    GDVIRTUAL_BIND(_poll, "delta");
    GDVIRTUAL_BIND(_join_address);
    GDVIRTUAL_BIND(_diagnostics, "peer_id");
    GDVIRTUAL_BIND(_timeout_hint);
    GDVIRTUAL_BIND(_close);
}

StringName NetwTransport::peer_class_of(const Ref<MultiplayerPeer> &p_peer) {
    return connect::peer_class_of(p_peer);
}

void NetwTransport::bind_session(NetwMultiplayer *p_session) {
    session_id = gd::instance_id(p_session);
}

NetwMultiplayer *NetwTransport::session() const {
    return Object::cast_to<NetwMultiplayer>(gd::object_of(session_id));
}

Ref<NetwSession> NetwTransport::get_session() const {
    NetwMultiplayer *owner = session();
    return owner != nullptr ? owner->get_session() : Ref<NetwSession>();
}

void NetwTransport::report(
    const RID &p_ticket,
    const StringName &,
    const String &p_message,
    double p_ratio
) {
    NetwMultiplayer *owner = session();
    if (owner != nullptr) {
        owner->transport_report_progress(p_ticket, p_message, p_ratio);
    }
}

void NetwTransport::deliver(
    const RID &p_ticket,
    const Ref<MultiplayerPeer> &p_peer
) {
    NetwMultiplayer *owner = session();
    if (owner != nullptr) {
        owner->transport_complete(p_ticket, OK, p_peer, String());
    }
}

void NetwTransport::deliver_probe(
    const RID &p_ticket,
    const Ref<NetwServerInfo> &p_info
) {
    NetwMultiplayer *owner = session();
    if (owner != nullptr) {
        owner->transport_complete(p_ticket, OK, p_info, String());
    }
}

void NetwTransport::fail(
    const RID &p_ticket,
    Error p_error,
    const String &p_message
) {
    NetwMultiplayer *owner = session();
    if (owner != nullptr) {
        owner->transport_complete(p_ticket, p_error, Variant(), p_message);
    }
}

StringName NetwTransport::peer_class() {
    StringName answered;
    if (GDVIRTUAL_CALL(_peer_class, answered)) {
        return answered;
    }
    return StringName();
}

bool NetwTransport::recognizes_peer(const Ref<MultiplayerPeer> &p_peer) {
    bool answered = false;
    if (GDVIRTUAL_CALL(_recognizes_peer, p_peer, answered)) {
        return answered;
    }
    return peer_class_of(p_peer) == peer_class();
}

String NetwTransport::display_name() {
    String answered;
    if (GDVIRTUAL_CALL(_display_name, answered)) {
        return answered;
    }
    return String("Generic");
}

bool NetwTransport::is_available() {
    bool answered = false;
    if (GDVIRTUAL_CALL(_is_available, answered)) {
        return answered;
    }
    return true;
}

bool NetwTransport::can_host_here() {
    bool answered = false;
    if (GDVIRTUAL_CALL(_can_host_here, answered)) {
        return answered;
    }
    return true;
}

bool NetwTransport::can_probe() {
    bool answered = false;
    if (GDVIRTUAL_CALL(_can_probe, answered)) {
        return answered;
    }
    return false;
}

String NetwTransport::address_label() {
    String answered;
    if (GDVIRTUAL_CALL(_address_label, answered)) {
        return answered;
    }
    return String("Address");
}

String NetwTransport::address_placeholder() {
    String answered;
    if (GDVIRTUAL_CALL(_address_placeholder, answered)) {
        return answered;
    }
    return String();
}

String NetwTransport::address_help() {
    String answered;
    if (GDVIRTUAL_CALL(_address_help, answered)) {
        return answered;
    }
    return String();
}

bool NetwTransport::accepts_empty_address() {
    bool answered = false;
    if (GDVIRTUAL_CALL(_accepts_empty_address, answered)) {
        return answered;
    }
    return false;
}

Dictionary NetwTransport::host_settings() {
    Dictionary answered;
    if (GDVIRTUAL_CALL(_host_settings, answered)) {
        return answered;
    }
    return Dictionary();
}

Dictionary NetwTransport::client_settings() {
    Dictionary answered;
    if (GDVIRTUAL_CALL(_client_settings, answered)) {
        return answered;
    }
    return Dictionary();
}

bool NetwTransport::make_peer(
    const RID &p_ticket,
    NetwMultiplayer::TransportMode p_mode,
    const String &p_address,
    const Dictionary &p_settings
) {
    return GDVIRTUAL_CALL(_make_peer, p_ticket, p_mode, p_address, p_settings);
}

void NetwTransport::cancel_peer_creation(const RID &p_ticket) {
    GDVIRTUAL_CALL(_cancel_peer_creation, p_ticket);
}

bool NetwTransport::can_browse() {
    bool answered = false;
    if (GDVIRTUAL_CALL(_can_browse, answered)) {
        return answered;
    }
    return false;
}

void NetwTransport::browse() {
    GDVIRTUAL_CALL(_browse);
}

void NetwTransport::publish_targets(
    const PackedStringArray &p_addresses,
    const PackedStringArray &p_names,
    const Array &p_infos
) {
    NetwMultiplayer *owner = session();
    if (owner != nullptr) {
        owner->transport_publish_targets(
            owner->transport_find(peer_class()),
            connect::targets_of(p_addresses, p_names, p_infos)
        );
    }
}

bool NetwTransport::probe(const RID &p_ticket, const String &p_address) {
    return GDVIRTUAL_CALL(_probe, p_ticket, p_address);
}

Ref<MultiplayerPeer> NetwTransport::make_probe_peer(const String &p_address) {
    Ref<MultiplayerPeer> answered;
    if (GDVIRTUAL_CALL(_make_probe_peer, p_address, answered)) {
        return answered;
    }
    return Ref<MultiplayerPeer>();
}

void NetwTransport::adopt(const Ref<MultiplayerPeer> &p_peer) {
    if (GDVIRTUAL_CALL(_adopt, p_peer)) {
        return;
    }
}

void NetwTransport::poll(double p_delta) {
    if (GDVIRTUAL_CALL(_poll, p_delta)) {
        return;
    }
}

String NetwTransport::join_address() {
    String answered;
    if (GDVIRTUAL_CALL(_join_address, answered)) {
        return answered;
    }
    return String();
}

Dictionary NetwTransport::diagnostics(int64_t p_peer_id) {
    Dictionary answered;
    if (GDVIRTUAL_CALL(_diagnostics, p_peer_id, answered)) {
        return answered;
    }
    return Dictionary();
}

double NetwTransport::timeout_hint() {
    double answered = 0.0;
    if (GDVIRTUAL_CALL(_timeout_hint, answered)) {
        return answered;
    }
    return 5.0;
}

void NetwTransport::close() {
    if (GDVIRTUAL_CALL(_close)) {
        return;
    }
}

} // namespace netw
