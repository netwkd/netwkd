#pragma once

#include <cstdint>

#include "godot/ref_counted.hpp"
#include "godot/variant.hpp"

#if defined(NETW_MODULE)
#include "core/object/script_backtrace.h"
#include "core/object/script_language.h"

namespace godot {
using ::ScriptBacktrace;
} // namespace godot
#elif defined(NETW_GDEXTENSION)
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/script_backtrace.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#else
#error "Define NETW_MODULE or NETW_GDEXTENSION."
#endif

namespace netw::gd {

struct ScriptSite {
    godot::String file;
    godot::String function;
    int32_t line = 0;
};

inline ScriptSite capture_script_site(const godot::String &p_skip_prefix) {
#if defined(NETW_MODULE)
    const Vector<Ref<ScriptBacktrace>> traces
        = ScriptServer::capture_script_backtraces(false);
#else
    const godot::TypedArray<godot::ScriptBacktrace> traces
        = godot::Engine::get_singleton()->capture_script_backtraces(false);
#endif
    for (int64_t at = 0; at < traces.size(); ++at) {
        const godot::Ref<godot::ScriptBacktrace> trace = traces[at];
        if (trace.is_null()) {
            continue;
        }
        for (int frame = 0; frame < trace->get_frame_count(); ++frame) {
            const godot::String file = trace->get_frame_file(frame);
            if (file.is_empty() || file.begins_with(p_skip_prefix)) {
                continue;
            }
            return {
                file,
                trace->get_frame_function(frame),
                int32_t(trace->get_frame_line(frame))
            };
        }
    }
    return {};
}

} // namespace netw::gd
