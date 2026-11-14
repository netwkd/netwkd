#pragma once

#include "netw/prediction_core.hpp"

namespace netw::predict {

enum class InputSource : int {
    LOCAL = 0,
    RECEIVED = 1,
    PREDICTED = 2,
    NONE = 3,
};

enum class SimMode : int {
    AUTHORITATIVE = 0,
    SPECULATIVE = 1,
    DISPLAY = 2,
};

struct KindDefaults {
    bool repeat_missing_input = false;
    double teleport_threshold = 2.0;
    bool restore_extrapolated = false;
    bool tolerates_quantizer_steps = false;
    bool gates_physics = false;
    bool samples_contacts = false;
};

KindDefaults kind_defaults(bool p_physics_driven);

int role_for_axes(int p_input_source, int p_sim_mode);

int correction_for_mode(int p_policy);

int resolve_mode(int p_policy, bool p_solves, int p_schedule);

bool replays_without_integration(int p_schedule, bool p_solves);

int integrable_mode(int p_policy, int p_schedule, bool p_solves);

int integrable_correction(int p_correction, int p_schedule, bool p_solves);

} // namespace netw::predict
