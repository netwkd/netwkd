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

inline constexpr uint16_t FORMAT_VERSION = 22;

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
    const ChannelDecl *find_channel_by_name(
        const godot::StringName &name
    ) const;

    Delivery delivery_of(uint8_t id) const;
    Delivery delivery_of(uint8_t id, Delivery requested) const;
    Cause cause_of(uint8_t id) const;

    static bool id_is_builtin(uint8_t id);

    uint64_t identity_hash() const;
    uint32_t active_count() const;
};

int64_t builtin_channel(const godot::StringName &name);

} // namespace netw::wire
