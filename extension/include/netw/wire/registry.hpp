#pragma once

#include <cstdint>

#include "godot/variant.hpp"

namespace netw::wire {

enum class ChannelKind : uint8_t {
    KEYED = 0,
    SESSION = 1,
    ROUTED = 2,
};

enum class Reliability : uint8_t {
    UNRELIABLE = 0,
    UNRELIABLE_ACKED = 1,
    RELIABLE = 2,
};

enum class Freshness : uint8_t {
    NONE = 0,
    FRESHEST_WINS = 1,
};

enum class Delivery : uint8_t {
    IMMEDIATE = 0,
    TICK = 1,
    RIDE = 2,
    WAIT = 3,
};

inline constexpr int64_t RIDE_DEADLINE_MS = 250;
inline constexpr int64_t WAIT_DEADLINE_MS = 1500;

inline constexpr int64_t ride_deadline_ms(Delivery p_delivery) {
    return p_delivery == Delivery::RIDE ? RIDE_DEADLINE_MS
        : p_delivery == Delivery::WAIT  ? WAIT_DEADLINE_MS
                                        : 0;
}

enum class Direction : uint8_t {
    EITHER = 0,
    SERVER_TO_CLIENT = 1,
    CLIENT_TO_SERVER = 2,
    OWNER_TO_SERVER = 3,
    SERVER_TO_OWNER = 4,
};

enum class PayloadContract : uint8_t {
    RAW = 0,
    PLANNED = 1,
    DELTA = 2,
};

enum class Cause : uint8_t {
    STATE = 0,
    INPUT = 1,
    CALLS = 2,
    SPAWNS = 3,
    UPKEEP = 4,
};

inline constexpr int CAUSE_COUNT = 5;
inline constexpr uint8_t USER_CHANNEL_FIRST = 100;

inline constexpr uint16_t FORMAT_VERSION = 24;

enum Channel : uint8_t {
    CHANNEL_ACTION = 2,
    CHANNEL_CALL = 3,
    CHANNEL_REPLY = 4,
    CHANNEL_SIGNAL = 5,
    CHANNEL_PROPERTY_SYNC = 6,
    CHANNEL_INTEREST_AWARENESS = 8,
    CHANNEL_CLOCK_HANDSHAKE = 9,
    CHANNEL_CLOCK_HANDSHAKE_REPLY = 10,
    CHANNEL_CLOCK_PING = 11,
    CHANNEL_CLOCK_PONG = 12,
    CHANNEL_LAGCOMP_DENY = 13,
    CHANNEL_SPAWN = 14,
    CHANNEL_DESPAWN = 15,
    CHANNEL_REPARENT = 16,
    CHANNEL_TABLE = 17,
    CHANNEL_HIDE = 18,
    CHANNEL_SYNC = 19,
    CHANNEL_SYNC_DELTA = 20,
    CHANNEL_CONTROL_REQUEST = 21,
    CHANNEL_CONTROL_APPLY = 22,
    CHANNEL_SESSION_JOIN = 23,
    CHANNEL_SESSION_ACCEPT = 24,
    CHANNEL_SESSION_ROSTER = 25,
    CHANNEL_SESSION_PAUSE = 26,
    CHANNEL_SESSION_UNPAUSE = 27,
    CHANNEL_SESSION_KICKED = 28,
    CHANNEL_SESSION_SHUTDOWN = 31,
    CHANNEL_SESSION_SCENE_RELEASED = 32,
    CHANNEL_SESSION_KICK_REQUEST = 33,
    CHANNEL_PREDICT_COMMAND = 35,
    CHANNEL_PREDICT_ACK = 36,
    CHANNEL_PREDICT_RELAY = 37,
    CHANNEL_PREDICT_RELAY_REQUEST = 38,
    CHANNEL_SYNC_ROW = 39,
    CHANNEL_SYNC_ROW_DELTA = 40,
    CHANNEL_SYNC_ROW_WINDOW = 41,
    CHANNEL_SESSION_SCENE_VIEWERS = 42,
    CHANNEL_ROW_CONTROL = 43,
    CHANNEL_ROUTE_LEASE = 44,
    CHANNEL_LIFECYCLE_DECISION = 45,
    CHANNEL_CLAIM_IMAGE = 46,
    CHANNEL_SESSION_RESYNC = 47,
    CHANNEL_LAGCOMP_CONFIRM = 48,
    CHANNEL_SESSION_LEAVE = 49,
    CHANNEL_SESSION_PLAYER_LEFT = 50,
    CHANNEL_SESSION_SPAWN_FAILED = 51,
};

struct ChannelDecl {
    uint8_t id = 0;
    godot::StringName name;
    ChannelKind kind = ChannelKind::SESSION;
    Reliability reliability = Reliability::RELIABLE;
    Freshness freshness = Freshness::NONE;
    Delivery delivery = Delivery::TICK;
    Direction direction = Direction::EITHER;
    PayloadContract payload = PayloadContract::PLANNED;
    uint16_t payload_revision = 0;
    int32_t cap_bytes = 0;
    float priority = 1.0f;
    bool is_reserved = false;
    Cause cause = Cause::SPAWNS;

    bool valid() const {
        return id > 0 && !is_reserved;
    }
};

class WireRegistry {
public:
    static constexpr int MAX_CHANNELS = 256;

private:
    ChannelDecl channels[MAX_CHANNELS];
    bool registered[MAX_CHANNELS] = {false};

public:
    static WireRegistry create_default();

    bool register_channel(const ChannelDecl &decl);
    const ChannelDecl *find_channel(uint8_t id) const;

    Delivery delivery_of(uint8_t id) const;
    Delivery delivery_of(uint8_t id, Delivery requested) const;
    Cause cause_of(uint8_t id) const;

    static bool id_is_builtin(uint8_t id);

    uint64_t identity_hash() const;
    uint32_t active_count() const;
};

} // namespace netw::wire
