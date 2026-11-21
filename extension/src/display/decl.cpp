#include "netw/sealed.hpp"

#include "netw/display/decl.hpp"

namespace netw::display {

Dirt dirt_of(int param) {
    switch (param) {
        case PARAM_ROLE:
        case PARAM_LIVE_MODE:
            return DIRT_ROLE;
        case PARAM_VISUAL_ROOT:
        case PARAM_LIVE_SMOOTH_TIME:
            return DIRT_RUNTIME;
        default:
            return DIRT_NONE;
    }
}

bool pump_is_predicted(int pump) {
    return pump == PUMP_CHASE || pump == PUMP_BRACKETED;
}

bool pump_clears_history(int previous, int next) {
    if (previous == PUMP_CHASE && next == PUMP_REMOTE) {
        return false;
    }
    return pump_is_predicted(previous) || pump_is_predicted(next);
}

bool pump_arms_offsets(int previous, int next) {
    return previous != PUMP_UNRESOLVED && previous != PUMP_DISABLED
        && next != PUMP_DISABLED;
}

int Decl::pump_for(int role) const {
    switch (role) {
        case ROLE_PREDICTED:
            return live_mode == LIVE_BRACKETED ? PUMP_BRACKETED : PUMP_CHASE;
        case ROLE_AUTHORITY:
            return PUMP_BRACKETED;
        case ROLE_REMOTE:
            return PUMP_REMOTE;
        default:
            return PUMP_DISABLED;
    }
}

} // namespace netw::display
