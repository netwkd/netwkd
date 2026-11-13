#include "netw/api/channel.hpp"

#include "godot/callable.hpp"
#include "godot/class_db.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/fault_book.hpp"
#include "netw/log.hpp"

using namespace godot;

namespace netw {

NetwMultiplayer *NetwChannel::multiplayer() const {
    return Object::cast_to<NetwMultiplayer>(gd::object_of(session));
}

Ref<NetwChannel> NetwChannel::over(int64_t p_id, NetwMultiplayer *p_session) {
    if (p_id < NetwMultiplayer::CHANNEL_USER_FIRST
        || p_id > NetwMultiplayer::CHANNEL_USER_LAST) {
        fault::raise(
            p_session,
            NETW_FAULT_BAD_VALUE,
            {String("{Netw.channel}"),
             vformat("channel id %d", int(p_id)),
             vformat("an id from %d to %d",
                     int(NetwMultiplayer::CHANNEL_USER_FIRST),
                     int(NetwMultiplayer::CHANNEL_USER_LAST))}
        );
        return Ref<NetwChannel>();
    }
    Ref<NetwChannel> channel;
    channel.instantiate();
    channel->id = p_id;
    channel->session = gd::instance_id(p_session);
    return channel;
}

Ref<NetwChannel> NetwChannel::of(Node *p_node, int64_t p_id) {
    NetwMultiplayer *api = NetwMultiplayer::session_holding(p_node);
    if (api == nullptr) {
        fault::raise(
            nullptr,
            NETW_FAULT_NOT_IN_SESSION,
            {String("{Netw.channel}"),
             p_node != nullptr ? String(p_node->get_name()) : String("<null>")},
            p_node != nullptr ? uint64_t(gd::instance_id(p_node)) : 0
        );
        return Ref<NetwChannel>();
    }
    return over(p_id, api);
}

void NetwChannel::send(
    int64_t p_peer_id,
    const PackedByteArray &p_payload,
    bool p_reliable,
    bool p_batched
) {
    NetwMultiplayer *api = multiplayer();
    if (api == nullptr) {
        return;
    }
    api->rpc_send(p_peer_id, id, p_payload, p_reliable, p_batched);
}

void NetwChannel::broadcast(
    const PackedByteArray &p_payload,
    bool p_reliable,
    bool p_batched
) {
    send(0, p_payload, p_reliable, p_batched);
}

void NetwChannel::register_handler(const Callable &p_handler) {
    NetwMultiplayer *api = multiplayer();
    if (api == nullptr) {
        return;
    }
    api->rpc_channel_install(id, p_handler, false, true);
}

void NetwChannel::_bind_methods() {
    ClassDB::bind_static_method(
        "NetwChannel",
        D_METHOD("of", "node", "id"),
        &NetwChannel::of
    );
    ClassDB::bind_method(
        D_METHOD("send", "peer_id", "payload", "reliable", "batched"),
        &NetwChannel::send,
        DEFVAL(true),
        DEFVAL(false)
    );
    ClassDB::bind_method(
        D_METHOD("broadcast", "payload", "reliable", "batched"),
        &NetwChannel::broadcast,
        DEFVAL(true),
        DEFVAL(false)
    );
    ClassDB::bind_method(
        D_METHOD("register", "handler"),
        &NetwChannel::register_handler
    );

    ClassDB::bind_method(D_METHOD("set_id", "id"), &NetwChannel::set_id);
    ClassDB::bind_method(D_METHOD("get_id"), &NetwChannel::get_id);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "id"), "set_id", "get_id");
}

} // namespace netw
