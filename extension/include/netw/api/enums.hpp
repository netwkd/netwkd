#pragma once

#include "godot/variant.hpp"
#include "netw/api/member_config.hpp"
#include "netw/api/schema_core.hpp"
#include "netw/api/session_stats.hpp"
#include "netw/clock_engine.hpp"
#include "netw/connect/creation.hpp"
#include "netw/display/decl.hpp"
#include "netw/interest/engine.hpp"
#include "netw/liveness_core.hpp"
#include "netw/repl/set_model.hpp"
#include "netw/scene_core.hpp"
#include "netw/session_core.hpp"

namespace netw::enums {

struct NetwMultiplayer {
    enum TableParam {
        TABLE_PARAM_RELIABLE = 0,
    };

    enum EntityState {
        ENTITY_STATE_UNKNOWN = NetwLivenessCore::STATE_UNKNOWN,
        ENTITY_STATE_LIVE = NetwLivenessCore::STATE_LIVE,
        ENTITY_STATE_LINGERING = NetwLivenessCore::STATE_LINGERING,
        ENTITY_STATE_DEAD = NetwLivenessCore::STATE_DEAD,
        ENTITY_STATE_ABSENT = NetwLivenessCore::STATE_ABSENT,
    };

    enum EntityParam {
        ENTITY_PARAM_AUTHORITY = 0,
        ENTITY_PARAM_TRANSFER = 1,
        ENTITY_PARAM_ON_CONTROLLER_DISCONNECT = 2,
        ENTITY_PARAM_ON_PARENT_DESPAWN = 3,
        ENTITY_PARAM_HOLD = 4,
        ENTITY_PARAM_REPLICATION_PRIORITY = 5,
    };

    enum ControlState {
        CONTROL_STATE_SETTLED = 0,
        CONTROL_STATE_PENDING = 1,
        CONTROL_STATE_HELD = 2,
    };

    enum ClockParam {
        CLOCK_PARAM_TICKRATE = 0,
        CLOCK_PARAM_SYNC_MODE = 1,
        CLOCK_PARAM_PING_INTERVAL = 2,
        CLOCK_PARAM_MAX_TICKS_PER_FRAME = 3,
        CLOCK_PARAM_STALL_THRESHOLD = 4,
        CLOCK_PARAM_PANIC_SNAP_THRESHOLD = 5,
        CLOCK_PARAM_STRETCH_NUDGE_FACTOR = 6,
        CLOCK_PARAM_DISPLAY_OFFSET = 7,
        CLOCK_PARAM_LEAD_TICKS = 8,
        CLOCK_PARAM_JITTER_MULTIPLIER = 9,
        CLOCK_PARAM_JITTER_WINDOW = 10,
        CLOCK_PARAM_JITTER_STABILITY_THRESHOLD = 11,
        CLOCK_PARAM_USE_PHYSICS_INTERPOLATION = 12,
        CLOCK_PARAM_MANUAL_TICK = 14,
        CLOCK_PARAM_TICK_FACTOR_OVERRIDE = 15,
    };

    enum LayerParam {
        LAYER_PARAM_POLICY = 0,
        LAYER_PARAM_LEAVE_POLICY = 1,
        LAYER_PARAM_PERCEPTION_POLICY = 2,
    };

    enum EmbedPhase {
        EMBED_PHASE_DECLARING = 0,
        EMBED_PHASE_SETTLING = 1,
        EMBED_PHASE_LIVE = 2,
    };

    enum TransportParam {
        TRANSPORT_PARAM_PEER_CLASS = 0,
        TRANSPORT_PARAM_DISPLAY_NAME = 1,
        TRANSPORT_PARAM_ADDRESS_LABEL = 2,
        TRANSPORT_PARAM_ADDRESS_PLACEHOLDER = 3,
        TRANSPORT_PARAM_ADDRESS_HELP = 4,
        TRANSPORT_PARAM_CAPABILITIES = 5,
        TRANSPORT_PARAM_HOST_SETTINGS = 6,
        TRANSPORT_PARAM_CLIENT_SETTINGS = 7,
        TRANSPORT_PARAM_TIMEOUT_HINT = 8,
    };

    enum EndpointParam {
        ENDPOINT_PARAM_TRANSPORT = 0,
        ENDPOINT_PARAM_ADDRESS = 1,
        ENDPOINT_PARAM_DISPLAY_NAME = 2,
        ENDPOINT_PARAM_SOURCE = 3,
        ENDPOINT_PARAM_INVITED_BY = 4,
    };

    enum EndpointState {
        ENDPOINT_STATE_FLAGS = 0,
        ENDPOINT_STATE_STATUS = 1,
        ENDPOINT_STATE_INFO = 2,
    };

    enum EndpointFlags {
        ENDPOINT_FLAG_CALLER = 1,
        ENDPOINT_FLAG_AVAILABLE = 2,
        ENDPOINT_FLAG_OBSERVED = 4,
        ENDPOINT_FLAG_INVITED = 8,
    };

    enum SceneParam {
        SCENE_PARAM_LABEL = 0,
        SCENE_PARAM_ISOLATION = 1,
        SCENE_PARAM_PROCESSING = 2,
    };

    enum DisplayParam {
        DISPLAY_PARAM_ROLE = display::PARAM_ROLE,
        DISPLAY_PARAM_LIVE_MODE = display::PARAM_LIVE_MODE,
        DISPLAY_PARAM_LIVE_SMOOTH_TIME = display::PARAM_LIVE_SMOOTH_TIME,
        DISPLAY_PARAM_CHASE_GLIDE_TIME = display::PARAM_CHASE_GLIDE_TIME,
        DISPLAY_PARAM_TIMELINE_MODE = display::PARAM_TIMELINE_MODE,
        DISPLAY_PARAM_MAX_FORECAST_TICKS = display::PARAM_MAX_FORECAST_TICKS,
        DISPLAY_PARAM_SMART_DILATION = display::PARAM_SMART_DILATION,
        DISPLAY_PARAM_LAG_ADAPT_RATE = display::PARAM_LAG_ADAPT_RATE,
        DISPLAY_PARAM_FLOOR_SMOOTHING = display::PARAM_FLOOR_SMOOTHING,
        DISPLAY_PARAM_TRACE_INTERVAL = display::PARAM_TRACE_INTERVAL,
        DISPLAY_PARAM_VISUAL_ROOT = display::PARAM_VISUAL_ROOT,
    };

    enum PredictParam {
        PREDICT_PARAM_REPEAT_MISSING_INPUT = 2,
        PREDICT_PARAM_MODE = 3,
        PREDICT_PARAM_TELEPORT_THRESHOLD = 6,
        PREDICT_PARAM_CORRECTION_THRESHOLD = 7,
        PREDICT_PARAM_PAUSE_ON_UNPREDICTED_CONTACT = 8,
        PREDICT_PARAM_CORRECTION_COOLDOWN_TICKS = 10,
        PREDICT_PARAM_MAX_INPUTS_PER_TICK = 11,
        PREDICT_PARAM_MAX_INPUT_LAG_TICKS = 12,
        PREDICT_PARAM_INPUT_BUFFER_TICKS = 13,
        PREDICT_PARAM_ROLLBACK_TOGETHER = 15,
        PREDICT_PARAM_WORLD_VERSION = 17,
        PREDICT_PARAM_CORRECTION_COUNT = 19,
    };

    enum SimulationParam {
        SIMULATION_PARAM_BODIES = 0,
        SIMULATION_PARAM_SCHEDULE = 1,
        SIMULATION_PARAM_REPLICAS = 2,
        SIMULATION_PARAM_RESTORE = 3,
        SIMULATION_PARAM_MAX_RESTORE_TICKS = 4,
        SIMULATION_PARAM_CLAIM_ON_CONTACT = 5,
        SIMULATION_PARAM_RELEASE_ON_REST = 6,
        SIMULATION_PARAM_PHYSICS_DRIVEN = 7,
    };

    enum SimPick {
        SIM_PICK_CHOSEN = 0,
        SIM_PICK_FORGET = 1,
    };

    enum WritePolicy {
        WRITE_POLICY_AUTHORITY = NetwMemberConfig::POLICY_AUTHORITY,
        WRITE_POLICY_CONTROLLER = NetwMemberConfig::POLICY_CONTROLLER,
        WRITE_POLICY_ANY_PEER = NetwMemberConfig::POLICY_ANY_PEER,
        WRITE_POLICY_SERVER = NetwMemberConfig::POLICY_SERVER,
    };

    enum ColumnParam {
        COLUMN_PARAM_UNCHECKED = 0,
        COLUMN_PARAM_CORRECTION_THRESHOLD = 1,
        COLUMN_PARAM_TELEPORT_THRESHOLD = 2,
        COLUMN_PARAM_EXTRAPOLATE = 3,
        COLUMN_PARAM_TELEPORT_ONLY = 4,
        COLUMN_PARAM_LANE = 6,
        COLUMN_PARAM_CORRECTION_WEIGHT = 7,
    };

    enum PropertySetParam {
        SET_PARAM_MASKED = 0,
        SET_PARAM_WINDOW = 1,
        SET_PARAM_AUDIENCE = 2,
        SET_PARAM_POLICY = 3,
        SET_PARAM_TRIGGER = 4,
        SET_PARAM_CADENCE = 5,
        SET_PARAM_STAMP = 6,
        SET_PARAM_PROFILE = 7,
        SET_PARAM_CHANNEL = 8,
        SET_PARAM_RELIABLE = 9,
        SET_PARAM_HEARTBEAT = 10,
    };

    enum Stat {
#define NETW_SESSION_STAT_ENUM(m_name, m_key) STAT_##m_name,
        NETW_SESSION_STAT_TABLE(NETW_SESSION_STAT_ENUM)
#undef NETW_SESSION_STAT_ENUM
            STAT_COUNT,
    };
};

struct NetwSchema {
    enum ColumnType {
        COLUMN_F32 = SchemaCore::F32,
        COLUMN_F64 = SchemaCore::F64,
        COLUMN_I8 = SchemaCore::I8,
        COLUMN_U8 = SchemaCore::U8,
        COLUMN_I16 = SchemaCore::I16,
        COLUMN_U16 = SchemaCore::U16,
        COLUMN_I32 = SchemaCore::I32,
        COLUMN_I64 = SchemaCore::I64,
        COLUMN_BOOL = SchemaCore::BOOL,
        COLUMN_VECTOR2 = SchemaCore::VECTOR2,
        COLUMN_VECTOR3 = SchemaCore::VECTOR3,
        COLUMN_VECTOR4 = SchemaCore::VECTOR4,
        COLUMN_COLOR = SchemaCore::COLOR,
        COLUMN_QUATERNION = SchemaCore::QUATERNION,
        COLUMN_ENTITY = SchemaCore::ENTITY,
        COLUMN_VARIANT = SchemaCore::VARIANT,
        COLUMN_STRING = SchemaCore::STRING,
    };

    enum RecordKind {
        RECORD_KIND_STATE = repl::SET_RECORD_STATE,
        RECORD_KIND_INPUT = repl::SET_RECORD_INPUT,
        RECORD_KIND_BROADCAST = repl::SET_RECORD_BROADCAST,
    };
};

struct NetwDatabase {
    enum State {
        STATE_CLOSED = 0,
        STATE_OPENING = 1,
        STATE_OPEN = 2,
        STATE_CLOSING = 3,
    };
};

struct NetwSession {
    enum State {
        STATE_OFFLINE = SessionCore::STATE_OFFLINE,
        STATE_CREATING = connect::ATTEMPT_CREATING,
        STATE_CONNECTING = SessionCore::STATE_CONNECTING,
        STATE_ONLINE = SessionCore::STATE_ONLINE,
        STATE_DISCONNECTING = SessionCore::STATE_DISCONNECTING,
    };

    enum Role {
        ROLE_NONE = SessionCore::ROLE_NONE,
        ROLE_CLIENT = SessionCore::ROLE_CLIENT,
        ROLE_DEDICATED_SERVER = SessionCore::ROLE_DEDICATED_SERVER,
        ROLE_LISTEN_SERVER = SessionCore::ROLE_LISTEN_SERVER,
    };

    enum ExitReason {
        LOCAL_LEAVE,
        SERVER_SHUTDOWN,
        CONNECTION_LOST,
        KICKED,
        REPLACED,
    };

    enum TransferMode {
        TRANSFER_REJOIN,
        TRANSFER_CONTINUE,
    };
};

struct NetwClock {
    enum SyncMode {
        SYNC_MODE_SNAP = ClockEngine::SYNC_SNAP,
        SYNC_MODE_STRETCH = ClockEngine::SYNC_STRETCH,
    };

    enum MismatchAction {
        MISMATCH_ACTION_WARN = 0,
        MISMATCH_ACTION_DISCONNECT = 1,
        MISMATCH_ACTION_SIGNAL = 2,
    };

    enum ClockMonitor {
        CLOCK_MONITOR_RTT = 0,
        CLOCK_MONITOR_RTT_AVG = 1,
        CLOCK_MONITOR_RTT_JITTER = 2,
        CLOCK_MONITOR_ONE_WAY_LATENCY = 3,
        CLOCK_MONITOR_TICKTIME = 4,
        CLOCK_MONITOR_TICK_ACCUMULATOR = 5,
        CLOCK_MONITOR_PHYSICS_FRAMES = 6,
        CLOCK_MONITOR_POLLS = 7,
        CLOCK_MONITOR_WALL_SECONDS = 8,
        CLOCK_MONITOR_PHYSICS_HZ = 9,
        CLOCK_MONITOR_POLL_HZ = 10,
    };
};

struct NetwConnection {
    enum TransportMode {
        TRANSPORT_MODE_HOST = connect::PEER_MODE_HOST,
        TRANSPORT_MODE_CLIENT = connect::PEER_MODE_CLIENT,
    };

    enum TransportCapability {
        TRANSPORT_AVAILABLE = 1,
        TRANSPORT_CAN_HOST = 2,
        TRANSPORT_CAN_PROBE = 4,
        TRANSPORT_CAN_BROWSE = 8,
        TRANSPORT_ACCEPTS_EMPTY_ADDRESS = 16,
    };
};

struct NetwEndpoint {
    enum Status {
        STATUS_UNKNOWN = 0,
        STATUS_ONLINE = 1,
        STATUS_FULL = 2,
        STATUS_UNREACHABLE = 3,
        STATUS_INCOMPATIBLE = 4,
        STATUS_UNSUPPORTED = 5,
    };
};

struct NetwDisplay {
    enum DisplayRole {
        DISPLAY_ROLE_AUTO = display::ROLE_AUTO,
        DISPLAY_ROLE_REMOTE = display::ROLE_REMOTE,
        DISPLAY_ROLE_PREDICTED = display::ROLE_PREDICTED,
        DISPLAY_ROLE_DISABLED = display::ROLE_DISABLED,
        DISPLAY_ROLE_AUTHORITY = display::ROLE_AUTHORITY,
    };

    enum DisplayPump {
        DISPLAY_PUMP_UNRESOLVED = display::PUMP_UNRESOLVED,
        DISPLAY_PUMP_DISABLED = display::PUMP_DISABLED,
        DISPLAY_PUMP_REMOTE = display::PUMP_REMOTE,
        DISPLAY_PUMP_BRACKETED = display::PUMP_BRACKETED,
        DISPLAY_PUMP_CHASE = display::PUMP_CHASE,
    };

    enum LiveMode {
        LIVE_MODE_CHASE = display::LIVE_CHASE,
        LIVE_MODE_BRACKETED = display::LIVE_BRACKETED,
    };

    enum TimelineMode {
        TIMELINE_MODE_BUFFERED = display::TIMELINE_BUFFERED,
        TIMELINE_MODE_FORECAST = display::TIMELINE_FORECAST,
    };
};

struct NetwInterestLayer {
    enum LayerPolicy {
        LAYER_POLICY_HIDE_FROM_OUTSIDERS
        = interest::Engine::HIDE_FROM_OUTSIDERS,
        LAYER_POLICY_HIDE_FROM_INSIDERS = interest::Engine::HIDE_FROM_INSIDERS,
    };

    enum LeavePolicy : int64_t {
        LEAVE_POLICY_HIDE = interest::Decl::LEAVE_HIDE,
        LEAVE_POLICY_RETAIN = interest::Decl::LEAVE_RETAIN,
        LEAVE_POLICY_CUSTOM = interest::Decl::LEAVE_CUSTOM,
    };

    enum PerceptionPolicy : int64_t {
        PERCEPTION_POLICY_HIDE = interest::Decl::PERCEPTION_HIDE,
        PERCEPTION_POLICY_SHOW = interest::Decl::PERCEPTION_SHOW,
        PERCEPTION_POLICY_CUSTOM = interest::Decl::PERCEPTION_CUSTOM,
    };
};

struct NetwScene {
    enum SceneIsolation {
        SCENE_ISOLATION_NONE = NetwSceneCore::ISOLATION_NONE,
        SCENE_ISOLATION_OWN_WORLD = NetwSceneCore::ISOLATION_OWN_WORLD,
    };
};

struct NetwPlayer {
    enum LeaveReason {
        LEAVE_QUIT,
        LEAVE_STEPPED_AWAY,
        LEAVE_KICKED,
        LEAVE_DROPPED,
    };
};

struct LobbyDirectory {
    enum Capability {
        CAPABILITY_BROWSE = 1,
        CAPABILITY_FRIENDS_ONLY_SUPPORT = 2,
        CAPABILITY_INVITES = 4,
        CAPABILITY_FRIEND_NAMES = 8,
    };
};

} // namespace netw::enums

VARIANT_ENUM_CAST(netw::enums::NetwScene::SceneIsolation);
VARIANT_ENUM_CAST(netw::enums::NetwSchema::ColumnType);
VARIANT_ENUM_CAST(netw::enums::NetwSchema::RecordKind);
VARIANT_ENUM_CAST(netw::enums::NetwDatabase::State);
VARIANT_ENUM_CAST(netw::enums::NetwSession::State);
VARIANT_ENUM_CAST(netw::enums::NetwSession::Role);
VARIANT_ENUM_CAST(netw::enums::NetwSession::ExitReason);
VARIANT_ENUM_CAST(netw::enums::NetwSession::TransferMode);
VARIANT_ENUM_CAST(netw::enums::NetwPlayer::LeaveReason);
VARIANT_ENUM_CAST(netw::enums::NetwClock::SyncMode);
VARIANT_ENUM_CAST(netw::enums::NetwClock::MismatchAction);
VARIANT_ENUM_CAST(netw::enums::NetwClock::ClockMonitor);
VARIANT_ENUM_CAST(netw::enums::NetwConnection::TransportMode);
VARIANT_ENUM_CAST(netw::enums::NetwConnection::TransportCapability);
VARIANT_ENUM_CAST(netw::enums::NetwEndpoint::Status);
VARIANT_ENUM_CAST(netw::enums::NetwDisplay::DisplayRole);
VARIANT_ENUM_CAST(netw::enums::NetwDisplay::DisplayPump);
VARIANT_ENUM_CAST(netw::enums::NetwDisplay::LiveMode);
VARIANT_ENUM_CAST(netw::enums::NetwDisplay::TimelineMode);
VARIANT_ENUM_CAST(netw::enums::NetwInterestLayer::LayerPolicy);
VARIANT_ENUM_CAST(netw::enums::NetwInterestLayer::LeavePolicy);
VARIANT_ENUM_CAST(netw::enums::NetwInterestLayer::PerceptionPolicy);
VARIANT_ENUM_CAST(netw::enums::LobbyDirectory::Capability);
VARIANT_ENUM_CAST(netw::enums::NetwMultiplayer::TableParam);
VARIANT_ENUM_CAST(netw::enums::NetwMultiplayer::EntityState);
VARIANT_ENUM_CAST(netw::enums::NetwMultiplayer::EntityParam);
VARIANT_ENUM_CAST(netw::enums::NetwMultiplayer::ControlState);
VARIANT_ENUM_CAST(netw::enums::NetwMultiplayer::ClockParam);
VARIANT_ENUM_CAST(netw::enums::NetwMultiplayer::LayerParam);
VARIANT_ENUM_CAST(netw::enums::NetwMultiplayer::TransportParam);
VARIANT_ENUM_CAST(netw::enums::NetwMultiplayer::EndpointParam);
VARIANT_ENUM_CAST(netw::enums::NetwMultiplayer::EndpointState);
VARIANT_ENUM_CAST(netw::enums::NetwMultiplayer::EndpointFlags);
VARIANT_ENUM_CAST(netw::enums::NetwMultiplayer::SceneParam);
VARIANT_ENUM_CAST(netw::enums::NetwMultiplayer::DisplayParam);
VARIANT_ENUM_CAST(netw::enums::NetwMultiplayer::PredictParam);
VARIANT_ENUM_CAST(netw::enums::NetwMultiplayer::SimulationParam);
VARIANT_ENUM_CAST(netw::enums::NetwMultiplayer::SimPick);
VARIANT_ENUM_CAST(netw::enums::NetwMultiplayer::WritePolicy);
VARIANT_ENUM_CAST(netw::enums::NetwMultiplayer::ColumnParam);
VARIANT_ENUM_CAST(netw::enums::NetwMultiplayer::PropertySetParam);
VARIANT_ENUM_CAST(netw::enums::NetwMultiplayer::Stat);
