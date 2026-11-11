#pragma once

#include <cstdint>

#include "godot/local_vector.hpp"
#include "godot/variant.hpp"

namespace netw {

class EventPlane {
public:
    enum Event : int64_t {
        SPAWNING = 0,
        SPAWNED = 1,
        DESPAWNING = 2,
        DESPAWNED = 3,
        REPARENTED = 4,
        STAGE_TRANSITION = 5,
        CONTROL_CHANGED = 6,
        CONTROL_REQUESTED = 7,
        HIDDEN = 8,

        INTEREST_ENTER = 16,
        INTEREST_EXIT = 17,
        OBSERVER_ENTERED = 18,
        OBSERVER_LEFT = 19,
        INTEREST_COMMIT = 20,

        EPISODE_OPEN = 32,
        EPISODE_CLOSE = 33,
        EPISODE_FALLBACK = 34,
        PREDICT_DRIVE = 35,
        PREDICT_CONSUME = 36,
        PREDICT_EVALUATE = 37,
        PREDICT_RECOVER = 38,
        DIVERGENCE = 39,
        RECOVERY = 40,

        GATE_SYNC = 48,
        GATE_SPAWN = 49,
        GATE_TABLE = 50,
        GATE_PREDICT = 51,

        SYNC_ENCODE = 64,
        SYNC_DECODE = 65,
        GATHER = 66,
        APPLY = 67,
        SPAWN_DECLARE = 68,
        SPAWN_RECONCILE = 69,
        SPAWN_CONSTRUCT = 70,
        DISPLAY_RECORD = 71,
        DISPLAY_PUMP = 72,
        DISPLAY_WRITE = 73,
        TABLE_COMMIT = 74,

        VERDICT = 96,

        PEER_JOINED = 113,
        PEER_LEFT = 114,
        PEER_AUTH_FAILED = 115,
        SCENE_LIVE = 116,

        DATAGRAM_SENT = 128,
        DATAGRAM_RECEIVED = 129,
        DATAGRAM_MALFORMED = 130,
        ACK_ADVANCED = 131,
    };

    static constexpr int64_t EVENT_LIMIT = 144;
    static constexpr int64_t ROW_WIDTH = 8;
    static constexpr uint32_t ROW_CAPACITY = 4096;

    struct Row {
        int64_t kind = 0;
        int64_t tick = 0;
        int64_t entity = 0;
        int64_t route = 0;
        int64_t peer = 0;
        int64_t verdict = 0;
        int64_t a = 0;
        int64_t b = 0;
    };
    static_assert(sizeof(Row) == sizeof(int64_t) * ROW_WIDTH);

    static const char *name_of(int64_t event);
    static int taxonomy_size();
    static int64_t value_at(int index);

    void arm(int64_t groups);
    bool records(int64_t kind) const {
        return kind >= 0 && kind < EVENT_LIMIT
            && (recorded_groups & (int64_t(1) << (kind >> 4))) != 0;
    }
    void keep(const Row &row);
    godot::PackedInt64Array drain();

private:
    int64_t recorded_groups = 0;
    godot::LocalVector<Row> kept;
    uint32_t kept_head = 0;
};

} // namespace netw
