#include "netw/api/event_plane.hpp"

#include <cstring>

using namespace godot;

namespace netw {

namespace {

struct Named {
    int64_t value;
    const char *name;
};

constexpr Named TAXONOMY[] = {
    {EventPlane::SPAWNING, "SPAWNING"},
    {EventPlane::SPAWNED, "SPAWNED"},
    {EventPlane::DESPAWNING, "DESPAWNING"},
    {EventPlane::DESPAWNED, "DESPAWNED"},
    {EventPlane::REPARENTED, "REPARENTED"},
    {EventPlane::STAGE_TRANSITION, "STAGE_TRANSITION"},
    {EventPlane::CONTROL_CHANGED, "CONTROL_CHANGED"},
    {EventPlane::CONTROL_REQUESTED, "CONTROL_REQUESTED"},
    {EventPlane::HIDDEN, "HIDDEN"},

    {EventPlane::INTEREST_ENTER, "INTEREST_ENTER"},
    {EventPlane::INTEREST_EXIT, "INTEREST_EXIT"},
    {EventPlane::OBSERVER_ENTERED, "OBSERVER_ENTERED"},
    {EventPlane::OBSERVER_LEFT, "OBSERVER_LEFT"},
    {EventPlane::INTEREST_COMMIT, "INTEREST_COMMIT"},

    {EventPlane::EPISODE_OPEN, "EPISODE_OPEN"},
    {EventPlane::EPISODE_CLOSE, "EPISODE_CLOSE"},
    {EventPlane::EPISODE_FALLBACK, "EPISODE_FALLBACK"},
    {EventPlane::PREDICT_DRIVE, "PREDICT_DRIVE"},
    {EventPlane::PREDICT_CONSUME, "PREDICT_CONSUME"},
    {EventPlane::PREDICT_EVALUATE, "PREDICT_EVALUATE"},
    {EventPlane::PREDICT_RECOVER, "PREDICT_RECOVER"},
    {EventPlane::DIVERGENCE, "DIVERGENCE"},
    {EventPlane::RECOVERY, "RECOVERY"},

    {EventPlane::GATE_SYNC, "GATE_SYNC"},
    {EventPlane::GATE_SPAWN, "GATE_SPAWN"},
    {EventPlane::GATE_TABLE, "GATE_TABLE"},
    {EventPlane::GATE_PREDICT, "GATE_PREDICT"},

    {EventPlane::SYNC_ENCODE, "SYNC_ENCODE"},
    {EventPlane::SYNC_DECODE, "SYNC_DECODE"},
    {EventPlane::GATHER, "GATHER"},
    {EventPlane::APPLY, "APPLY"},
    {EventPlane::SPAWN_DECLARE, "SPAWN_DECLARE"},
    {EventPlane::SPAWN_RECONCILE, "SPAWN_RECONCILE"},
    {EventPlane::SPAWN_CONSTRUCT, "SPAWN_CONSTRUCT"},
    {EventPlane::DISPLAY_RECORD, "DISPLAY_RECORD"},
    {EventPlane::DISPLAY_PUMP, "DISPLAY_PUMP"},
    {EventPlane::DISPLAY_WRITE, "DISPLAY_WRITE"},
    {EventPlane::TABLE_COMMIT, "TABLE_COMMIT"},

    {EventPlane::VERDICT, "VERDICT"},

    {EventPlane::PEER_JOINED, "PEER_JOINED"},
    {EventPlane::PEER_LEFT, "PEER_LEFT"},
    {EventPlane::PEER_AUTH_FAILED, "PEER_AUTH_FAILED"},
    {EventPlane::SCENE_LIVE, "SCENE_LIVE"},

    {EventPlane::DATAGRAM_SENT, "DATAGRAM_SENT"},
    {EventPlane::DATAGRAM_RECEIVED, "DATAGRAM_RECEIVED"},
    {EventPlane::DATAGRAM_MALFORMED, "DATAGRAM_MALFORMED"},
    {EventPlane::ACK_ADVANCED, "ACK_ADVANCED"},
};

constexpr int TAXONOMY_SIZE = int(sizeof(TAXONOMY) / sizeof(TAXONOMY[0]));

} // namespace

const char *EventPlane::name_of(int64_t p_event) {
    for (int index = 0; index < TAXONOMY_SIZE; index++) {
        if (TAXONOMY[index].value == p_event) {
            return TAXONOMY[index].name;
        }
    }
    return "";
}

int EventPlane::taxonomy_size() {
    return TAXONOMY_SIZE;
}

int64_t EventPlane::value_at(int p_index) {
    if (p_index < 0 || p_index >= TAXONOMY_SIZE) {
        return -1;
    }
    return TAXONOMY[p_index].value;
}

void EventPlane::arm(int64_t p_groups) {
    recorded_groups = p_groups;
}

void EventPlane::keep(const Row &p_row) {
    if (kept.size() < ROW_CAPACITY) {
        kept.push_back(p_row);
        return;
    }
    kept[kept_head] = p_row;
    kept_head = (kept_head + 1) % ROW_CAPACITY;
}

PackedInt64Array EventPlane::drain() {
    PackedInt64Array drained;
    const uint32_t size = kept.size();
    drained.resize(int64_t(size) * ROW_WIDTH);
    int64_t *out = drained.ptrw();
    for (uint32_t offset = 0; offset < size; offset++) {
        const Row &row = kept[(kept_head + offset) % size];
        memcpy(out + int64_t(offset) * ROW_WIDTH, &row, sizeof(Row));
    }
    kept.clear();
    kept_head = 0;
    return drained;
}

} // namespace netw
