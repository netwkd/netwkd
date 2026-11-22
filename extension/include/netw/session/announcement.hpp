#pragma once

#include <cstdint>

#include "godot/object.hpp"
#include "godot/rid.hpp"
#include "godot/variant.hpp"
#include "netw/api/promise.hpp"

namespace netw::session {

enum class AnnouncementKind : uint8_t {
    KICK_REQUESTED,
    TICKRATE_MISMATCH,
    ENTITY_ENTERED,
    ENTITY_EXITED,
    SCENE_REMOVED,
    SCENE_READY,
    SCENE_CHANGED,
    STATE_CHANGED,
    ENTERED,
    LOCAL_JOINED,
    PLAYER_JOINED,
    PLAYER_LEFT,
    JOIN_FAILED,
    PAUSED_CHANGED,
    SERVER_TRANSFERRED,
    VIEWER_ENTERED,
    VIEWER_EXITED,
    CONTROL_CHANGED,
    DESPAWNING,
    DESPAWNED,
    LIFECYCLE_REFUSED,
    REPARENTED,
    HIDDEN,
    MODE_CHANGED,
    SYNCHRONIZED,
    TABLE_RECEIVED,
    STATE_EVALUATED,
    EPISODE_CHANGED,
    RECOVERED,
    DIVERGENCE_DETECTED,
    CONNECTION_PROGRESSED,
    CONNECTION_FAILED,
    JOIN_ADDRESS_CHANGED,
    ENDPOINT_ADDED,
    ENDPOINT_REMOVED,
    INVITED,
    ENDPOINT_CHANGED,
    PERSIST_LOADED,
    PERSIST_SAVED,
    DATABASE_FAILED,
    ATTEMPT_SETTLED,
};

struct Announcement {
    AnnouncementKind kind = AnnouncementKind::KICK_REQUESTED;
    godot::RID scene;
    godot::RID scene_to;
    godot::RID entity;
    godot::RID table;
    godot::RID endpoint;
    godot::RID database;
    godot::ObjectID node;
    bool entity_is_player = false;
    int64_t player = 0;
    int64_t requester = 0;
    int64_t target = 0;
    int64_t server_tickrate = 0;
    int64_t code = 0;
    int64_t successor_was = 0;
    int64_t former_now = 0;
    int64_t from = 0;
    int64_t to = 0;
    double amount = 0.0;
    bool flag = false;
    bool paused = false;
    godot::String reason;
    godot::Error refusal = godot::OK;
    godot::Dictionary report;
    godot::Ref<NetwPromise> promise;
};

} // namespace netw::session
