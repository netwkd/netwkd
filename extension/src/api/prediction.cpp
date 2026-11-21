#include "netw/api/prediction.hpp"

#include "godot/callable.hpp"
#include "godot/class_db.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/predict/decl.hpp"

using namespace godot;

namespace netw {

void NetwPrediction::write(int p_param, const Variant &p_value) {
    const Ref<NetwEntity> bound = get_entity();
    if (bound.is_null()) {
        return;
    }
    NetwMultiplayer::predict_write_param(
        bound->home_session(),
        *bound->get_record(),
        NetwMultiplayer::PredictParam(p_param),
        p_value
    );
}

Variant NetwPrediction::read(int p_param) const {
    const Ref<NetwEntity> bound = get_entity();
    const auto param = NetwMultiplayer::PredictParam(p_param);
    if (bound.is_null()) {
        return NetwMultiplayer::predict_read_param(
            NetwEntityRecord(),
            nullptr,
            param
        );
    }
    return NetwMultiplayer::predict_read_param(
        *bound->get_record(),
        bound->get_owner(),
        param
    );
}

int64_t NetwPrediction::get_correction_count() const {
    return int64_t(read(NetwMultiplayer::PREDICT_PARAM_CORRECTION_COUNT));
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
    NetwMultiplayer *home
        = bound.is_valid() ? bound->home_session() : nullptr;
    return home != nullptr
        && home->predict_is_declared(bound->get_rid_handle());
}

void NetwPrediction::set_mode(NetwPrediction::Mode p_value) {
    write(NetwMultiplayer::PREDICT_PARAM_MODE, p_value);
}

void NetwPrediction::set_pause_on_unpredicted_contact(bool p_value) {
    write(NetwMultiplayer::PREDICT_PARAM_PAUSE_ON_UNPREDICTED_CONTACT, p_value);
}

void NetwPrediction::set_world_version(int64_t p_value) {
    write(NetwMultiplayer::PREDICT_PARAM_WORLD_VERSION, p_value);
}

void NetwPrediction::set_correction_path_clear(const Callable &p_value) {
    const Ref<NetwEntity> bound = get_entity();
    if (bound.is_null()) {
        return;
    }
    NetwMultiplayer::predict_write_correction_path(
        *bound->get_record(),
        bound->get_owner(),
        p_value
    );
}

void NetwPrediction::set_teleport_threshold(double p_value) {
    write(NetwMultiplayer::PREDICT_PARAM_TELEPORT_THRESHOLD, p_value);
}

void NetwPrediction::set_correction_cooldown_ticks(int p_value) {
    write(NetwMultiplayer::PREDICT_PARAM_CORRECTION_COOLDOWN_TICKS, p_value);
}

void NetwPrediction::set_repeat_missing_input(bool p_value) {
    write(NetwMultiplayer::PREDICT_PARAM_REPEAT_MISSING_INPUT, p_value);
}

void NetwPrediction::set_max_inputs_per_tick(int p_value) {
    write(NetwMultiplayer::PREDICT_PARAM_MAX_INPUTS_PER_TICK, p_value);
}

void NetwPrediction::set_input_buffer_ticks(int p_value) {
    write(NetwMultiplayer::PREDICT_PARAM_INPUT_BUFFER_TICKS, p_value);
}

void NetwPrediction::set_max_input_lag_ticks(int p_value) {
    write(NetwMultiplayer::PREDICT_PARAM_MAX_INPUT_LAG_TICKS, p_value);
}

void NetwPrediction::set_correction_threshold(double p_value) {
    write(NetwMultiplayer::PREDICT_PARAM_CORRECTION_THRESHOLD, p_value);
}

void NetwPrediction::set_rollback_together(bool p_value) {
    write(NetwMultiplayer::PREDICT_PARAM_ROLLBACK_TOGETHER, p_value);
}

NetwPrediction::Mode NetwPrediction::get_mode() const {
    return Mode(int(read(NetwMultiplayer::PREDICT_PARAM_MODE)));
}

bool NetwPrediction::get_pause_on_unpredicted_contact() const {
    return bool(
        read(NetwMultiplayer::PREDICT_PARAM_PAUSE_ON_UNPREDICTED_CONTACT)
    );
}

int64_t NetwPrediction::get_world_version() const {
    return int64_t(read(NetwMultiplayer::PREDICT_PARAM_WORLD_VERSION));
}

Callable NetwPrediction::get_correction_path_clear() const {
    const Ref<NetwEntity> bound = get_entity();
    const predict::Decl *decl = bound.is_valid()
        ? bound->get_record()->find_predict_declaration()
        : nullptr;
    return decl != nullptr ? decl->correction_path : Callable();
}

double NetwPrediction::get_teleport_threshold() const {
    return double(read(NetwMultiplayer::PREDICT_PARAM_TELEPORT_THRESHOLD));
}

int NetwPrediction::get_correction_cooldown_ticks() const {
    return int(read(NetwMultiplayer::PREDICT_PARAM_CORRECTION_COOLDOWN_TICKS));
}

bool NetwPrediction::get_repeat_missing_input() const {
    return bool(read(NetwMultiplayer::PREDICT_PARAM_REPEAT_MISSING_INPUT));
}

int NetwPrediction::get_max_inputs_per_tick() const {
    return int(read(NetwMultiplayer::PREDICT_PARAM_MAX_INPUTS_PER_TICK));
}

int NetwPrediction::get_input_buffer_ticks() const {
    return int(read(NetwMultiplayer::PREDICT_PARAM_INPUT_BUFFER_TICKS));
}

int NetwPrediction::get_max_input_lag_ticks() const {
    return int(read(NetwMultiplayer::PREDICT_PARAM_MAX_INPUT_LAG_TICKS));
}

double NetwPrediction::get_correction_threshold() const {
    return double(read(NetwMultiplayer::PREDICT_PARAM_CORRECTION_THRESHOLD));
}

bool NetwPrediction::get_rollback_together() const {
    return bool(read(NetwMultiplayer::PREDICT_PARAM_ROLLBACK_TOGETHER));
}

void NetwPrediction::notify_collision() {
    const Ref<NetwEntity> bound = get_entity();
    NetwMultiplayer *home
        = bound.is_valid() ? bound->home_session() : nullptr;
    if (home != nullptr && home->predict_is_declared(bound->get_rid_handle())) {
        home->predict_notify_collision(bound->get_rid_handle());
    }
}

void NetwPrediction::set_predict_input(const Callable &p_predictor) {
    const Ref<NetwEntity> bound = get_entity();
    if (bound.is_null()) {
        return;
    }
    NetwMultiplayer::predict_write_input(
        *bound->get_record(),
        bound->get_owner(),
        p_predictor
    );
}

Callable NetwPrediction::get_predict_input() const {
    const Ref<NetwEntity> bound = get_entity();
    const predict::Decl *decl = bound.is_valid()
        ? bound->get_record()->find_predict_declaration()
        : nullptr;
    return decl != nullptr ? decl->input : Callable();
}

void NetwPrediction::_bind_methods() {
    ClassDB::bind_method(
        D_METHOD("notify_collision"),
        &NetwPrediction::notify_collision
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
    ClassDB::bind_method(D_METHOD("get_mode"), &NetwPrediction::get_mode);
    ClassDB::bind_method(
        D_METHOD("set_mode", "value"),
        &NetwPrediction::set_mode
    );
    ADD_PROPERTY(
        PropertyInfo(
            Variant::INT,
            "mode",
            PROPERTY_HINT_ENUM,
            "Rollback:0,Offset:1,Disabled:2,Auto:4",
            PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_CLASS_IS_ENUM,
            "NetwPrediction.Mode"
        ),
        "set_mode",
        "get_mode"
    );
    ClassDB::bind_method(
        D_METHOD("get_pause_on_unpredicted_contact"),
        &NetwPrediction::get_pause_on_unpredicted_contact
    );
    ClassDB::bind_method(
        D_METHOD("set_pause_on_unpredicted_contact", "value"),
        &NetwPrediction::set_pause_on_unpredicted_contact
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::BOOL, "pause_on_unpredicted_contact"),
        "set_pause_on_unpredicted_contact",
        "get_pause_on_unpredicted_contact"
    );
    ClassDB::bind_method(
        D_METHOD("get_world_version"),
        &NetwPrediction::get_world_version
    );
    ClassDB::bind_method(
        D_METHOD("set_world_version", "value"),
        &NetwPrediction::set_world_version
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::INT, "world_version"),
        "set_world_version",
        "get_world_version"
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
        D_METHOD("get_correction_cooldown_ticks"),
        &NetwPrediction::get_correction_cooldown_ticks
    );
    ClassDB::bind_method(
        D_METHOD("set_correction_cooldown_ticks", "value"),
        &NetwPrediction::set_correction_cooldown_ticks
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::INT, "correction_cooldown_ticks"),
        "set_correction_cooldown_ticks",
        "get_correction_cooldown_ticks"
    );
    ClassDB::bind_method(
        D_METHOD("get_repeat_missing_input"),
        &NetwPrediction::get_repeat_missing_input
    );
    ClassDB::bind_method(
        D_METHOD("set_repeat_missing_input", "value"),
        &NetwPrediction::set_repeat_missing_input
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::BOOL, "repeat_missing_input"),
        "set_repeat_missing_input",
        "get_repeat_missing_input"
    );
    ClassDB::bind_method(
        D_METHOD("get_max_inputs_per_tick"),
        &NetwPrediction::get_max_inputs_per_tick
    );
    ClassDB::bind_method(
        D_METHOD("set_max_inputs_per_tick", "value"),
        &NetwPrediction::set_max_inputs_per_tick
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::INT, "max_inputs_per_tick"),
        "set_max_inputs_per_tick",
        "get_max_inputs_per_tick"
    );
    ClassDB::bind_method(
        D_METHOD("get_input_buffer_ticks"),
        &NetwPrediction::get_input_buffer_ticks
    );
    ClassDB::bind_method(
        D_METHOD("set_input_buffer_ticks", "value"),
        &NetwPrediction::set_input_buffer_ticks
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::INT, "input_buffer_ticks"),
        "set_input_buffer_ticks",
        "get_input_buffer_ticks"
    );
    ClassDB::bind_method(
        D_METHOD("get_max_input_lag_ticks"),
        &NetwPrediction::get_max_input_lag_ticks
    );
    ClassDB::bind_method(
        D_METHOD("set_max_input_lag_ticks", "value"),
        &NetwPrediction::set_max_input_lag_ticks
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::INT, "max_input_lag_ticks"),
        "set_max_input_lag_ticks",
        "get_max_input_lag_ticks"
    );
    ClassDB::bind_method(
        D_METHOD("get_correction_threshold"),
        &NetwPrediction::get_correction_threshold
    );
    ClassDB::bind_method(
        D_METHOD("set_correction_threshold", "value"),
        &NetwPrediction::set_correction_threshold
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::FLOAT, "correction_threshold"),
        "set_correction_threshold",
        "get_correction_threshold"
    );
    ClassDB::bind_method(
        D_METHOD("get_rollback_together"),
        &NetwPrediction::get_rollback_together
    );
    ClassDB::bind_method(
        D_METHOD("set_rollback_together", "value"),
        &NetwPrediction::set_rollback_together
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::BOOL, "rollback_together"),
        "set_rollback_together",
        "get_rollback_together"
    );

    ClassDB::bind_method(
        D_METHOD("get_correction_count"),
        &NetwPrediction::get_correction_count
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::INT, "correction_count"),
        godot::String(),
        "get_correction_count"
    );

    BIND_ENUM_CONSTANT(MODE_ROLLBACK);
    BIND_ENUM_CONSTANT(MODE_OFFSET);
    BIND_ENUM_CONSTANT(MODE_DISABLED);
    BIND_ENUM_CONSTANT(MODE_AUTO);

    ADD_SIGNAL(MethodInfo(
        "corrected",
        PropertyInfo(Variant::INT, "tick"),
        PropertyInfo(Variant::DICTIONARY, "deltas"),
        PropertyInfo(Variant::BOOL, "teleported")
    ));
}

Ref<NetwPrediction> build_prediction_handle(Object *p_entity) {
    Ref<NetwPrediction> made;
    made.instantiate();
    made->bind_entity(Ref<NetwEntity>(Object::cast_to<NetwEntity>(p_entity)));
    return made;
}

} // namespace netw
