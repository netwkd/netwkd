#include "netw/api/interest.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/scene.hpp"

#include "netw/api/participant.hpp"

#include "godot/class_db.hpp"
#include "godot/node.hpp"
#include "godot/object.hpp"
#include "godot/os.hpp"
#include "godot/packed_scene.hpp"
#include "godot/physics_server.hpp"
#include "godot/rendering_server.hpp"
#include "godot/resource.hpp"
#include "godot/scene_tree.hpp"
#include "godot/spatial_node.hpp"
#include "godot/time.hpp"
#include "godot/utility.hpp"
#include "godot/viewport.hpp"
#include "godot/world.hpp"
#include "netw/api/action_config.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/join_config.hpp"
#include "netw/api/join_request.hpp"
#include "netw/api/prediction.hpp"
#include "netw/api/replication_core.hpp"
#include "netw/api/schema_model.hpp"
#include "netw/colors.hpp"
#include "netw/comp_table.hpp"
#include "netw/log.hpp"
#include "netw/prediction_core.hpp"
#include "netw/profile.hpp"
#include "netw/repl/sync_pipeline.hpp"
#include "netw/script/model.hpp"
#include "netw/synchronizers.hpp"
#include "netw/wire/frame.hpp"

using namespace godot;
using namespace netw;

namespace netw {

void NetwMultiplayer::_bind_methods() {
    ClassDB::bind_method(
        D_METHOD("entity_get_recipients", "entity"),
        &NetwMultiplayer::entity_get_recipients
    );
    ClassDB::bind_method(
        D_METHOD("clock_auto_configure_offset"),
        &NetwMultiplayer::clock_auto_configure_offset
    );
    ClassDB::bind_method(
        D_METHOD("rpc_call", "callable", "args", "peer"),
        &NetwMultiplayer::rpc_call
    );
    ClassDB::bind_method(
        D_METHOD(
            "rpc_request_call",
            "peer",
            "callable",
            "args",
            "timeout_seconds"
        ),
        &NetwMultiplayer::rpc_request_call,
        DEFVAL(0.0)
    );
    ClassDB::bind_method(
        D_METHOD(
            "rpc_request_call_group",
            "callable",
            "args",
            "timeout_seconds"
        ),
        &NetwMultiplayer::rpc_request_call_group,
        DEFVAL(0.0)
    );
    ClassDB::bind_method(
        D_METHOD("session_get_stats"),
        &NetwMultiplayer::session_get_stats
    );
    ClassDB::bind_method(
        D_METHOD("stats_get", "stat"),
        &NetwMultiplayer::stats_get
    );
    ClassDB::bind_method(
        D_METHOD("interest_layer_find", "name"),
        &NetwMultiplayer::interest_layer_find
    );
    ClassDB::bind_method(
        D_METHOD("interest_layer_set_monitor_callback", "layer", "callback"),
        &NetwMultiplayer::interest_layer_set_monitor_callback
    );
    ClassDB::bind_method(
        D_METHOD("interest_is_filtered", "entity"),
        &NetwMultiplayer::interest_is_filtered
    );
    ClassDB::bind_method(
        D_METHOD("session_get_state"),
        &NetwMultiplayer::session_get_state
    );
    ClassDB::bind_method(
        D_METHOD("session_get_role"),
        &NetwMultiplayer::session_get_role
    );
    ClassDB::bind_method(
        D_METHOD("session_is_online"),
        &NetwMultiplayer::session_is_online
    );
    ClassDB::bind_method(
        D_METHOD("session_is_local_client"),
        &NetwMultiplayer::session_is_local_client
    );
    ClassDB::bind_method(
        D_METHOD("session_get_root"),
        &NetwMultiplayer::session_get_root
    );
    ClassDB::bind_method(D_METHOD("clear"), &NetwMultiplayer::clear);
    ClassDB::bind_method(
        D_METHOD("endpoint_add", "transport", "address", "display_name"),
        &NetwMultiplayer::endpoint_add,
        DEFVAL(String())
    );
    ClassDB::bind_method(
        D_METHOD("endpoint_invite", "transport", "address", "from_name"),
        &NetwMultiplayer::endpoint_invite,
        DEFVAL(String())
    );
    ClassDB::bind_method(
        D_METHOD("endpoint_remove", "endpoint"),
        &NetwMultiplayer::endpoint_remove
    );
    ClassDB::bind_method(
        D_METHOD("endpoint_find", "transport", "address"),
        &NetwMultiplayer::endpoint_find
    );
    ClassDB::bind_method(
        D_METHOD("endpoint_list"),
        &NetwMultiplayer::endpoint_list
    );
    ClassDB::bind_method(
        D_METHOD("endpoint_get_current"),
        &NetwMultiplayer::endpoint_get_current
    );
    ClassDB::bind_method(
        D_METHOD("endpoint_get_param", "endpoint", "param"),
        &NetwMultiplayer::endpoint_get_param
    );
    ClassDB::bind_method(
        D_METHOD("endpoint_get_state", "endpoint", "state"),
        &NetwMultiplayer::endpoint_get_state
    );
    ClassDB::bind_method(
        D_METHOD("endpoint_probe", "endpoint"),
        &NetwMultiplayer::endpoint_probe
    );
    ClassDB::bind_method(
        D_METHOD("endpoint_set_browsing", "enabled"),
        &NetwMultiplayer::endpoint_set_browsing
    );
    ClassDB::bind_method(
        D_METHOD("endpoint_is_browsing"),
        &NetwMultiplayer::endpoint_is_browsing
    );
    ClassDB::bind_method(
        D_METHOD("endpoint_set_probe_interval", "seconds"),
        &NetwMultiplayer::endpoint_set_probe_interval
    );
    ClassDB::bind_method(
        D_METHOD("endpoint_get_probe_interval"),
        &NetwMultiplayer::endpoint_get_probe_interval
    );
    ClassDB::bind_method(
        D_METHOD(
            "transport_create_peer",
            "transport",
            "mode",
            "address",
            "settings",
            "completed"
        ),
        &NetwMultiplayer::transport_create_peer
    );
    ClassDB::bind_method(
        D_METHOD("transport_cancel_peer_creation", "ticket"),
        &NetwMultiplayer::transport_cancel_peer_creation
    );
    ClassDB::bind_method(
        D_METHOD("endpoint_join", "endpoint", "settings"),
        &NetwMultiplayer::endpoint_join,
        DEFVAL(Dictionary())
    );
    ClassDB::bind_method(
        D_METHOD("transport_host", "transport", "settings"),
        &NetwMultiplayer::transport_host,
        DEFVAL(Dictionary())
    );
    ClassDB::bind_method(
        D_METHOD("transport_cancel_attempt"),
        &NetwMultiplayer::transport_cancel_attempt
    );
    ClassDB::bind_method(
        D_METHOD("transport_set_defaults", "transport", "settings"),
        &NetwMultiplayer::transport_set_defaults
    );
    ClassDB::bind_method(
        D_METHOD("transport_get_defaults", "transport"),
        &NetwMultiplayer::transport_get_defaults
    );
    ClassDB::bind_method(
        D_METHOD("transport_register", "type"),
        &NetwMultiplayer::transport_register
    );
    ClassDB::bind_method(
        D_METHOD("transport_unregister", "transport"),
        &NetwMultiplayer::transport_unregister
    );
    ClassDB::bind_method(
        D_METHOD("transport_list"),
        &NetwMultiplayer::transport_list
    );
    ClassDB::bind_method(
        D_METHOD("transport_find", "peer_class"),
        &NetwMultiplayer::transport_find
    );
    ClassDB::bind_method(
        D_METHOD("transport_find_script", "type"),
        &NetwMultiplayer::transport_find_script
    );
    ClassDB::bind_method(
        D_METHOD("transport_get_param", "transport", "param"),
        &NetwMultiplayer::transport_get_param
    );
    ClassDB::bind_method(
        D_METHOD("transport_get_join_address"),
        &NetwMultiplayer::transport_get_join_address
    );
    ClassDB::bind_method(
        D_METHOD("transport_set_join_address", "address"),
        &NetwMultiplayer::transport_set_join_address
    );
    ClassDB::bind_method(
        D_METHOD("transport_get_max_players"),
        &NetwMultiplayer::transport_get_max_players
    );
    ClassDB::bind_method(
        D_METHOD("transport_get_peer_diagnostics", "peer"),
        &NetwMultiplayer::transport_get_peer_diagnostics
    );
    ClassDB::bind_method(
        D_METHOD("transport_report_progress", "ticket", "message", "ratio"),
        &NetwMultiplayer::transport_report_progress,
        DEFVAL(-1.0)
    );
    ClassDB::bind_method(
        D_METHOD("transport_complete", "ticket", "error", "result", "message"),
        &NetwMultiplayer::transport_complete,
        DEFVAL(Variant()),
        DEFVAL(String())
    );
    ClassDB::bind_method(
        D_METHOD("transport_publish_targets", "transport", "targets"),
        &NetwMultiplayer::transport_publish_targets
    );
    ClassDB::bind_method(
        D_METHOD("session_get_join_schema"),
        &NetwMultiplayer::session_get_join_schema
    );
    ClassDB::bind_method(
        D_METHOD("session_get_config"),
        &NetwMultiplayer::session_get_config
    );
    ClassDB::bind_method(
        D_METHOD("session_set_server_info", "info"),
        &NetwMultiplayer::session_set_server_info
    );
    ClassDB::bind_method(
        D_METHOD("session_get_server_info"),
        &NetwMultiplayer::session_get_server_info
    );
    ClassDB::bind_method(
        D_METHOD("session_get_app_id"),
        &NetwMultiplayer::session_get_app_id
    );
    ClassDB::bind_method(
        D_METHOD("session_step_away"),
        &NetwMultiplayer::session_step_away
    );
    ClassDB::bind_method(
        D_METHOD("session_transfer_server", "player"),
        &NetwMultiplayer::session_transfer_server
    );
    ClassDB::bind_method(
        D_METHOD("session_pause", "reason"),
        &NetwMultiplayer::session_pause,
        DEFVAL(String())
    );
    ClassDB::bind_method(
        D_METHOD("session_unpause"),
        &NetwMultiplayer::session_unpause
    );
    ClassDB::bind_method(
        D_METHOD("session_is_paused"),
        &NetwMultiplayer::session_is_paused
    );
    ClassDB::bind_method(
        D_METHOD(
            "rpc_send",
            "peer",
            "channel",
            "payload",
            "reliable",
            "batched"
        ),
        &NetwMultiplayer::rpc_send,
        DEFVAL(true),
        DEFVAL(false)
    );
    ClassDB::bind_method(
        D_METHOD("database_find", "name"),
        &NetwMultiplayer::database_find
    );
    ClassDB::bind_method(
        D_METHOD("database_open", "database", "slot"),
        &NetwMultiplayer::database_open
    );
    ClassDB::bind_method(
        D_METHOD("database_close", "database"),
        &NetwMultiplayer::database_close
    );
    ClassDB::bind_method(
        D_METHOD("database_flush", "database"),
        &NetwMultiplayer::database_flush
    );
    ClassDB::bind_method(
        D_METHOD("database_read", "database", "schema", "id"),
        &NetwMultiplayer::database_read
    );
    ClassDB::bind_method(
        D_METHOD("database_write", "database", "schema", "id", "values"),
        &NetwMultiplayer::database_write
    );
    ClassDB::bind_method(
        D_METHOD("database_patch", "database", "schema", "id", "values"),
        &NetwMultiplayer::database_patch
    );
    ClassDB::bind_method(
        D_METHOD("database_erase", "database", "schema", "id"),
        &NetwMultiplayer::database_erase
    );
    ClassDB::bind_method(
        D_METHOD(
            "database_scan",
            "database",
            "schema",
            "filter",
            "cursor",
            "limit"
        ),
        &NetwMultiplayer::database_scan
    );
    ClassDB::bind_method(
        D_METHOD("database_submit", "database", "operations"),
        &NetwMultiplayer::database_submit
    );
    ClassDB::bind_method(
        D_METHOD("database_list_slots", "database"),
        &NetwMultiplayer::database_list_slots
    );
    ClassDB::bind_method(
        D_METHOD("database_delete_slot", "database", "slot"),
        &NetwMultiplayer::database_delete_slot
    );
    ClassDB::bind_method(
        D_METHOD("database_get_slot", "database"),
        &NetwMultiplayer::database_get_slot
    );
    ClassDB::bind_method(
        D_METHOD("database_get_name", "database"),
        &NetwMultiplayer::database_get_name
    );
    ClassDB::bind_method(
        D_METHOD("database_get_state", "database"),
        &NetwMultiplayer::database_get_state
    );
    ClassDB::bind_method(
        D_METHOD("database_is_valid", "database"),
        &NetwMultiplayer::database_is_valid
    );
    ClassDB::bind_method(
        D_METHOD("persist_load", "entity"),
        &NetwMultiplayer::persist_load
    );
    ClassDB::bind_method(
        D_METHOD("persist_save", "entity"),
        &NetwMultiplayer::persist_save
    );
    ClassDB::bind_method(
        D_METHOD("persist_is_dirty", "entity"),
        &NetwMultiplayer::persist_is_dirty
    );
    ClassDB::bind_method(
        D_METHOD("persist_get_record_id", "entity"),
        &NetwMultiplayer::persist_get_record_id
    );
    ClassDB::bind_method(
        D_METHOD("persist_get_database", "entity"),
        &NetwMultiplayer::persist_get_database
    );
    ClassDB::bind_method(
        D_METHOD("persist_get_schema", "entity"),
        &NetwMultiplayer::persist_get_schema
    );
    ClassDB::bind_method(
        D_METHOD("persist_flush_all"),
        &NetwMultiplayer::persist_flush_all
    );
    ClassDB::bind_method(
        D_METHOD("table_save", "table", "database", "key", "ids"),
        &NetwMultiplayer::table_save
    );
    ClassDB::bind_method(
        D_METHOD("table_load", "table", "database", "key"),
        &NetwMultiplayer::table_load
    );
    ClassDB::bind_method(
        D_METHOD("peer_get_link_stats", "peer"),
        &NetwMultiplayer::peer_get_link_stats
    );
    ClassDB::bind_method(
        D_METHOD("session_get_traffic", "peer"),
        &NetwMultiplayer::session_get_traffic
    );
    ClassDB::bind_method(
        D_METHOD("peer_set_bandwidth", "peer", "kbps"),
        &NetwMultiplayer::peer_set_bandwidth
    );
    ClassDB::bind_method(
        D_METHOD("schema_declare", "schema"),
        &NetwMultiplayer::schema_declare
    );
    ClassDB::bind_method(
        D_METHOD("schema_find", "name"),
        &NetwMultiplayer::schema_find
    );
    ClassDB::bind_method(
        D_METHOD("table_find_or_adopt", "name"),
        &NetwMultiplayer::table_find_or_adopt
    );
    ClassDB::bind_method(
        D_METHOD("schema_get_name", "schema"),
        &NetwMultiplayer::schema_get_name
    );
    ClassDB::bind_method(
        D_METHOD("lagcomp_effect_arm", "key", "revert", "timeout_ticks"),
        &NetwMultiplayer::lagcomp_effect_arm,
        DEFVAL(0)
    );
    ClassDB::bind_method(
        D_METHOD("lagcomp_effect_watch", "key", "confirmed", "denied"),
        &NetwMultiplayer::lagcomp_effect_watch
    );
    ClassDB::bind_method(
        D_METHOD("event_arm", "groups"),
        &NetwMultiplayer::event_arm
    );
    ClassDB::bind_method(
        D_METHOD("event_drain"),
        &NetwMultiplayer::event_drain
    );
    for (int index = 0; index < EventPlane::taxonomy_size(); index++) {
        const int64_t value = EventPlane::value_at(index);
        ClassDB::bind_integer_constant(
            get_class_static(),
            StringName("Event"),
            StringName(String("EVENT_") + String(EventPlane::name_of(value))),
            value
        );
    }
    BIND_BITFIELD_FLAG(EVENT_GROUP_LIFECYCLE);
    BIND_BITFIELD_FLAG(EVENT_GROUP_INTEREST);
    BIND_BITFIELD_FLAG(EVENT_GROUP_PREDICTION);
    BIND_BITFIELD_FLAG(EVENT_GROUP_GATE);
    BIND_BITFIELD_FLAG(EVENT_GROUP_STAGE);
    BIND_BITFIELD_FLAG(EVENT_GROUP_VERDICT);
    BIND_BITFIELD_FLAG(EVENT_GROUP_SESSION);
    BIND_BITFIELD_FLAG(EVENT_GROUP_CARRIER);
    ClassDB::bind_integer_constant(
        get_class_static(),
        StringName(),
        StringName("EVENT_ROW_WIDTH"),
        EVENT_ROW_WIDTH
    );
    ClassDB::bind_method(
        D_METHOD("table_get_schema", "table"),
        &NetwMultiplayer::table_get_schema
    );
    ClassDB::bind_method(
        D_METHOD("table_set_param", "table", "param", "value"),
        &NetwMultiplayer::table_set_param
    );
    ClassDB::bind_method(
        D_METHOD("table_get_param", "table", "param"),
        &NetwMultiplayer::table_get_param
    );
    ClassDB::bind_method(
        D_METHOD("table_claim_routes", "table", "count"),
        &NetwMultiplayer::table_claim_routes
    );
    ClassDB::bind_method(
        D_METHOD("table_release_routes", "table", "routes"),
        &NetwMultiplayer::table_release_routes
    );
    ClassDB::bind_method(
        D_METHOD("table_get_wire_hash", "table"),
        &NetwMultiplayer::table_get_wire_hash
    );
    ClassDB::bind_method(
        D_METHOD("table_write_routes", "table", "routes"),
        &NetwMultiplayer::table_write_routes
    );
    ClassDB::bind_method(
        D_METHOD("table_write_column", "table", "column", "data"),
        &NetwMultiplayer::table_write_column
    );
    ClassDB::bind_method(
        D_METHOD("table_commit", "table"),
        &NetwMultiplayer::table_commit
    );
    ClassDB::bind_method(
        D_METHOD("table_read_routes", "table"),
        &NetwMultiplayer::table_read_routes
    );
    ClassDB::bind_method(
        D_METHOD("table_read_column", "table", "column"),
        &NetwMultiplayer::table_read_column
    );
    ClassDB::bind_method(
        D_METHOD("table_read_births", "table"),
        &NetwMultiplayer::table_read_births
    );
    ClassDB::bind_method(
        D_METHOD("table_read_deaths", "table"),
        &NetwMultiplayer::table_read_deaths
    );
    ClassDB::bind_method(
        D_METHOD("table_get_row", "table", "route"),
        &NetwMultiplayer::table_get_row
    );
    ClassDB::bind_method(
        D_METHOD("table_get_rows", "table", "routes"),
        &NetwMultiplayer::table_get_rows
    );
    ClassDB::bind_method(
        D_METHOD("table_get_tick", "table"),
        &NetwMultiplayer::table_get_tick
    );
    BIND_ENUM_CONSTANT(TABLE_PARAM_RELIABLE);
    ClassDB::bind_method(
        D_METHOD("service_register", "service", "type"),
        &NetwMultiplayer::service_register,
        DEFVAL(static_cast<Object *>(nullptr))
    );
    ClassDB::bind_method(
        D_METHOD("service_unregister", "service", "type"),
        &NetwMultiplayer::service_unregister,
        DEFVAL(static_cast<Object *>(nullptr))
    );
    ClassDB::bind_method(
        D_METHOD("service_get", "type"),
        &NetwMultiplayer::service_get
    );
    ClassDB::bind_static_method(
        "NetwMultiplayer",
        D_METHOD("session_of", "node"),
        &NetwMultiplayer::session_of,
        DEFVAL(Variant())
    );
    ClassDB::bind_method(
        D_METHOD(
            "spawn_register_constructor",
            "id",
            "function",
            "arg_types",
            "quantizers"
        ),
        &NetwMultiplayer::spawn_register_constructor,
        DEFVAL(Array()),
        DEFVAL(Array())
    );
    ClassDB::bind_method(
        D_METHOD("spawn_registered", "id", "args", "player"),
        &NetwMultiplayer::spawn_registered,
        DEFVAL(Array()),
        DEFVAL(0)
    );
    ClassDB::bind_method(
        D_METHOD("spawn_call", "function", "args", "player"),
        &NetwMultiplayer::spawn_call,
        DEFVAL(Array()),
        DEFVAL(0)
    );
    ClassDB::bind_method(
        D_METHOD("spawn_call_async", "function", "args"),
        &NetwMultiplayer::spawn_call_async,
        DEFVAL(Array())
    );
    ClassDB::bind_method(
        D_METHOD("sync_send_property", "entity", "node", "property"),
        &NetwMultiplayer::sync_send_property
    );
    ClassDB::bind_method(
        D_METHOD("sync_send_signal", "entity", "node", "signal", "args"),
        &NetwMultiplayer::sync_send_signal,
        DEFVAL(Array())
    );
    ClassDB::bind_method(
        D_METHOD("entity_attach", "node"),
        &NetwMultiplayer::entity_attach
    );
    ClassDB::bind_method(
        D_METHOD("player_list"),
        &NetwMultiplayer::player_list
    );
    ClassDB::bind_method(
        D_METHOD("player_get_local"),
        &NetwMultiplayer::player_get_local
    );
    ClassDB::bind_method(
        D_METHOD("player_find_account", "account"),
        &NetwMultiplayer::player_find_account
    );
    ClassDB::bind_method(
        D_METHOD("player_get_peer", "player"),
        &NetwMultiplayer::player_get_peer
    );
    ClassDB::bind_method(
        D_METHOD("player_is_active", "player"),
        &NetwMultiplayer::player_is_active
    );
    ClassDB::bind_method(
        D_METHOD("player_get_username", "player"),
        &NetwMultiplayer::player_get_username
    );
    ClassDB::bind_method(
        D_METHOD("player_get_account", "player"),
        &NetwMultiplayer::player_get_account
    );
    ClassDB::bind_method(
        D_METHOD("player_get_account_name", "player"),
        &NetwMultiplayer::player_get_account_name
    );
    ClassDB::bind_method(
        D_METHOD("player_get_scene", "player"),
        &NetwMultiplayer::player_get_scene
    );
    ClassDB::bind_method(
        D_METHOD("player_get_entities", "player"),
        &NetwMultiplayer::player_get_entities
    );
    BIND_ENUM_CONSTANT(ENTITY_STATE_UNKNOWN);
    BIND_ENUM_CONSTANT(ENTITY_STATE_LIVE);
    BIND_ENUM_CONSTANT(ENTITY_STATE_LINGERING);
    BIND_ENUM_CONSTANT(ENTITY_STATE_DEAD);
    BIND_ENUM_CONSTANT(ENTITY_STATE_ABSENT);
    BIND_ENUM_CONSTANT(ENTITY_PARAM_AUTHORITY);
    BIND_ENUM_CONSTANT(ENTITY_PARAM_TRANSFER);
    BIND_ENUM_CONSTANT(ENTITY_PARAM_ON_CONTROLLER_DISCONNECT);
    BIND_ENUM_CONSTANT(ENTITY_PARAM_ON_PARENT_DESPAWN);
    BIND_ENUM_CONSTANT(ENTITY_PARAM_HOLD);
    BIND_ENUM_CONSTANT(ENTITY_PARAM_REPLICATION_PRIORITY);
    BIND_ENUM_CONSTANT(CONTROL_STATE_SETTLED);
    BIND_ENUM_CONSTANT(CONTROL_STATE_PENDING);
    BIND_ENUM_CONSTANT(CONTROL_STATE_HELD);
    BIND_ENUM_CONSTANT(LAYER_PARAM_POLICY);
    BIND_ENUM_CONSTANT(LAYER_PARAM_LEAVE_POLICY);
    BIND_ENUM_CONSTANT(LAYER_PARAM_PERCEPTION_POLICY);
    BIND_ENUM_CONSTANT(TRANSPORT_PARAM_PEER_CLASS);
    BIND_ENUM_CONSTANT(TRANSPORT_PARAM_DISPLAY_NAME);
    BIND_ENUM_CONSTANT(TRANSPORT_PARAM_ADDRESS_LABEL);
    BIND_ENUM_CONSTANT(TRANSPORT_PARAM_ADDRESS_PLACEHOLDER);
    BIND_ENUM_CONSTANT(TRANSPORT_PARAM_ADDRESS_HELP);
    BIND_ENUM_CONSTANT(TRANSPORT_PARAM_CAPABILITIES);
    BIND_ENUM_CONSTANT(TRANSPORT_PARAM_HOST_SETTINGS);
    BIND_ENUM_CONSTANT(TRANSPORT_PARAM_CLIENT_SETTINGS);
    BIND_ENUM_CONSTANT(TRANSPORT_PARAM_TIMEOUT_HINT);

    BIND_ENUM_CONSTANT(ENDPOINT_PARAM_TRANSPORT);
    BIND_ENUM_CONSTANT(ENDPOINT_PARAM_ADDRESS);
    BIND_ENUM_CONSTANT(ENDPOINT_PARAM_DISPLAY_NAME);
    BIND_ENUM_CONSTANT(ENDPOINT_PARAM_SOURCE);
    BIND_ENUM_CONSTANT(ENDPOINT_PARAM_INVITED_BY);
    BIND_ENUM_CONSTANT(ENDPOINT_STATE_FLAGS);
    BIND_ENUM_CONSTANT(ENDPOINT_STATE_STATUS);
    BIND_ENUM_CONSTANT(ENDPOINT_STATE_INFO);
    BIND_ENUM_CONSTANT(ENDPOINT_FLAG_CALLER);
    BIND_ENUM_CONSTANT(ENDPOINT_FLAG_AVAILABLE);
    BIND_ENUM_CONSTANT(ENDPOINT_FLAG_OBSERVED);
    BIND_ENUM_CONSTANT(ENDPOINT_FLAG_INVITED);

    BIND_ENUM_CONSTANT(SCENE_PARAM_LABEL);
    BIND_ENUM_CONSTANT(SCENE_PARAM_ISOLATION);
    BIND_ENUM_CONSTANT(SCENE_PARAM_PROCESSING);
    BIND_ENUM_CONSTANT(CLOCK_PARAM_TICKRATE);
    BIND_ENUM_CONSTANT(CLOCK_PARAM_SYNC_MODE);
    BIND_ENUM_CONSTANT(CLOCK_PARAM_MAX_TICKS_PER_FRAME);
    BIND_ENUM_CONSTANT(CLOCK_PARAM_STALL_THRESHOLD);
    BIND_ENUM_CONSTANT(CLOCK_PARAM_PANIC_SNAP_THRESHOLD);
    BIND_ENUM_CONSTANT(CLOCK_PARAM_STRETCH_NUDGE_FACTOR);
    BIND_ENUM_CONSTANT(CLOCK_PARAM_DISPLAY_OFFSET);
    BIND_ENUM_CONSTANT(CLOCK_PARAM_LEAD_TICKS);
    BIND_ENUM_CONSTANT(CLOCK_PARAM_JITTER_MULTIPLIER);
    BIND_ENUM_CONSTANT(CLOCK_PARAM_JITTER_WINDOW);
    BIND_ENUM_CONSTANT(CLOCK_PARAM_JITTER_STABILITY_THRESHOLD);
    BIND_ENUM_CONSTANT(CLOCK_PARAM_USE_PHYSICS_INTERPOLATION);
    BIND_ENUM_CONSTANT(CLOCK_PARAM_MANUAL_TICK);
    BIND_ENUM_CONSTANT(CLOCK_PARAM_TICK_FACTOR_OVERRIDE);
    BIND_ENUM_CONSTANT(DISPLAY_PARAM_ROLE);
    BIND_ENUM_CONSTANT(DISPLAY_PARAM_LIVE_MODE);
    BIND_ENUM_CONSTANT(DISPLAY_PARAM_LIVE_SMOOTH_TIME);
    BIND_ENUM_CONSTANT(DISPLAY_PARAM_CHASE_GLIDE_TIME);
    BIND_ENUM_CONSTANT(DISPLAY_PARAM_TIMELINE_MODE);
    BIND_ENUM_CONSTANT(DISPLAY_PARAM_MAX_FORECAST_TICKS);
    BIND_ENUM_CONSTANT(DISPLAY_PARAM_SMART_DILATION);
    BIND_ENUM_CONSTANT(DISPLAY_PARAM_LAG_ADAPT_RATE);
    BIND_ENUM_CONSTANT(DISPLAY_PARAM_FLOOR_SMOOTHING);
    BIND_ENUM_CONSTANT(DISPLAY_PARAM_TRACE_INTERVAL);
    BIND_ENUM_CONSTANT(DISPLAY_PARAM_VISUAL_ROOT);
    BIND_ENUM_CONSTANT(PREDICT_PARAM_REPEAT_MISSING_INPUT);
    BIND_ENUM_CONSTANT(PREDICT_PARAM_MODE);
    BIND_ENUM_CONSTANT(PREDICT_PARAM_TELEPORT_THRESHOLD);
    BIND_ENUM_CONSTANT(PREDICT_PARAM_CORRECTION_THRESHOLD);
    BIND_ENUM_CONSTANT(PREDICT_PARAM_PAUSE_ON_UNPREDICTED_CONTACT);
    BIND_ENUM_CONSTANT(PREDICT_PARAM_CORRECTION_COOLDOWN_TICKS);
    BIND_ENUM_CONSTANT(PREDICT_PARAM_MAX_INPUTS_PER_TICK);
    BIND_ENUM_CONSTANT(PREDICT_PARAM_MAX_INPUT_LAG_TICKS);
    BIND_ENUM_CONSTANT(PREDICT_PARAM_INPUT_BUFFER_TICKS);
    BIND_ENUM_CONSTANT(PREDICT_PARAM_ROLLBACK_TOGETHER);
    BIND_ENUM_CONSTANT(PREDICT_PARAM_WORLD_VERSION);
    BIND_ENUM_CONSTANT(PREDICT_PARAM_CORRECTION_COUNT);
    BIND_ENUM_CONSTANT(SIMULATION_PARAM_BODIES);
    BIND_ENUM_CONSTANT(SIMULATION_PARAM_SCHEDULE);
    BIND_ENUM_CONSTANT(SIMULATION_PARAM_REMOTE_MODE);
    BIND_ENUM_CONSTANT(SIMULATION_PARAM_RESTORE);
    BIND_ENUM_CONSTANT(SIMULATION_PARAM_MAX_RESTORE_TICKS);
    BIND_ENUM_CONSTANT(SIMULATION_PARAM_CLAIM_ON_CONTACT);
    BIND_ENUM_CONSTANT(SIMULATION_PARAM_RELEASE_ON_REST);
    BIND_ENUM_CONSTANT(SIMULATION_PARAM_PHYSICS_DRIVEN);
#define NETW_SESSION_STAT_BIND(m_name, m_key) BIND_ENUM_CONSTANT(STAT_##m_name);
    NETW_SESSION_STAT_TABLE(NETW_SESSION_STAT_BIND)
#undef NETW_SESSION_STAT_BIND
    ADD_SIGNAL(
        MethodInfo("peer_authenticating", PropertyInfo(Variant::INT, "id"))
    );
    ADD_SIGNAL(MethodInfo(
        "peer_authentication_failed",
        PropertyInfo(Variant::INT, "id")
    ));
    ADD_SIGNAL(MethodInfo(
        "peer_packet",
        PropertyInfo(Variant::INT, "id"),
        PropertyInfo(Variant::PACKED_BYTE_ARRAY, "packet")
    ));
    ClassDB::bind_method(
        D_METHOD("scene_watch", "scene", "player"),
        &NetwMultiplayer::scene_watch
    );
    ClassDB::bind_method(
        D_METHOD("scene_unwatch", "scene", "player"),
        &NetwMultiplayer::scene_unwatch
    );
    ClassDB::bind_method(
        D_METHOD("scene_is_watching", "scene", "player"),
        &NetwMultiplayer::scene_is_watching
    );
    ClassDB::bind_method(
        D_METHOD("player_kick", "player", "reason"),
        &NetwMultiplayer::player_kick,
        DEFVAL(String())
    );
    ClassDB::bind_method(
        D_METHOD("player_request_kick", "player", "reason"),
        &NetwMultiplayer::player_request_kick,
        DEFVAL(String())
    );
    ClassDB::bind_static_method(
        "NetwMultiplayer",
        D_METHOD("session_join_declare", "handler", "arg_types"),
        &NetwMultiplayer::session_join_declare
    );
    ClassDB::bind_method(
        D_METHOD("session_prepare_join", "username", "args"),
        &NetwMultiplayer::session_prepare_join,
        DEFVAL(Array())
    );
    ClassDB::bind_method(
        D_METHOD("session_leave"),
        &NetwMultiplayer::session_leave
    );
    ClassDB::bind_method(
        D_METHOD("interest_layer_create", "name"),
        &NetwMultiplayer::interest_layer_create
    );
    ClassDB::bind_method(
        D_METHOD("interest_layer_free", "layer"),
        &NetwMultiplayer::interest_layer_free
    );
    ClassDB::bind_method(
        D_METHOD("interest_layer_add_viewer", "layer", "peer"),
        &NetwMultiplayer::interest_layer_add_viewer
    );
    ClassDB::bind_method(
        D_METHOD("interest_layer_remove_viewer", "layer", "peer"),
        &NetwMultiplayer::interest_layer_remove_viewer
    );
    ClassDB::bind_method(
        D_METHOD("interest_layer_add_entity", "layer", "entity"),
        &NetwMultiplayer::interest_layer_add_entity
    );
    ClassDB::bind_method(
        D_METHOD("interest_layer_remove_entity", "layer", "entity"),
        &NetwMultiplayer::interest_layer_remove_entity
    );
    ClassDB::bind_method(
        D_METHOD("interest_layer_set_param", "layer", "param", "value"),
        &NetwMultiplayer::interest_layer_set_param
    );
    ClassDB::bind_method(
        D_METHOD("interest_layer_set_driver_callback", "layer", "callback"),
        &NetwMultiplayer::interest_layer_set_driver_callback
    );
    ClassDB::bind_method(
        D_METHOD("interest_layer_get_name", "layer"),
        &NetwMultiplayer::interest_layer_get_name
    );
    ClassDB::bind_method(
        D_METHOD("interest_layer_get_param", "layer", "param"),
        &NetwMultiplayer::interest_layer_get_param
    );
    ClassDB::bind_method(
        D_METHOD("interest_layer_get_viewers", "layer"),
        &NetwMultiplayer::interest_layer_get_viewers
    );
    ClassDB::bind_method(
        D_METHOD("interest_layer_has_viewer", "layer", "peer"),
        &NetwMultiplayer::interest_layer_has_viewer
    );
    ClassDB::bind_method(
        D_METHOD("interest_layer_get_entities", "layer"),
        &NetwMultiplayer::interest_layer_get_entities
    );
    ClassDB::bind_method(
        D_METHOD("interest_layer_has_entity", "layer", "entity"),
        &NetwMultiplayer::interest_layer_has_entity
    );
    ClassDB::bind_method(
        D_METHOD("interest_layer_get_stats", "layer"),
        &NetwMultiplayer::interest_layer_get_stats
    );
    ClassDB::bind_method(
        D_METHOD("interest_join", "entity", "layer"),
        &NetwMultiplayer::interest_join
    );
    ClassDB::bind_method(
        D_METHOD("interest_leave", "entity", "layer"),
        &NetwMultiplayer::interest_leave
    );
    ClassDB::bind_method(
        D_METHOD("interest_on_enter", "entity", "layer", "callback"),
        &NetwMultiplayer::interest_on_enter
    );
    ClassDB::bind_method(
        D_METHOD("interest_on_leave", "entity", "layer", "callback"),
        &NetwMultiplayer::interest_on_leave
    );
    ClassDB::bind_method(
        D_METHOD(
            "interest_on_leave_policy",
            "entity",
            "layer",
            "policy",
            "custom_callback"
        ),
        &NetwMultiplayer::interest_on_leave_policy,
        DEFVAL(Callable())
    );
    ClassDB::bind_method(
        D_METHOD(
            "interest_on_perception_policy",
            "entity",
            "layer",
            "policy",
            "custom_callback"
        ),
        &NetwMultiplayer::interest_on_perception_policy,
        DEFVAL(Callable())
    );
    ClassDB::bind_method(
        D_METHOD("predict_stepper_install", "space", "stepper"),
        &NetwMultiplayer::predict_stepper_install,
        DEFVAL(Ref<RefCounted>())
    );
    ClassDB::bind_method(
        D_METHOD("interest_get_layers", "entity"),
        &NetwMultiplayer::interest_get_layers
    );
    ClassDB::bind_method(
        D_METHOD("interest_is_visible", "entity", "peer"),
        &NetwMultiplayer::interest_is_visible
    );
    ClassDB::bind_method(
        D_METHOD("get_root_path"),
        &NetwMultiplayer::get_root_path
    );
    ClassDB::bind_method(
        D_METHOD("set_root_path", "path"),
        &NetwMultiplayer::set_root_path
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::NODE_PATH, "root_path"),
        "set_root_path",
        "get_root_path"
    );
    ClassDB::bind_method(
        D_METHOD("is_object_decoding_allowed"),
        &NetwMultiplayer::is_object_decoding_allowed
    );
    ClassDB::bind_method(
        D_METHOD("set_allow_object_decoding", "enable"),
        &NetwMultiplayer::set_allow_object_decoding
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::BOOL, "allow_object_decoding"),
        "set_allow_object_decoding",
        "is_object_decoding_allowed"
    );
    ClassDB::bind_method(
        D_METHOD("get_auth_timeout"),
        &NetwMultiplayer::get_auth_timeout
    );
    ClassDB::bind_method(
        D_METHOD("set_auth_timeout", "timeout"),
        &NetwMultiplayer::set_auth_timeout
    );
    ADD_PROPERTY(
        PropertyInfo(
            Variant::FLOAT,
            "auth_timeout",
            PROPERTY_HINT_RANGE,
            "0,30,0.1,or_greater,suffix:s"
        ),
        "set_auth_timeout",
        "get_auth_timeout"
    );
    ClassDB::bind_method(
        D_METHOD("is_refusing_new_connections"),
        &NetwMultiplayer::is_refusing_new_connections
    );
    ClassDB::bind_method(
        D_METHOD("set_refuse_new_connections", "refuse"),
        &NetwMultiplayer::set_refuse_new_connections
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::BOOL, "refuse_new_connections"),
        "set_refuse_new_connections",
        "is_refusing_new_connections"
    );
    ClassDB::bind_method(
        D_METHOD("is_server_relay_enabled"),
        &NetwMultiplayer::is_server_relay_enabled
    );
    ClassDB::bind_method(
        D_METHOD("set_server_relay_enabled", "enabled"),
        &NetwMultiplayer::set_server_relay_enabled
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::BOOL, "server_relay"),
        "set_server_relay_enabled",
        "is_server_relay_enabled"
    );
    ClassDB::bind_method(
        D_METHOD("get_max_sync_packet_size"),
        &NetwMultiplayer::get_max_sync_packet_size
    );
    ClassDB::bind_method(
        D_METHOD("set_max_sync_packet_size", "size"),
        &NetwMultiplayer::set_max_sync_packet_size
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::INT, "max_sync_packet_size"),
        "set_max_sync_packet_size",
        "get_max_sync_packet_size"
    );
    ClassDB::bind_method(
        D_METHOD("get_max_delta_packet_size"),
        &NetwMultiplayer::get_max_delta_packet_size
    );
    ClassDB::bind_method(
        D_METHOD("set_max_delta_packet_size", "size"),
        &NetwMultiplayer::set_max_delta_packet_size
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::INT, "max_delta_packet_size"),
        "set_max_delta_packet_size",
        "get_max_delta_packet_size"
    );
    ClassDB::bind_method(
        D_METHOD(
            "rpc_channel_register",
            "channel",
            "handler",
            "defer_when_unknown"
        ),
        &NetwMultiplayer::rpc_channel_register,
        DEFVAL(false)
    );
    ClassDB::bind_method(
        D_METHOD("lagcomp_rewind", "entities", "tick", "body"),
        &NetwMultiplayer::lagcomp_rewind
    );
    ClassDB::bind_method(
        D_METHOD("lagcomp_sample", "entity", "tick"),
        &NetwMultiplayer::lagcomp_sample
    );
    ClassDB::bind_method(
        D_METHOD("entity_despawn", "entity", "opts"),
        &NetwMultiplayer::entity_despawn,
        DEFVAL(Ref<NetwDespawnOpts>())
    );
    ClassDB::bind_method(
        D_METHOD("send_bytes", "bytes", "id", "mode", "channel"),
        &NetwMultiplayer::send_bytes,
        DEFVAL(0),
        DEFVAL(int(MultiplayerPeer::TRANSFER_MODE_RELIABLE)),
        DEFVAL(0)
    );
    ClassDB::bind_method(
        D_METHOD("predict_declare", "entity"),
        &NetwMultiplayer::predict_declare
    );
    ClassDB::bind_method(
        D_METHOD("predict_undeclare", "entity"),
        &NetwMultiplayer::predict_undeclare
    );
    ClassDB::bind_method(
        D_METHOD("predict_set_param", "entity", "param", "value"),
        &NetwMultiplayer::predict_set_param
    );
    ClassDB::bind_method(
        D_METHOD("predict_get_param", "entity", "param"),
        &NetwMultiplayer::predict_get_param
    );
    ClassDB::bind_method(
        D_METHOD("predict_set_correction_path_callback", "entity", "callback"),
        &NetwMultiplayer::predict_set_correction_path_callback
    );
    ClassDB::bind_method(
        D_METHOD("predict_set_input_callback", "entity", "callback"),
        &NetwMultiplayer::predict_set_input_callback
    );
    ClassDB::bind_method(
        D_METHOD("predict_notify_collision", "entity"),
        &NetwMultiplayer::predict_notify_collision
    );
    ClassDB::bind_method(
        D_METHOD("simulation_set_param", "entity", "param", "value"),
        &NetwMultiplayer::simulation_set_param
    );
    ClassDB::bind_method(
        D_METHOD("simulation_get_param", "entity", "param"),
        &NetwMultiplayer::simulation_get_param
    );
    ClassDB::bind_method(
        D_METHOD(
            "simulation_set_network_process_callback",
            "entity",
            "callback"
        ),
        &NetwMultiplayer::simulation_set_network_process_callback
    );
    ClassDB::bind_method(
        D_METHOD("simulation_get_mode", "entity"),
        &NetwMultiplayer::simulation_get_mode
    );
    ClassDB::bind_method(
        D_METHOD("simulation_get_schedule", "entity"),
        &NetwMultiplayer::simulation_get_schedule
    );
    ClassDB::bind_method(
        D_METHOD("peer_get_player", "peer"),
        &NetwMultiplayer::peer_get_player
    );
    ClassDB::bind_method(
        D_METHOD("predict_is_declared", "entity"),
        &NetwMultiplayer::predict_is_declared
    );
    ClassDB::bind_method(
        D_METHOD("lagcomp_action_deny", "requester", "key", "code", "reason"),
        &NetwMultiplayer::lagcomp_action_deny,
        DEFVAL(ERR_UNAUTHORIZED),
        DEFVAL(String())
    );
    ClassDB::bind_method(
        D_METHOD("clock_get_param", "param"),
        &NetwMultiplayer::clock_get_param
    );
    ClassDB::bind_method(
        D_METHOD("clock_set_param", "param", "value"),
        &NetwMultiplayer::clock_set_param
    );
    ClassDB::bind_method(
        D_METHOD("clock_get_monitor", "monitor"),
        &NetwMultiplayer::clock_get_monitor
    );
    ClassDB::bind_method(
        D_METHOD("clock_get_tick_factor"),
        &NetwMultiplayer::clock_get_tick_factor
    );
    ClassDB::bind_method(
        D_METHOD("clock_get_tick_phase"),
        &NetwMultiplayer::clock_get_tick_phase
    );
    ClassDB::bind_method(
        D_METHOD("clock_get_physics_factor"),
        &NetwMultiplayer::clock_get_physics_factor
    );
    ClassDB::bind_method(
        D_METHOD("clock_get_recommended_display_offset"),
        &NetwMultiplayer::clock_get_recommended_display_offset
    );
    ClassDB::bind_method(
        D_METHOD("clock_get_tick"),
        &NetwMultiplayer::clock_get_tick
    );
    ClassDB::bind_method(
        D_METHOD("clock_get_simulation_behind_count"),
        &NetwMultiplayer::clock_get_simulation_behind_count
    );
    ClassDB::bind_method(
        D_METHOD("clock_is_configured"),
        &NetwMultiplayer::clock_is_configured
    );
    ClassDB::bind_method(
        D_METHOD("clock_is_synchronized"),
        &NetwMultiplayer::clock_is_synchronized
    );
    ClassDB::bind_method(
        D_METHOD("clock_is_simulating"),
        &NetwMultiplayer::clock_is_simulating
    );
    ClassDB::bind_method(
        D_METHOD("clock_is_gated"),
        &NetwMultiplayer::clock_is_gated
    );
    ClassDB::bind_method(
        D_METHOD("clock_step", "count"),
        &NetwMultiplayer::clock_step
    );
    ClassDB::bind_method(
        D_METHOD("clock_set_gate", "armed"),
        &NetwMultiplayer::clock_set_gate
    );
    ClassDB::bind_method(
        D_METHOD("display_set_param", "entity", "param", "value"),
        &NetwMultiplayer::display_set_param
    );
    ClassDB::bind_method(
        D_METHOD("display_set_callback", "entity", "callback"),
        &NetwMultiplayer::display_set_callback
    );
    ClassDB::bind_method(
        D_METHOD("display_set_target_item", "entity", "item"),
        &NetwMultiplayer::display_set_target_item
    );
    ClassDB::bind_method(
        D_METHOD("display_get_param", "entity", "param"),
        &NetwMultiplayer::display_get_param
    );
    ClassDB::bind_method(
        D_METHOD("display_reset", "entity"),
        &NetwMultiplayer::display_reset
    );
    ClassDB::bind_method(
        D_METHOD("display_get_tick", "entity"),
        &NetwMultiplayer::display_get_tick
    );
    ClassDB::bind_method(
        D_METHOD("display_get_track_stat", "entity", "track", "stat"),
        &NetwMultiplayer::display_get_track_stat
    );
    ClassDB::bind_method(
        D_METHOD("scene_set_param", "scene", "param", "value"),
        &NetwMultiplayer::scene_set_param
    );
    ClassDB::bind_method(
        D_METHOD("scene_get_param", "scene", "param"),
        &NetwMultiplayer::scene_get_param
    );
    ClassDB::bind_method(
        D_METHOD("scene_find", "stem"),
        &NetwMultiplayer::scene_find
    );
    ClassDB::bind_method(D_METHOD("scene_list"), &NetwMultiplayer::scene_list);
    ClassDB::bind_method(
        D_METHOD("scene_get_root", "scene"),
        &NetwMultiplayer::scene_get_root
    );
    ClassDB::bind_method(
        D_METHOD("scene_get_world", "scene"),
        &NetwMultiplayer::scene_get_world
    );
    ClassDB::bind_method(
        D_METHOD("scene_is_live", "scene"),
        &NetwMultiplayer::scene_is_live
    );
    ClassDB::bind_method(
        D_METHOD("scene_get_label", "scene"),
        &NetwMultiplayer::scene_get_label
    );
    ClassDB::bind_method(
        D_METHOD("scene_get_entities", "scene"),
        &NetwMultiplayer::scene_get_entities
    );
    ClassDB::bind_method(
        D_METHOD("scene_get_player_entities", "scene"),
        &NetwMultiplayer::scene_get_player_entities
    );
    ClassDB::bind_method(
        D_METHOD("scene_get_viewers", "scene"),
        &NetwMultiplayer::scene_get_viewers
    );
    ClassDB::bind_method(
        D_METHOD("scene_get_local_player_entities", "scene"),
        &NetwMultiplayer::scene_get_local_player_entities
    );
    ClassDB::bind_method(
        D_METHOD("lagcomp_action_key", "entity", "view_tick", "method"),
        &NetwMultiplayer::lagcomp_action_key
    );
    ClassDB::bind_static_method(
        "NetwMultiplayer",
        D_METHOD("lagcomp_action_declare", "root", "method", "handler"),
        &NetwMultiplayer::lagcomp_action_declare
    );
    ClassDB::bind_method(
        D_METHOD(
            "lagcomp_action_request",
            "target",
            "method",
            "view_tick",
            "args"
        ),
        &NetwMultiplayer::lagcomp_action_request,
        DEFVAL(Array())
    );
    ClassDB::bind_method(
        D_METHOD("lagcomp_timeline_declare", "entity"),
        &NetwMultiplayer::lagcomp_timeline_declare
    );
    ClassDB::bind_method(
        D_METHOD("lagcomp_timeline_undeclare", "entity"),
        &NetwMultiplayer::lagcomp_timeline_undeclare
    );
    ClassDB::bind_method(
        D_METHOD("entity_get_route", "entity"),
        &NetwMultiplayer::entity_get_route
    );
    ClassDB::bind_method(
        D_METHOD("entity_get_node", "entity"),
        &NetwMultiplayer::entity_get_node
    );
    ClassDB::bind_method(
        D_METHOD("entity_get_parent", "entity"),
        &NetwMultiplayer::entity_get_parent
    );
    ClassDB::bind_method(
        D_METHOD("entity_get_scene", "entity"),
        &NetwMultiplayer::entity_get_scene
    );
    ClassDB::bind_method(
        D_METHOD("entity_reparent", "entity", "parent"),
        &NetwMultiplayer::entity_reparent
    );
    ClassDB::bind_method(
        D_METHOD("entity_get_controller", "entity"),
        &NetwMultiplayer::entity_get_controller
    );
    ClassDB::bind_method(
        D_METHOD("entity_get_player", "entity"),
        &NetwMultiplayer::entity_get_player
    );
    ClassDB::bind_method(
        D_METHOD("entity_set_controller", "entity", "peer"),
        &NetwMultiplayer::entity_set_controller
    );
    ClassDB::bind_method(
        D_METHOD("entity_request_control", "entity", "hold"),
        &NetwMultiplayer::entity_request_control,
        DEFVAL(entity::Control::HOLD_EXCLUSIVE)
    );
    ClassDB::bind_method(
        D_METHOD("entity_release_control", "entity", "successor"),
        &NetwMultiplayer::entity_release_control,
        DEFVAL(entity::Control::SERVER)
    );
    ClassDB::bind_method(
        D_METHOD("entity_get_control_state", "entity"),
        &NetwMultiplayer::entity_get_control_state
    );
    ClassDB::bind_method(
        D_METHOD("entity_get_param", "entity", "param"),
        &NetwMultiplayer::entity_get_param
    );
    ClassDB::bind_method(
        D_METHOD("entity_set_param", "entity", "param", "value"),
        &NetwMultiplayer::entity_set_param
    );
    ClassDB::bind_method(
        D_METHOD("entity_get_state", "entity"),
        &NetwMultiplayer::entity_get_state
    );
    ClassDB::bind_method(
        D_METHOD("send_auth", "id", "data"),
        &NetwMultiplayer::send_auth
    );
    ClassDB::bind_method(
        D_METHOD("complete_auth", "id"),
        &NetwMultiplayer::complete_auth
    );
    ClassDB::bind_method(
        D_METHOD("get_authenticating_peers"),
        &NetwMultiplayer::get_authenticating_peers
    );
    ClassDB::bind_method(
        D_METHOD("set_auth_callback", "callback"),
        &NetwMultiplayer::set_auth_callback
    );
    ClassDB::bind_method(
        D_METHOD("get_auth_callback"),
        &NetwMultiplayer::get_auth_callback
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::CALLABLE, "auth_callback"),
        "set_auth_callback",
        "get_auth_callback"
    );
    ClassDB::bind_method(
        D_METHOD("disconnect_peer", "id"),
        &NetwMultiplayer::disconnect_peer
    );
    ClassDB::bind_method(
        D_METHOD("peer_get_bucket", "peer", "bucket_type"),
        &NetwMultiplayer::peer_get_bucket
    );
}

} // namespace netw
