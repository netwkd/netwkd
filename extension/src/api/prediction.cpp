#include "netw/api/prediction.hpp"

#include "godot/callable.hpp"
#include "godot/class_db.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/predict/decl.hpp"

using namespace godot;

namespace netw {

const char *NetwPrediction::GENERATOR_UNKNOWN_BEYOND_RETENTION
    = "UNKNOWN_BEYOND_RETENTION";

void NetwPrediction::write(int p_param, const Variant &p_value) {
    const Ref<NetwEntity> bound = get_entity();
    NetwMultiplayer::predict_write_param(
        bound.ptr(),
        NetwMultiplayer::PredictParam(p_param),
        p_value
    );
}

Variant NetwPrediction::read(int p_param) const {
    const Ref<NetwEntity> bound = get_entity();
    return NetwMultiplayer::predict_read_param(
        bound.ptr(),
        NetwMultiplayer::PredictParam(p_param)
    );
}

Ref<NetwPredictStats> NetwPrediction::get_stats() const {
    const Ref<NetwEntity> bound = get_entity();
    return NetwMultiplayer::predict_stats_of(bound.ptr());
}

void NetwPrediction::bind_entity(const Ref<NetwEntity> &p_entity) {
    entity_id = p_entity.is_valid() ? p_entity->get_instance_id() : ObjectID();
}

Ref<NetwEntity> NetwPrediction::get_entity() const {
    return Ref<NetwEntity>(
        Object::cast_to<NetwEntity>(ObjectDB::get_instance(entity_id))
    );
}

bool NetwPrediction::is_registered() const {
    const Ref<NetwEntity> bound = get_entity();
    NetwMultiplayer *home = NetwMultiplayer::entity_home(bound.ptr());
    return home != nullptr
        && home->predict_is_declared(bound->get_rid_handle());
}

void NetwPrediction::set_recovery_policy(
    NetwPrediction::RecoveryPolicy p_value
) {
    write(NetwMultiplayer::PREDICT_PARAM_RECOVERY_POLICY, p_value);
}

void NetwPrediction::set_breach_response(
    NetwPrediction::BreachResponse p_value
) {
    write(NetwMultiplayer::PREDICT_PARAM_BREACH_RESPONSE, p_value);
}

void NetwPrediction::set_sensors(const Dictionary &p_value) {
    const Ref<NetwEntity> bound = get_entity();
    NetwMultiplayer::predict_write_sensors(bound.ptr(), p_value);
}

void NetwPrediction::set_epoch(int64_t p_value) {
    write(NetwMultiplayer::PREDICT_PARAM_EPOCH, p_value);
}

void NetwPrediction::set_witness_contacts(const Callable &p_value) {
    const Ref<NetwEntity> bound = get_entity();
    NetwMultiplayer::predict_write_witness(bound.ptr(), p_value);
}

void NetwPrediction::set_correction_path_clear(const Callable &p_value) {
    const Ref<NetwEntity> bound = get_entity();
    NetwMultiplayer::predict_write_correction_path(bound.ptr(), p_value);
}

void NetwPrediction::set_teleport_threshold(double p_value) {
    write(NetwMultiplayer::PREDICT_PARAM_TELEPORT_THRESHOLD, p_value);
}

void NetwPrediction::set_collision_cooldown_ticks(int p_value) {
    write(NetwMultiplayer::PREDICT_PARAM_COLLISION_COOLDOWN_TICKS, p_value);
}

void NetwPrediction::set_sleeping(bool p_value) {
    write(NetwMultiplayer::PREDICT_PARAM_SLEEPING, p_value);
}

void NetwPrediction::set_missing_policy(NetwPrediction::MissingInput p_value) {
    write(NetwMultiplayer::PREDICT_PARAM_MISSING_POLICY, p_value);
}

void NetwPrediction::set_max_consume_per_tick(int p_value) {
    write(NetwMultiplayer::PREDICT_PARAM_MAX_CONSUME_PER_TICK, p_value);
}

void NetwPrediction::set_consume_buffer_ticks(int p_value) {
    write(NetwMultiplayer::PREDICT_PARAM_CONSUME_BUFFER_TICKS, p_value);
}

void NetwPrediction::set_replay_buffer_depth(int p_value) {
    write(NetwMultiplayer::PREDICT_PARAM_REPLAY_BUFFER_DEPTH, p_value);
}

void NetwPrediction::set_max_consume_lag_ticks(int p_value) {
    write(NetwMultiplayer::PREDICT_PARAM_MAX_CONSUME_LAG_TICKS, p_value);
}

void NetwPrediction::set_ack_age_ticks(int p_value) {
    write(NetwMultiplayer::PREDICT_PARAM_ACK_AGE_TICKS, p_value);
}

void NetwPrediction::set_divergence_epsilon(double p_value) {
    write(NetwMultiplayer::PREDICT_PARAM_DIVERGENCE_EPSILON, p_value);
}

void NetwPrediction::set_reconcile_mode(NetwPrediction::Reconcile p_value) {
    write(NetwMultiplayer::PREDICT_PARAM_RECONCILE_MODE, p_value);
}

void NetwPrediction::set_archetype(NetwPrediction::Archetype p_value) {
    write(NetwMultiplayer::PREDICT_PARAM_ARCHETYPE, p_value);
}

NetwPrediction::RecoveryPolicy NetwPrediction::get_recovery_policy() const {
    return RecoveryPolicy(
        int(read(NetwMultiplayer::PREDICT_PARAM_RECOVERY_POLICY))
    );
}

NetwPrediction::BreachResponse NetwPrediction::get_breach_response() const {
    return BreachResponse(
        int(read(NetwMultiplayer::PREDICT_PARAM_BREACH_RESPONSE))
    );
}

Dictionary NetwPrediction::get_sensors() const {
    const Ref<NetwEntity> bound = get_entity();
    const predict::Decl *decl
        = NetwMultiplayer::predict_find_declaration(bound.ptr());
    return decl != nullptr ? decl->sensors : Dictionary();
}

int64_t NetwPrediction::get_epoch() const {
    return int64_t(read(NetwMultiplayer::PREDICT_PARAM_EPOCH));
}

Callable NetwPrediction::get_witness_contacts() const {
    const Ref<NetwEntity> bound = get_entity();
    const predict::Decl *decl
        = NetwMultiplayer::predict_find_declaration(bound.ptr());
    return decl != nullptr ? decl->witness : Callable();
}

Callable NetwPrediction::get_correction_path_clear() const {
    const Ref<NetwEntity> bound = get_entity();
    const predict::Decl *decl
        = NetwMultiplayer::predict_find_declaration(bound.ptr());
    return decl != nullptr ? decl->correction_path : Callable();
}

double NetwPrediction::get_teleport_threshold() const {
    return double(read(NetwMultiplayer::PREDICT_PARAM_TELEPORT_THRESHOLD));
}

int NetwPrediction::get_collision_cooldown_ticks() const {
    return int(read(NetwMultiplayer::PREDICT_PARAM_COLLISION_COOLDOWN_TICKS));
}

bool NetwPrediction::get_sleeping() const {
    return bool(read(NetwMultiplayer::PREDICT_PARAM_SLEEPING));
}

NetwPrediction::MissingInput NetwPrediction::get_missing_policy() const {
    return MissingInput(
        int(read(NetwMultiplayer::PREDICT_PARAM_MISSING_POLICY))
    );
}

int NetwPrediction::get_max_consume_per_tick() const {
    return int(read(NetwMultiplayer::PREDICT_PARAM_MAX_CONSUME_PER_TICK));
}

int NetwPrediction::get_consume_buffer_ticks() const {
    return int(read(NetwMultiplayer::PREDICT_PARAM_CONSUME_BUFFER_TICKS));
}

int NetwPrediction::get_replay_buffer_depth() const {
    return int(read(NetwMultiplayer::PREDICT_PARAM_REPLAY_BUFFER_DEPTH));
}

int NetwPrediction::get_max_consume_lag_ticks() const {
    return int(read(NetwMultiplayer::PREDICT_PARAM_MAX_CONSUME_LAG_TICKS));
}

int NetwPrediction::get_ack_age_ticks() const {
    return int(read(NetwMultiplayer::PREDICT_PARAM_ACK_AGE_TICKS));
}

double NetwPrediction::get_divergence_epsilon() const {
    return double(read(NetwMultiplayer::PREDICT_PARAM_DIVERGENCE_EPSILON));
}

NetwPrediction::Reconcile NetwPrediction::get_reconcile_mode() const {
    return Reconcile(int(read(NetwMultiplayer::PREDICT_PARAM_RECONCILE_MODE)));
}

NetwPrediction::Archetype NetwPrediction::get_archetype() const {
    return Archetype(int(read(NetwMultiplayer::PREDICT_PARAM_ARCHETYPE)));
}

Variant NetwPrediction::sensor(
    const StringName &p_name,
    const Variant &p_default
) const {
    const Ref<NetwEntity> bound = get_entity();
    NetwMultiplayer *home = NetwMultiplayer::entity_home(bound.ptr());
    return home != nullptr && home->predict_is_declared(bound->get_rid_handle())
        ? home->predict_get_sensor(bound->get_rid_handle(), p_name, p_default)
        : p_default;
}

NetwPrediction::RecoveryPolicy NetwPrediction::
    resolved_recovery_policy() const {
    const Ref<NetwEntity> bound = get_entity();
    return NetwMultiplayer::predict_resolve_recovery(bound.ptr());
}

void NetwPrediction::simulate_tick(double p_delta, int64_t p_tick) {
    const Ref<NetwEntity> bound = get_entity();
    NetwMultiplayer::predict_simulate(bound.ptr(), p_delta, p_tick, false);
}

void NetwPrediction::simulate_frame(double p_delta) {
    const Ref<NetwEntity> bound = get_entity();
    NetwMultiplayer::predict_simulate(bound.ptr(), p_delta, 0, true);
}

Dictionary NetwPrediction::reachability() const {
    const Ref<NetwEntity> bound = get_entity();
    NetwMultiplayer *home = NetwMultiplayer::entity_home(bound.ptr());
    return home != nullptr
        ? home->predict_get_reachability(bound->get_rid_handle())
        : Dictionary();
}

void NetwPrediction::notify_contact() {
    const Ref<NetwEntity> bound = get_entity();
    NetwMultiplayer *home = NetwMultiplayer::entity_home(bound.ptr());
    if (home != nullptr && home->predict_is_declared(bound->get_rid_handle())) {
        home->predict_notify_contact(bound->get_rid_handle());
    }
}

void NetwPrediction::set_predict_input(const Callable &p_predictor) {
    const Ref<NetwEntity> bound = get_entity();
    NetwMultiplayer::predict_write_input(bound.ptr(), p_predictor);
}

Callable NetwPrediction::get_predict_input() const {
    const Ref<NetwEntity> bound = get_entity();
    const predict::Decl *decl
        = NetwMultiplayer::predict_find_declaration(bound.ptr());
    return decl != nullptr ? decl->input : Callable();
}

void NetwPrediction::_bind_methods() {
    ClassDB::bind_method(
        D_METHOD("bind_entity", "entity"),
        &NetwPrediction::bind_entity
    );
    ClassDB::bind_method(D_METHOD("entity"), &NetwPrediction::get_entity);
    ClassDB::bind_method(
        D_METHOD("is_registered"),
        &NetwPrediction::is_registered
    );
    ClassDB::bind_method(
        D_METHOD("sensor", "name", "default"),
        &NetwPrediction::sensor,
        DEFVAL(Variant())
    );
    ClassDB::bind_method(
        D_METHOD("resolved_recovery_policy"),
        &NetwPrediction::resolved_recovery_policy
    );
    ClassDB::bind_method(
        D_METHOD("simulate_tick", "delta", "tick"),
        &NetwPrediction::simulate_tick
    );
    ClassDB::bind_method(
        D_METHOD("simulate_frame", "delta"),
        &NetwPrediction::simulate_frame
    );
    ClassDB::bind_method(
        D_METHOD("reachability"),
        &NetwPrediction::reachability
    );
    ClassDB::bind_method(
        D_METHOD("notify_contact"),
        &NetwPrediction::notify_contact
    );

    ClassDB::bind_method(
        D_METHOD("get_predict_input"),
        &NetwPrediction::get_predict_input
    );
    ClassDB::bind_method(
        D_METHOD("set_predict_input", "value"),
        &NetwPrediction::set_predict_input
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::CALLABLE, "predict_input"),
        "set_predict_input",
        "get_predict_input"
    );
    ClassDB::bind_method(
        D_METHOD("get_recovery_policy"),
        &NetwPrediction::get_recovery_policy
    );
    ClassDB::bind_method(
        D_METHOD("set_recovery_policy", "value"),
        &NetwPrediction::set_recovery_policy
    );
    ADD_PROPERTY(
        PropertyInfo(
            Variant::INT,
            "recovery_policy",
            PROPERTY_HINT_ENUM,
            "Rebase Replay,Rebase Recover,Delay Closed,Observe,Auto",
            PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_CLASS_IS_ENUM,
            "NetwPrediction.RecoveryPolicy"
        ),
        "set_recovery_policy",
        "get_recovery_policy"
    );
    ClassDB::bind_method(
        D_METHOD("get_breach_response"),
        &NetwPrediction::get_breach_response
    );
    ClassDB::bind_method(
        D_METHOD("set_breach_response", "value"),
        &NetwPrediction::set_breach_response
    );
    ADD_PROPERTY(
        PropertyInfo(
            Variant::INT,
            "breach_response",
            PROPERTY_HINT_ENUM,
            "Discard,Fallback,Trace",
            PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_CLASS_IS_ENUM,
            "NetwPrediction.BreachResponse"
        ),
        "set_breach_response",
        "get_breach_response"
    );
    ClassDB::bind_method(D_METHOD("get_sensors"), &NetwPrediction::get_sensors);
    ClassDB::bind_method(
        D_METHOD("set_sensors", "value"),
        &NetwPrediction::set_sensors
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::DICTIONARY, "sensors"),
        "set_sensors",
        "get_sensors"
    );
    ClassDB::bind_method(D_METHOD("get_epoch"), &NetwPrediction::get_epoch);
    ClassDB::bind_method(
        D_METHOD("set_epoch", "value"),
        &NetwPrediction::set_epoch
    );
    ADD_PROPERTY(PropertyInfo(Variant::INT, "epoch"), "set_epoch", "get_epoch");
    ClassDB::bind_method(
        D_METHOD("get_witness_contacts"),
        &NetwPrediction::get_witness_contacts
    );
    ClassDB::bind_method(
        D_METHOD("set_witness_contacts", "value"),
        &NetwPrediction::set_witness_contacts
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::CALLABLE, "witness_contacts"),
        "set_witness_contacts",
        "get_witness_contacts"
    );
    ClassDB::bind_method(
        D_METHOD("get_correction_path_clear"),
        &NetwPrediction::get_correction_path_clear
    );
    ClassDB::bind_method(
        D_METHOD("set_correction_path_clear", "value"),
        &NetwPrediction::set_correction_path_clear
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::CALLABLE, "correction_path_clear"),
        "set_correction_path_clear",
        "get_correction_path_clear"
    );
    ClassDB::bind_method(
        D_METHOD("get_teleport_threshold"),
        &NetwPrediction::get_teleport_threshold
    );
    ClassDB::bind_method(
        D_METHOD("set_teleport_threshold", "value"),
        &NetwPrediction::set_teleport_threshold
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::FLOAT, "teleport_threshold"),
        "set_teleport_threshold",
        "get_teleport_threshold"
    );
    ClassDB::bind_method(
        D_METHOD("get_collision_cooldown_ticks"),
        &NetwPrediction::get_collision_cooldown_ticks
    );
    ClassDB::bind_method(
        D_METHOD("set_collision_cooldown_ticks", "value"),
        &NetwPrediction::set_collision_cooldown_ticks
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::INT, "collision_cooldown_ticks"),
        "set_collision_cooldown_ticks",
        "get_collision_cooldown_ticks"
    );
    ClassDB::bind_method(
        D_METHOD("get_sleeping"),
        &NetwPrediction::get_sleeping
    );
    ClassDB::bind_method(
        D_METHOD("set_sleeping", "value"),
        &NetwPrediction::set_sleeping
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::BOOL, "sleeping"),
        "set_sleeping",
        "get_sleeping"
    );
    ClassDB::bind_method(
        D_METHOD("get_missing_policy"),
        &NetwPrediction::get_missing_policy
    );
    ClassDB::bind_method(
        D_METHOD("set_missing_policy", "value"),
        &NetwPrediction::set_missing_policy
    );
    ADD_PROPERTY(
        PropertyInfo(
            Variant::INT,
            "missing_policy",
            PROPERTY_HINT_ENUM,
            "None,RepeatLast,Extrapolate,Custom",
            PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_CLASS_IS_ENUM,
            "NetwPrediction.MissingInput"
        ),
        "set_missing_policy",
        "get_missing_policy"
    );
    ClassDB::bind_method(
        D_METHOD("get_max_consume_per_tick"),
        &NetwPrediction::get_max_consume_per_tick
    );
    ClassDB::bind_method(
        D_METHOD("set_max_consume_per_tick", "value"),
        &NetwPrediction::set_max_consume_per_tick
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::INT, "max_consume_per_tick"),
        "set_max_consume_per_tick",
        "get_max_consume_per_tick"
    );
    ClassDB::bind_method(
        D_METHOD("get_consume_buffer_ticks"),
        &NetwPrediction::get_consume_buffer_ticks
    );
    ClassDB::bind_method(
        D_METHOD("set_consume_buffer_ticks", "value"),
        &NetwPrediction::set_consume_buffer_ticks
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::INT, "consume_buffer_ticks"),
        "set_consume_buffer_ticks",
        "get_consume_buffer_ticks"
    );
    ClassDB::bind_method(
        D_METHOD("get_replay_buffer_depth"),
        &NetwPrediction::get_replay_buffer_depth
    );
    ClassDB::bind_method(
        D_METHOD("set_replay_buffer_depth", "value"),
        &NetwPrediction::set_replay_buffer_depth
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::INT, "replay_buffer_depth"),
        "set_replay_buffer_depth",
        "get_replay_buffer_depth"
    );
    ClassDB::bind_method(
        D_METHOD("get_max_consume_lag_ticks"),
        &NetwPrediction::get_max_consume_lag_ticks
    );
    ClassDB::bind_method(
        D_METHOD("set_max_consume_lag_ticks", "value"),
        &NetwPrediction::set_max_consume_lag_ticks
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::INT, "max_consume_lag_ticks"),
        "set_max_consume_lag_ticks",
        "get_max_consume_lag_ticks"
    );
    ClassDB::bind_method(
        D_METHOD("get_ack_age_ticks"),
        &NetwPrediction::get_ack_age_ticks
    );
    ClassDB::bind_method(
        D_METHOD("set_ack_age_ticks", "value"),
        &NetwPrediction::set_ack_age_ticks
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::INT, "ack_age_ticks"),
        "set_ack_age_ticks",
        "get_ack_age_ticks"
    );
    ClassDB::bind_method(
        D_METHOD("get_divergence_epsilon"),
        &NetwPrediction::get_divergence_epsilon
    );
    ClassDB::bind_method(
        D_METHOD("set_divergence_epsilon", "value"),
        &NetwPrediction::set_divergence_epsilon
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::FLOAT, "divergence_epsilon"),
        "set_divergence_epsilon",
        "get_divergence_epsilon"
    );
    ClassDB::bind_method(
        D_METHOD("get_reconcile_mode"),
        &NetwPrediction::get_reconcile_mode
    );
    ClassDB::bind_method(
        D_METHOD("set_reconcile_mode", "value"),
        &NetwPrediction::set_reconcile_mode
    );
    ADD_PROPERTY(
        PropertyInfo(
            Variant::INT,
            "reconcile_mode",
            PROPERTY_HINT_ENUM,
            "Replay,State,Custom",
            PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_CLASS_IS_ENUM,
            "NetwPrediction.Reconcile"
        ),
        "set_reconcile_mode",
        "get_reconcile_mode"
    );
    ClassDB::bind_method(
        D_METHOD("get_archetype"),
        &NetwPrediction::get_archetype
    );
    ClassDB::bind_method(
        D_METHOD("set_archetype", "value"),
        &NetwPrediction::set_archetype
    );
    ADD_PROPERTY(
        PropertyInfo(
            Variant::INT,
            "archetype",
            PROPERTY_HINT_ENUM,
            "None,Scripted,SolverBody",
            PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_CLASS_IS_ENUM,
            "NetwPrediction.Archetype"
        ),
        "set_archetype",
        "get_archetype"
    );

    ClassDB::bind_method(D_METHOD("get_stats"), &NetwPrediction::get_stats);
    ADD_PROPERTY(
        PropertyInfo(
            Variant::OBJECT,
            "stats",
            PROPERTY_HINT_RESOURCE_TYPE,
            "NetwPredictStats",
            PROPERTY_USAGE_NONE
        ),
        godot::String(),
        "get_stats"
    );

    ClassDB::bind_integer_constant(
        get_class_static(),
        StringName(),
        "GENERATOR_UNKNOWN_BEYOND_RETENTION",
        0
    );

    BIND_CONSTANT(ACK_AGE_MAX);

    BIND_ENUM_CONSTANT(CONTACT_CLASS_NONE);
    BIND_ENUM_CONSTANT(CONTACT_CLASS_DECLARED_SUPPORT);
    BIND_ENUM_CONSTANT(CONTACT_CLASS_OTHER_STATIC);
    BIND_ENUM_CONSTANT(CONTACT_CLASS_PREDICTED_DYNAMIC);
    BIND_ENUM_CONSTANT(CONTACT_CLASS_UNPREDICTED_DYNAMIC);
    BIND_ENUM_CONSTANT(CONTACT_CLASS_KINEMATIC_PROXY);

    BIND_ENUM_CONSTANT(COMMAND_ORIGIN_PREDICTED);
    BIND_ENUM_CONSTANT(COMMAND_ORIGIN_RELAYED);

    BIND_ENUM_CONSTANT(DRIVE_KIND_NONE);
    BIND_ENUM_CONSTANT(DRIVE_KIND_FRESH);
    BIND_ENUM_CONSTANT(DRIVE_KIND_REPEAT);
    BIND_ENUM_CONSTANT(DRIVE_KIND_HOLD);
    BIND_ENUM_CONSTANT(DRIVE_KIND_STARVED);
    BIND_ENUM_CONSTANT(DRIVE_KIND_FOLD_DRIVE);
    BIND_ENUM_CONSTANT(DRIVE_KIND_MISSING);
    BIND_ENUM_CONSTANT(DRIVE_KIND_SUBSTITUTED);

    BIND_ENUM_CONSTANT(TRIGGER_SHAPE_NONE);
    BIND_ENUM_CONSTANT(TRIGGER_SHAPE_MIXED);
    BIND_ENUM_CONSTANT(TRIGGER_SHAPE_ALL_WITHHELD);

    BIND_ENUM_CONSTANT(CONSUME_ACTION_REPLAY);
    BIND_ENUM_CONSTANT(CONSUME_ACTION_HOLD);
    BIND_ENUM_CONSTANT(CONSUME_ACTION_STARVED);

    BIND_ENUM_CONSTANT(EXACT_VERDICT_UNJUDGED);
    BIND_ENUM_CONSTANT(EXACT_VERDICT_EQUAL);
    BIND_ENUM_CONSTANT(EXACT_VERDICT_UNEQUAL);

    BIND_ENUM_CONSTANT(MISSING_INPUT_STALL);
    BIND_ENUM_CONSTANT(MISSING_INPUT_REPEAT_LAST);

    BIND_ENUM_CONSTANT(ARCHETYPE_NONE);
    BIND_ENUM_CONSTANT(ARCHETYPE_SCRIPTED);
    BIND_ENUM_CONSTANT(ARCHETYPE_SOLVER_BODY);

    BIND_ENUM_CONSTANT(RECOVERY_POLICY_REBASE_REPLAY);
    BIND_ENUM_CONSTANT(RECOVERY_POLICY_REBASE_RECOVER);
    BIND_ENUM_CONSTANT(RECOVERY_POLICY_DELAY_CLOSED);
    BIND_ENUM_CONSTANT(RECOVERY_POLICY_OBSERVE);
    BIND_ENUM_CONSTANT(RECOVERY_POLICY_AUTO);

    BIND_ENUM_CONSTANT(RECONCILE_INDEPENDENT);
    BIND_ENUM_CONSTANT(RECONCILE_JOINT);

    BIND_ENUM_CONSTANT(BREACH_RESPONSE_PREDICT_THROUGH);
    BIND_ENUM_CONSTANT(BREACH_RESPONSE_DEMOTE);

    BIND_ENUM_CONSTANT(EPISODE_STATE_OPEN);
    BIND_ENUM_CONSTANT(EPISODE_STATE_CLOSED);
    BIND_ENUM_CONSTANT(EPISODE_STATE_FALLBACK);
    BIND_ENUM_CONSTANT(EPISODE_CHANGE_OPENED);
    BIND_ENUM_CONSTANT(EPISODE_CHANGE_FALLBACK);
    BIND_ENUM_CONSTANT(EPISODE_CHANGE_CLOSED);

    BIND_ENUM_CONSTANT(OPERATOR_OUTCOME_PENDING);
    BIND_ENUM_CONSTANT(OPERATOR_OUTCOME_CONTRACTED);
    BIND_ENUM_CONSTANT(OPERATOR_OUTCOME_FAILED_TO_CONTRACT);
    BIND_ENUM_CONSTANT(OPERATOR_OUTCOME_INTRODUCED_BOUNDARY);
    BIND_ENUM_CONSTANT(OPERATOR_OUTCOME_WITHHELD);

    BIND_ENUM_CONSTANT(ATTRIBUTION_UNKNOWN);
    BIND_ENUM_CONSTANT(ATTRIBUTION_PRE_STATE);
    BIND_ENUM_CONSTANT(ATTRIBUTION_COMMAND);
    BIND_ENUM_CONSTANT(ATTRIBUTION_ENVIRONMENT);
    BIND_ENUM_CONSTANT(ATTRIBUTION_TOPOLOGY);
    BIND_ENUM_CONSTANT(ATTRIBUTION_EXECUTION);
    BIND_ENUM_CONSTANT(ATTRIBUTION_CONTACT);
    BIND_ENUM_CONSTANT(ATTRIBUTION_CLOSURE);

    BIND_ENUM_CONSTANT(OPERATOR_NONE);
    BIND_ENUM_CONSTANT(OPERATOR_REBASE_PROJECTED);
    BIND_ENUM_CONSTANT(OPERATOR_REBASE_EXACT);
    BIND_ENUM_CONSTANT(OPERATOR_FULL_CLOSURE);
    BIND_ENUM_CONSTANT(OPERATOR_TRANSPORT_DELTA);
    BIND_ENUM_CONSTANT(OPERATOR_RESEED);
    BIND_ENUM_CONSTANT(OPERATOR_RESEED_ALIGN);
    BIND_ENUM_CONSTANT(OPERATOR_DEMOTE);
    BIND_ENUM_CONSTANT(OPERATOR_DISSIPATE);
    BIND_ENUM_CONSTANT(OPERATOR_JOINT_REBASE);

    BIND_ENUM_CONSTANT(DOMAIN_IN);
    BIND_ENUM_CONSTANT(DOMAIN_OUT);

    ADD_SIGNAL(MethodInfo(
        "divergence_detected",
        PropertyInfo(Variant::INT, "entry"),
        PropertyInfo(
            Variant::INT,
            "attribution",
            PROPERTY_HINT_NONE,
            "",
            PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_CLASS_IS_ENUM,
            "NetwPrediction.Attribution"
        )
    ));
    ADD_SIGNAL(MethodInfo(
        "episode_changed",
        PropertyInfo(
            Variant::INT,
            "change",
            PROPERTY_HINT_NONE,
            "",
            PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_CLASS_IS_ENUM,
            "NetwPrediction.EpisodeChange"
        ),
        PropertyInfo(Variant::DICTIONARY, "report")
    ));
    ADD_SIGNAL(MethodInfo(
        "recovered",
        PropertyInfo(Variant::INT, "entry"),
        PropertyInfo(Variant::DICTIONARY, "deltas"),
        PropertyInfo(Variant::BOOL, "teleported"),
        PropertyInfo(
            Variant::INT,
            "attribution",
            PROPERTY_HINT_NONE,
            "",
            PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_CLASS_IS_ENUM,
            "NetwPrediction.Attribution"
        )
    ));
    ADD_SIGNAL(MethodInfo(
        "state_evaluated",
        PropertyInfo(Variant::INT, "recv_tick"),
        PropertyInfo(Variant::INT, "ack"),
        PropertyInfo(Variant::FLOAT, "divergence"),
        PropertyInfo(Variant::BOOL, "diverged")
    ));
}

Ref<NetwPrediction> build_prediction_handle(Object *p_entity) {
    Ref<NetwPrediction> made;
    made.instantiate();
    made->bind_entity(Ref<NetwEntity>(Object::cast_to<NetwEntity>(p_entity)));
    return made;
}

} // namespace netw
