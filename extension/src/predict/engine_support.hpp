#pragma once

#include "netw/predict/engine.hpp"

#include "netw/api/predict_slot_engine.hpp"
#include "godot/engine.hpp"
#include "netw/log.hpp"
#include "netw/predict/decl.hpp"

namespace netw {

constexpr int ACK_WINDOW_MAX = 64;

inline bool recovery_request_in_range(
    int p_policy,
    int p_domain,
    int p_attribution,
    double p_tick_delta
) {
    NETW_BUG_COND_V(
        p_policy < int(RecoveryPolicy::REBASE_REPLAY)
            || p_policy > int(RecoveryPolicy::OBSERVE),
        false
    );
    NETW_BUG_COND_V(
        p_domain < int(predict::Domain::IN_DOMAIN)
            || p_domain > int(predict::Domain::OUT_OF_DOMAIN),
        false
    );
    NETW_BUG_COND_V(
        p_attribution < int(predict::Attribution::UNKNOWN)
            || p_attribution > int(predict::Attribution::CLOSURE),
        false
    );
    NETW_BUG_COND_V(p_tick_delta <= 0.0, false);
    return true;
}

inline uint64_t subject_of(godot::Node *p_node) {
    return uint64_t(gd::instance_id(p_node));
}

inline godot::String subject_name(godot::Node *p_node) {
    godot::Node *node = p_node;
    return node != nullptr ? godot::String(node->get_name())
                           : godot::String("an entity with no node");
}

inline void note_errors(
    predict::Slot &p_row,
    predict::FieldReadings &p_into,
    const godot::LocalVector<double> &p_errors
) {
    p_into.resize(p_row.wiring.count());
    const uint32_t count
        = std::min(p_errors.size(), uint32_t(p_row.wiring.count()));
    for (uint32_t at = 0; at < count; ++at) {
        if (p_errors[at] >= 0.0) {
            p_into.note(int(at), p_errors[at]);
        }
    }
}

inline void fill_correction_tolerances(
    const predict::Slot &p_row,
    double p_fallback_epsilon,
    godot::LocalVector<double> &r_out
) {
    const uint32_t count = uint32_t(p_row.wiring.count());
    if (r_out.size() != count) {
        r_out.resize(count);
    }
    for (uint32_t field = 0; field < count; ++field) {
        if (p_row.wiring.vote_exclude[field] != 0) {
            r_out[field] = -1.0;
            continue;
        }
        const double declared = p_row.wiring.epsilon[field];
        r_out[field] = declared >= 0.0 ? declared : p_fallback_epsilon;
    }
}

inline bool reports_to_test([[maybe_unused]] const predict::Decl *p_decl) {
#if defined(NETW_TESTS)
    return p_decl != nullptr && bool(p_decl->test_report);
#else
    return false;
#endif
}

} // namespace netw
