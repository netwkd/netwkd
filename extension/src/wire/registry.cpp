#include "netw/sealed.hpp"

#include "netw/wire/registry.hpp"

#include "netw/colors.hpp"
#include "netw/fault_book.hpp"
#include "netw/log.hpp"
#include "netw/profile.hpp"

namespace netw::wire {

namespace {

uint64_t hash_combine(uint64_t h, uint64_t v) {
    return h ^ (v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2));
}

} // namespace

bool WireRegistry::register_channel(const ChannelDecl &decl) {
    if (decl.id == 0 && !decl.is_reserved) {
        fault::raise(
            nullptr,
            NETW_FAULT_BAD_VALUE,
            {godot::String("A channel registration"),
             godot::vformat("id 0 for '%s'", godot::String(decl.name)),
             godot::String("an id above 0")}
        );
        return false;
    }
    if (registered[decl.id] && channels[decl.id].name != decl.name) {
        fault::raise(
            nullptr,
            NETW_FAULT_DECLARED_AGAIN,
            {godot::vformat("Channel id %d", int(decl.id)),
             godot::String(channels[decl.id].name)}
        );
    }
    channels[decl.id] = decl;
    registered[decl.id] = true;
    return true;
}

const ChannelDecl *WireRegistry::find_channel(uint8_t id) const {
    if (!registered[id]) {
        return nullptr;
    }
    return &channels[id];
}

Delivery WireRegistry::delivery_of(uint8_t id) const {
    const ChannelDecl *decl = find_channel(id);
    if (decl == nullptr || decl->is_reserved) {
        return Delivery::IMMEDIATE;
    }
    return decl->delivery;
}

Delivery WireRegistry::delivery_of(uint8_t id, Delivery requested) const {
    const ChannelDecl *decl = find_channel(id);
    if (decl == nullptr || decl->is_reserved) {
        return requested;
    }
    if (decl->delivery == Delivery::IMMEDIATE) {
        return Delivery::IMMEDIATE;
    }
    return requested;
}

Cause WireRegistry::cause_of(uint8_t id) const {
    return id >= USER_CHANNEL_FIRST ? Cause::CALLS : channels[id].cause;
}

uint32_t WireRegistry::active_count() const {
    uint32_t count = 0;
    for (int i = 0; i < MAX_CHANNELS; ++i) {
        if (registered[i] && !channels[i].is_reserved) {
            count++;
        }
    }
    return count;
}

bool WireRegistry::id_is_builtin(uint8_t id) {
    static const WireRegistry table = create_default();
    return table.registered[id];
}

uint64_t WireRegistry::identity_hash() const {
    uint64_t hash = 14695981039346656037ULL;
    hash = hash_combine(hash, FORMAT_VERSION);
    for (int i = 0; i < MAX_CHANNELS; ++i) {
        if (!registered[i] || channels[i].is_reserved) {
            continue;
        }
        const ChannelDecl &d = channels[i];
        hash = hash_combine(hash, d.id);
        hash = hash_combine(hash, static_cast<uint64_t>(d.kind));
        hash = hash_combine(hash, static_cast<uint64_t>(d.reliability));
        hash = hash_combine(hash, static_cast<uint64_t>(d.freshness));
        hash = hash_combine(hash, static_cast<uint64_t>(d.delivery));
        hash = hash_combine(hash, static_cast<uint64_t>(d.direction));
        hash = hash_combine(hash, static_cast<uint64_t>(d.payload));
        if (d.payload_revision != 0) {
            hash = hash_combine(hash, d.payload_revision);
        }
        hash = hash_combine(hash, d.name.hash());
    }
    return hash;
}

WireRegistry WireRegistry::create_default() {
    NETW_ZONE_NC("WireRegistry default table", colors::WIRE);
    WireRegistry reg;

    auto res = [&](uint8_t id) {
        ChannelDecl d;
        d.id = id;
        d.is_reserved = true;
        reg.register_channel(d);
    };

    auto reg_c = [&](Channel id,
                     const char *name,
                     ChannelKind kind,
                     Reliability rel,
                     Freshness fresh,
                     Delivery deliv,
                     Direction dir,
                     PayloadContract payload,
                     uint16_t revision = 0) {
        ChannelDecl d;
        d.id = id;
        d.name = godot::StringName(name);
        d.kind = kind;
        d.reliability = rel;
        d.freshness = fresh;
        d.delivery = deliv;
        d.direction = dir;
        d.payload = payload;
        d.payload_revision = revision;
        reg.register_channel(d);
    };

    res(0);
    res(1);
    res(7);

    reg_c(
        CHANNEL_ACTION,
        "ACTION",
        ChannelKind::ROUTED,
        Reliability::RELIABLE,
        Freshness::NONE,
        Delivery::IMMEDIATE,
        Direction::EITHER,
        PayloadContract::PLANNED,
        2
    );
    reg_c(
        CHANNEL_CALL,
        "CALL",
        ChannelKind::ROUTED,
        Reliability::RELIABLE,
        Freshness::NONE,
        Delivery::IMMEDIATE,
        Direction::EITHER,
        PayloadContract::PLANNED,
        1
    );
    reg_c(
        CHANNEL_REPLY,
        "REPLY",
        ChannelKind::ROUTED,
        Reliability::RELIABLE,
        Freshness::NONE,
        Delivery::IMMEDIATE,
        Direction::EITHER,
        PayloadContract::PLANNED,
        1
    );
    reg_c(
        CHANNEL_SIGNAL,
        "SIGNAL",
        ChannelKind::ROUTED,
        Reliability::RELIABLE,
        Freshness::NONE,
        Delivery::TICK,
        Direction::EITHER,
        PayloadContract::PLANNED,
        1
    );
    reg_c(
        CHANNEL_PROPERTY_SYNC,
        "PROPERTY_SYNC",
        ChannelKind::ROUTED,
        Reliability::RELIABLE,
        Freshness::NONE,
        Delivery::TICK,
        Direction::EITHER,
        PayloadContract::PLANNED,
        1
    );

    reg_c(
        CHANNEL_INTEREST_AWARENESS,
        "INTEREST_AWARENESS",
        ChannelKind::SESSION,
        Reliability::RELIABLE,
        Freshness::NONE,
        Delivery::IMMEDIATE,
        Direction::SERVER_TO_CLIENT,
        PayloadContract::PLANNED,
        1
    );
    reg_c(
        CHANNEL_CLOCK_HANDSHAKE,
        "CLOCK_HANDSHAKE",
        ChannelKind::SESSION,
        Reliability::RELIABLE,
        Freshness::NONE,
        Delivery::TICK,
        Direction::CLIENT_TO_SERVER,
        PayloadContract::PLANNED
    );
    reg_c(
        CHANNEL_CLOCK_HANDSHAKE_REPLY,
        "CLOCK_HANDSHAKE_REPLY",
        ChannelKind::SESSION,
        Reliability::RELIABLE,
        Freshness::NONE,
        Delivery::TICK,
        Direction::SERVER_TO_CLIENT,
        PayloadContract::PLANNED
    );
    reg_c(
        CHANNEL_CLOCK_PING,
        "CLOCK_PING",
        ChannelKind::SESSION,
        Reliability::UNRELIABLE,
        Freshness::NONE,
        Delivery::RIDE,
        Direction::CLIENT_TO_SERVER,
        PayloadContract::PLANNED
    );
    reg_c(
        CHANNEL_CLOCK_PONG,
        "CLOCK_PONG",
        ChannelKind::SESSION,
        Reliability::UNRELIABLE,
        Freshness::NONE,
        Delivery::TICK,
        Direction::SERVER_TO_CLIENT,
        PayloadContract::PLANNED,
        1
    );

    reg_c(
        CHANNEL_LAGCOMP_DENY,
        "LAGCOMP_DENY",
        ChannelKind::SESSION,
        Reliability::RELIABLE,
        Freshness::NONE,
        Delivery::IMMEDIATE,
        Direction::SERVER_TO_CLIENT,
        PayloadContract::PLANNED,
        1
    );
    reg_c(
        CHANNEL_SPAWN,
        "SPAWN",
        ChannelKind::SESSION,
        Reliability::RELIABLE,
        Freshness::NONE,
        Delivery::TICK,
        Direction::EITHER,
        PayloadContract::PLANNED,
        6
    );
    reg_c(
        CHANNEL_DESPAWN,
        "DESPAWN",
        ChannelKind::SESSION,
        Reliability::RELIABLE,
        Freshness::NONE,
        Delivery::TICK,
        Direction::EITHER,
        PayloadContract::PLANNED,
        2
    );
    reg_c(
        CHANNEL_REPARENT,
        "REPARENT",
        ChannelKind::SESSION,
        Reliability::RELIABLE,
        Freshness::NONE,
        Delivery::TICK,
        Direction::EITHER,
        PayloadContract::PLANNED,
        2
    );
    reg_c(
        CHANNEL_TABLE,
        "TABLE",
        ChannelKind::KEYED,
        Reliability::UNRELIABLE_ACKED,
        Freshness::FRESHEST_WINS,
        Delivery::TICK,
        Direction::SERVER_TO_CLIENT,
        PayloadContract::DELTA
    );

    reg_c(
        CHANNEL_HIDE,
        "HIDE",
        ChannelKind::SESSION,
        Reliability::RELIABLE,
        Freshness::NONE,
        Delivery::TICK,
        Direction::SERVER_TO_CLIENT,
        PayloadContract::PLANNED
    );

    reg_c(
        CHANNEL_SYNC,
        "SYNC",
        ChannelKind::KEYED,
        Reliability::UNRELIABLE_ACKED,
        Freshness::FRESHEST_WINS,
        Delivery::TICK,
        Direction::EITHER,
        PayloadContract::DELTA,
        1
    );
    reg_c(
        CHANNEL_SYNC_DELTA,
        "SYNC_DELTA",
        ChannelKind::ROUTED,
        Reliability::RELIABLE,
        Freshness::NONE,
        Delivery::TICK,
        Direction::EITHER,
        PayloadContract::DELTA,
        1
    );
    reg_c(
        CHANNEL_CONTROL_REQUEST,
        "CONTROL_REQUEST",
        ChannelKind::ROUTED,
        Reliability::RELIABLE,
        Freshness::NONE,
        Delivery::IMMEDIATE,
        Direction::CLIENT_TO_SERVER,
        PayloadContract::PLANNED,
        1
    );
    reg_c(
        CHANNEL_CONTROL_APPLY,
        "CONTROL_APPLY",
        ChannelKind::ROUTED,
        Reliability::RELIABLE,
        Freshness::NONE,
        Delivery::IMMEDIATE,
        Direction::SERVER_TO_CLIENT,
        PayloadContract::PLANNED,
        2
    );

    reg_c(
        CHANNEL_SESSION_JOIN,
        "SESSION_JOIN",
        ChannelKind::SESSION,
        Reliability::RELIABLE,
        Freshness::NONE,
        Delivery::IMMEDIATE,
        Direction::CLIENT_TO_SERVER,
        PayloadContract::PLANNED,
        4
    );
    reg_c(
        CHANNEL_SESSION_ACCEPT,
        "SESSION_ACCEPT",
        ChannelKind::SESSION,
        Reliability::RELIABLE,
        Freshness::NONE,
        Delivery::IMMEDIATE,
        Direction::SERVER_TO_CLIENT,
        PayloadContract::PLANNED,
        3
    );
    reg_c(
        CHANNEL_SESSION_ROSTER,
        "SESSION_ROSTER",
        ChannelKind::SESSION,
        Reliability::RELIABLE,
        Freshness::NONE,
        Delivery::IMMEDIATE,
        Direction::SERVER_TO_CLIENT,
        PayloadContract::PLANNED,
        3
    );
    reg_c(
        CHANNEL_SESSION_PAUSE,
        "SESSION_PAUSE",
        ChannelKind::SESSION,
        Reliability::RELIABLE,
        Freshness::NONE,
        Delivery::IMMEDIATE,
        Direction::SERVER_TO_CLIENT,
        PayloadContract::PLANNED
    );
    reg_c(
        CHANNEL_SESSION_UNPAUSE,
        "SESSION_UNPAUSE",
        ChannelKind::SESSION,
        Reliability::RELIABLE,
        Freshness::NONE,
        Delivery::IMMEDIATE,
        Direction::SERVER_TO_CLIENT,
        PayloadContract::PLANNED
    );
    reg_c(
        CHANNEL_SESSION_KICKED,
        "SESSION_KICKED",
        ChannelKind::SESSION,
        Reliability::RELIABLE,
        Freshness::NONE,
        Delivery::IMMEDIATE,
        Direction::SERVER_TO_CLIENT,
        PayloadContract::PLANNED
    );
    reg_c(
        CHANNEL_SESSION_SHUTDOWN,
        "SESSION_SHUTDOWN",
        ChannelKind::SESSION,
        Reliability::RELIABLE,
        Freshness::NONE,
        Delivery::IMMEDIATE,
        Direction::SERVER_TO_CLIENT,
        PayloadContract::PLANNED
    );
    reg_c(
        CHANNEL_SESSION_SCENE_RELEASED,
        "SESSION_SCENE_RELEASED",
        ChannelKind::SESSION,
        Reliability::RELIABLE,
        Freshness::NONE,
        Delivery::IMMEDIATE,
        Direction::SERVER_TO_CLIENT,
        PayloadContract::PLANNED
    );
    reg_c(
        CHANNEL_SESSION_KICK_REQUEST,
        "SESSION_KICK_REQUEST",
        ChannelKind::SESSION,
        Reliability::RELIABLE,
        Freshness::NONE,
        Delivery::IMMEDIATE,
        Direction::CLIENT_TO_SERVER,
        PayloadContract::PLANNED,
        1
    );

    reg_c(
        CHANNEL_PREDICT_COMMAND,
        "PREDICT_COMMAND",
        ChannelKind::ROUTED,
        Reliability::UNRELIABLE,
        Freshness::FRESHEST_WINS,
        Delivery::TICK,
        Direction::OWNER_TO_SERVER,
        PayloadContract::PLANNED,
        4
    );
    reg_c(
        CHANNEL_PREDICT_ACK,
        "PREDICT_ACK",
        ChannelKind::ROUTED,
        Reliability::UNRELIABLE,
        Freshness::FRESHEST_WINS,
        Delivery::TICK,
        Direction::SERVER_TO_OWNER,
        PayloadContract::PLANNED,
        4
    );
    reg_c(
        CHANNEL_PREDICT_RELAY,
        "PREDICT_RELAY",
        ChannelKind::ROUTED,
        Reliability::UNRELIABLE,
        Freshness::FRESHEST_WINS,
        Delivery::TICK,
        Direction::SERVER_TO_CLIENT,
        PayloadContract::PLANNED,
        4
    );
    reg_c(
        CHANNEL_PREDICT_RELAY_REQUEST,
        "PREDICT_RELAY_REQUEST",
        ChannelKind::ROUTED,
        Reliability::RELIABLE,
        Freshness::NONE,
        Delivery::IMMEDIATE,
        Direction::CLIENT_TO_SERVER,
        PayloadContract::PLANNED
    );
    reg_c(
        CHANNEL_SYNC_ROW,
        "SYNC_ROW",
        ChannelKind::KEYED,
        Reliability::UNRELIABLE_ACKED,
        Freshness::FRESHEST_WINS,
        Delivery::TICK,
        Direction::EITHER,
        PayloadContract::DELTA,
        2
    );
    reg_c(
        CHANNEL_SYNC_ROW_DELTA,
        "SYNC_ROW_DELTA",
        ChannelKind::ROUTED,
        Reliability::RELIABLE,
        Freshness::NONE,
        Delivery::TICK,
        Direction::EITHER,
        PayloadContract::DELTA,
        2
    );

    reg_c(
        CHANNEL_SYNC_ROW_WINDOW,
        "SYNC_ROW_WINDOW",
        ChannelKind::KEYED,
        Reliability::UNRELIABLE,
        Freshness::FRESHEST_WINS,
        Delivery::TICK,
        Direction::EITHER,
        PayloadContract::PLANNED,
        2
    );

    reg_c(
        CHANNEL_SESSION_SCENE_VIEWERS,
        "SESSION_SCENE_VIEWERS",
        ChannelKind::SESSION,
        Reliability::RELIABLE,
        Freshness::NONE,
        Delivery::IMMEDIATE,
        Direction::SERVER_TO_CLIENT,
        PayloadContract::PLANNED,
        1
    );

    reg_c(
        CHANNEL_ROW_CONTROL,
        "ROW_CONTROL",
        ChannelKind::SESSION,
        Reliability::RELIABLE,
        Freshness::NONE,
        Delivery::TICK,
        Direction::EITHER,
        PayloadContract::PLANNED,
        5
    );

    reg_c(
        CHANNEL_ROUTE_LEASE,
        "ROUTE_LEASE",
        ChannelKind::SESSION,
        Reliability::RELIABLE,
        Freshness::NONE,
        Delivery::IMMEDIATE,
        Direction::SERVER_TO_CLIENT,
        PayloadContract::PLANNED
    );

    reg_c(
        CHANNEL_LIFECYCLE_DECISION,
        "LIFECYCLE_DECISION",
        ChannelKind::SESSION,
        Reliability::RELIABLE,
        Freshness::NONE,
        Delivery::TICK,
        Direction::SERVER_TO_OWNER,
        PayloadContract::PLANNED,
        2
    );

    reg_c(
        CHANNEL_CLAIM_IMAGE,
        "CLAIM_IMAGE",
        ChannelKind::ROUTED,
        Reliability::UNRELIABLE,
        Freshness::FRESHEST_WINS,
        Delivery::TICK,
        Direction::EITHER,
        PayloadContract::PLANNED
    );

    reg_c(
        CHANNEL_SESSION_RESYNC,
        "SESSION_RESYNC",
        ChannelKind::SESSION,
        Reliability::RELIABLE,
        Freshness::NONE,
        Delivery::IMMEDIATE,
        Direction::CLIENT_TO_SERVER,
        PayloadContract::PLANNED
    );

    reg_c(
        CHANNEL_LAGCOMP_CONFIRM,
        "LAGCOMP_CONFIRM",
        ChannelKind::SESSION,
        Reliability::RELIABLE,
        Freshness::NONE,
        Delivery::IMMEDIATE,
        Direction::SERVER_TO_CLIENT,
        PayloadContract::PLANNED
    );

    reg_c(
        CHANNEL_SESSION_LEAVE,
        "SESSION_LEAVE",
        ChannelKind::SESSION,
        Reliability::RELIABLE,
        Freshness::NONE,
        Delivery::IMMEDIATE,
        Direction::CLIENT_TO_SERVER,
        PayloadContract::PLANNED
    );

    reg_c(
        CHANNEL_SESSION_PLAYER_LEFT,
        "SESSION_PLAYER_LEFT",
        ChannelKind::SESSION,
        Reliability::RELIABLE,
        Freshness::NONE,
        Delivery::IMMEDIATE,
        Direction::SERVER_TO_CLIENT,
        PayloadContract::PLANNED
    );

    reg_c(
        CHANNEL_SESSION_SPAWN_FAILED,
        "SESSION_SPAWN_FAILED",
        ChannelKind::SESSION,
        Reliability::RELIABLE,
        Freshness::NONE,
        Delivery::IMMEDIATE,
        Direction::CLIENT_TO_SERVER,
        PayloadContract::PLANNED
    );

    auto spends = [&](Cause cause, Channel channel) {
        reg.channels[channel].cause = cause;
    };
    spends(Cause::STATE, CHANNEL_TABLE);
    spends(Cause::STATE, CHANNEL_SYNC);
    spends(Cause::STATE, CHANNEL_SYNC_DELTA);
    spends(Cause::STATE, CHANNEL_SYNC_ROW);
    spends(Cause::STATE, CHANNEL_SYNC_ROW_DELTA);
    spends(Cause::STATE, CHANNEL_PROPERTY_SYNC);
    spends(Cause::STATE, CHANNEL_CLAIM_IMAGE);
    spends(Cause::INPUT, CHANNEL_PREDICT_COMMAND);
    spends(Cause::INPUT, CHANNEL_PREDICT_RELAY);
    spends(Cause::INPUT, CHANNEL_SYNC_ROW_WINDOW);
    spends(Cause::CALLS, CHANNEL_ACTION);
    spends(Cause::CALLS, CHANNEL_CALL);
    spends(Cause::CALLS, CHANNEL_REPLY);
    spends(Cause::CALLS, CHANNEL_SIGNAL);
    spends(Cause::CALLS, CHANNEL_LAGCOMP_DENY);
    spends(Cause::CALLS, CHANNEL_LAGCOMP_CONFIRM);
    spends(Cause::UPKEEP, CHANNEL_PREDICT_ACK);
    spends(Cause::UPKEEP, CHANNEL_PREDICT_RELAY_REQUEST);
    spends(Cause::UPKEEP, CHANNEL_ROW_CONTROL);
    spends(Cause::UPKEEP, CHANNEL_CLOCK_HANDSHAKE);
    spends(Cause::UPKEEP, CHANNEL_CLOCK_HANDSHAKE_REPLY);
    spends(Cause::UPKEEP, CHANNEL_CLOCK_PING);
    spends(Cause::UPKEEP, CHANNEL_CLOCK_PONG);
    spends(Cause::UPKEEP, CHANNEL_SESSION_RESYNC);

    return reg;
}

} // namespace netw::wire
