#pragma once

#include <cstdint>

#include "godot/callable.hpp"
#include "godot/object.hpp"
#include "godot/ref_counted.hpp"
#include "godot/variant.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/predict_stats.hpp"
#include "netw/api/simulation.hpp"

namespace netw {

class NetwPrediction : public godot::RefCounted {
    GDCLASS(NetwPrediction, godot::RefCounted)

public:
    static constexpr double DEFAULT_TELEPORT_THRESHOLD = 2.0;

    enum Role {
        ROLE_PREDICT = 0,
        ROLE_CONSUME = 1,
        ROLE_HOST_LOCAL = 2,
        ROLE_REMOTE = 3,
        ROLE_SIMULATE = 4,
    };

    enum InputSource {
        INPUT_SOURCE_LOCAL = 0,
        INPUT_SOURCE_RECEIVED = 1,
        INPUT_SOURCE_PREDICTED = 2,
        INPUT_SOURCE_NONE = 3,
    };

    enum SimMode {
        SIM_MODE_AUTHORITATIVE = 0,
        SIM_MODE_SPECULATIVE = 1,
        SIM_MODE_DISPLAY = 2,
    };

    enum ContactClass {
        CONTACT_CLASS_NONE = 0,
        CONTACT_CLASS_DECLARED_SUPPORT = 1,
        CONTACT_CLASS_OTHER_STATIC = 2,
        CONTACT_CLASS_PREDICTED_DYNAMIC = 3,
        CONTACT_CLASS_UNPREDICTED_DYNAMIC = 4,
        CONTACT_CLASS_KINEMATIC_PROXY = 5,
    };

    enum CommandOrigin {
        COMMAND_ORIGIN_PREDICTED = 0,
        COMMAND_ORIGIN_RELAYED = 1,
    };

    enum DriveKind : int {
        DRIVE_KIND_NONE = 0,
        DRIVE_KIND_FRESH = 1,
        DRIVE_KIND_REPEAT = 2,
        DRIVE_KIND_HOLD = 3,
        DRIVE_KIND_STARVED = 4,
        DRIVE_KIND_FOLD_DRIVE = 5,
        DRIVE_KIND_MISSING = 6,
        DRIVE_KIND_SUBSTITUTED = 7,
    };

    enum TriggerShape {
        TRIGGER_SHAPE_NONE = 0,
        TRIGGER_SHAPE_MIXED = 1,
        TRIGGER_SHAPE_ALL_WITHHELD = 2,
    };

    enum ConsumeAction {
        CONSUME_ACTION_REPLAY = 0,
        CONSUME_ACTION_HOLD = 1,
        CONSUME_ACTION_STARVED = 2,
    };

    enum ExactVerdict {
        EXACT_VERDICT_UNJUDGED = 0,
        EXACT_VERDICT_EQUAL = 1,
        EXACT_VERDICT_UNEQUAL = 2,
    };

    enum Mode {
        MODE_ROLLBACK = 0,
        MODE_OFFSET = 1,
        MODE_DISABLED = 2,
        MODE_OBSERVE = 3,
        MODE_AUTO = 4,
    };

    enum EpisodeState {
        EPISODE_STATE_OPEN = 0,
        EPISODE_STATE_CLOSED = 1,
        EPISODE_STATE_FALLBACK = 2,
    };

    enum EpisodeChange {
        EPISODE_CHANGE_OPENED = 0,
        EPISODE_CHANGE_FALLBACK = 1,
        EPISODE_CHANGE_CLOSED = 2,
    };

    enum OperatorOutcome {
        OPERATOR_OUTCOME_PENDING = 0,
        OPERATOR_OUTCOME_CONTRACTED = 1,
        OPERATOR_OUTCOME_FAILED_TO_CONTRACT = 2,
        OPERATOR_OUTCOME_INTRODUCED_BOUNDARY = 3,
        OPERATOR_OUTCOME_WITHHELD = 4,
    };

    enum Attribution {
        ATTRIBUTION_UNKNOWN = 0,
        ATTRIBUTION_PRE_STATE = 1,
        ATTRIBUTION_COMMAND = 2,
        ATTRIBUTION_ENVIRONMENT = 3,
        ATTRIBUTION_TOPOLOGY = 4,
        ATTRIBUTION_EXECUTION = 5,
        ATTRIBUTION_CONTACT = 6,
        ATTRIBUTION_CLOSURE = 7,
    };

    enum Operator {
        OPERATOR_NONE = 0,
        OPERATOR_REBASE_PROJECTED = 1,
        OPERATOR_REBASE_EXACT = 2,
        OPERATOR_FULL_CLOSURE = 3,
        OPERATOR_TRANSPORT_DELTA = 4,
        OPERATOR_RESEED = 5,
        OPERATOR_RESEED_ALIGN = 6,
        OPERATOR_DEMOTE = 7,
        OPERATOR_DISSIPATE = 8,
        OPERATOR_JOINT_REBASE = 9,
    };

    enum Domain {
        DOMAIN_IN = 0,
        DOMAIN_OUT = 1,
    };

private:
    godot::ObjectID entity_id;

    void write(int p_param, const godot::Variant &p_value);
    godot::Variant read(int p_param) const;

protected:
    static void _bind_methods();

public:
    void bind_entity(const godot::Ref<NetwEntity> &p_entity);

    godot::Ref<NetwEntity> get_entity() const;
    bool is_registered() const;

    void set_mode(NetwPrediction::Mode p_value);
    void set_pause_on_unpredicted_contact(bool p_value);
    void set_world_version(int64_t p_value);
    void set_correction_path_clear(const godot::Callable &p_value);
    void set_teleport_threshold(double p_value);
    void set_correction_cooldown_ticks(int p_value);
    void set_repeat_missing_input(bool p_value);
    void set_max_inputs_per_tick(int p_value);
    void set_input_buffer_ticks(int p_value);
    void set_max_input_lag_ticks(int p_value);
    void set_correction_threshold(double p_value);
    void set_rollback_together(bool p_value);

    NetwPrediction::Mode get_mode() const;
    bool get_pause_on_unpredicted_contact() const;
    int64_t get_world_version() const;
    godot::Callable get_correction_path_clear() const;
    double get_teleport_threshold() const;
    int get_correction_cooldown_ticks() const;
    bool get_repeat_missing_input() const;
    int get_max_inputs_per_tick() const;
    int get_input_buffer_ticks() const;
    int get_max_input_lag_ticks() const;
    double get_correction_threshold() const;
    bool get_rollback_together() const;

    int64_t get_correction_count() const;

    void notify_collision();

    void set_predict_input(const godot::Callable &p_predictor);
    godot::Callable get_predict_input() const;
};

godot::Ref<NetwPrediction> build_prediction_handle(godot::Object *p_entity);

} // namespace netw

VARIANT_ENUM_CAST(netw::NetwPrediction::Mode);
