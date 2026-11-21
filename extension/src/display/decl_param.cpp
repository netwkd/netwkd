#include "netw/display/decl_param.hpp"

#include "netw/fault_book.hpp"
#include "netw/log.hpp"

using namespace godot;

namespace netw::display {

namespace {

bool ordinal_names_one(int value, int limit, const char *what) {
    if (value < 0 || value >= limit) {
        fault::raise(
            nullptr,
            NETW_FAULT_BAD_VALUE,
            {String("The display ") + String(what),
             String::num_int64(value),
             String("one of its values")}
        );
        return false;
    }
    return true;
}

} // namespace

Error set_param(Decl &decl, int param, const Variant &value) {
    switch (param) {
        case PARAM_ROLE: {
            const int ordinal = int(value);
            if (!ordinal_names_one(ordinal, ROLE_MAX, "role")) {
                return ERR_INVALID_PARAMETER;
            }
            decl.display_role = ordinal;
            return OK;
        }
        case PARAM_LIVE_MODE: {
            const int ordinal = int(value);
            if (!ordinal_names_one(ordinal, LIVE_MODE_MAX, "live mode")) {
                return ERR_INVALID_PARAMETER;
            }
            decl.live_mode = ordinal;
            return OK;
        }
        case PARAM_TIMELINE_MODE: {
            const int ordinal = int(value);
            if (!ordinal_names_one(
                    ordinal,
                    TIMELINE_MODE_MAX,
                    "timeline mode"
                )) {
                return ERR_INVALID_PARAMETER;
            }
            decl.timeline_mode = ordinal;
            return OK;
        }
        case PARAM_VISUAL_ROOT: {
            const Variant::Type type = value.get_type();
            if (type != Variant::NODE_PATH && type != Variant::STRING
                && type != Variant::STRING_NAME) {
                fault::raise(
                    nullptr,
                    NETW_FAULT_BAD_VALUE,
                    {String("{NetwDisplay.visual_root}"),
                     String("a ") + Variant::get_type_name(type),
                     String("a NodePath")}
                );
                return ERR_INVALID_PARAMETER;
            }
            decl.visual_root = value.operator NodePath();
            return OK;
        }
        case PARAM_LIVE_SMOOTH_TIME:
            decl.live_smooth_time = double(value);
            return OK;
        case PARAM_CHASE_GLIDE_TIME:
            decl.chase_glide_time = double(value);
            return OK;
        case PARAM_MAX_FORECAST_TICKS:
            decl.max_forecast_ticks = int(value);
            return OK;
        case PARAM_SMART_DILATION:
            decl.enable_smart_dilation = bool(value);
            return OK;
        case PARAM_LAG_ADAPT_RATE:
            decl.lag_adapt_rate = double(value);
            return OK;
        case PARAM_FLOOR_SMOOTHING:
            decl.floor_smoothing = double(value);
            return OK;
        case PARAM_TRACE_INTERVAL:
            decl.trace_interval = int(value);
            return OK;
        default:
            break;
    }
    fault::raise(
        nullptr,
        NETW_FAULT_BAD_VALUE,
        {String("The display settings"),
         String("the setting ") + String::num_int64(param),
         String("a setting that exists")}
    );
    return ERR_INVALID_PARAMETER;
}

Variant get_param(const Decl &decl, int param) {
    switch (param) {
        case PARAM_ROLE:
            return decl.display_role;
        case PARAM_LIVE_MODE:
            return decl.live_mode;
        case PARAM_LIVE_SMOOTH_TIME:
            return decl.live_smooth_time;
        case PARAM_CHASE_GLIDE_TIME:
            return decl.chase_glide_time;
        case PARAM_TIMELINE_MODE:
            return decl.timeline_mode;
        case PARAM_MAX_FORECAST_TICKS:
            return decl.max_forecast_ticks;
        case PARAM_SMART_DILATION:
            return decl.enable_smart_dilation;
        case PARAM_LAG_ADAPT_RATE:
            return decl.lag_adapt_rate;
        case PARAM_FLOOR_SMOOTHING:
            return decl.floor_smoothing;
        case PARAM_TRACE_INTERVAL:
            return decl.trace_interval;
        case PARAM_VISUAL_ROOT:
            return decl.visual_root;
        default:
            break;
    }
    fault::raise(
        nullptr,
        NETW_FAULT_BAD_VALUE,
        {String("The display settings"),
         String("the setting ") + String::num_int64(param),
         String("a setting that exists")}
    );
    return Variant();
}

} // namespace netw::display
