#include "netw/display/roles.hpp"

#include "godot/callable.hpp"
#include "godot/node.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/colors.hpp"
#include "netw/display/build.hpp"
#include "netw/display/channel.hpp"
#include "netw/display/history.hpp"
#include "netw/display/role_facts.hpp"
#include "netw/fault_book.hpp"
#include "netw/log.hpp"
#include "netw/profile.hpp"
#include "netw/subsystems.hpp"

using namespace godot;

namespace netw::display {

bool owner_is_solver_body(Node *p_owner) {
    return p_owner != nullptr
        && (p_owner->is_class("RigidBody2D")
            || p_owner->is_class("RigidBody3D"));
}

void warn_self_feedback(Runtime *p_runtime) {
    const LocalVector<Channel *> &states = p_runtime->channels();
    for (int at = 0; at < int(states.size()); ++at) {
        Channel *state = states[at];
        if (!state->get_self_feedback()) {
            continue;
        }
        Node *owner = p_runtime->owner();
        const String named
            = owner != nullptr ? String(owner->get_name()) : String("?");
        fault::raise(
            NetwMultiplayer::session_holding(owner),
            NETW_FAULT_INTERPOLATION_IGNORED,
            {named + String(":") + String(state->get_target_prop()),
             String(),
             String(),
             0,
             NETW_INTERPOLATION_SIMULATED_HERE},
            gd::instance_id(owner)
        );
        return;
    }
}

namespace {

int role_verdict(Runtime *p_runtime, const Hooks &p_hooks) {
    const Decl &config = p_runtime->get_config();
    if (config.display_role != ROLE_AUTO) {
        return config.display_role;
    }
    const RID entity = p_runtime->entity_rid();
    return p_hooks.role_of(entity, p_hooks.authors(entity));
}

} // namespace

void resolve_role(Runtime *p_runtime, const Hooks &p_hooks) {
    int role = 0;
    {
        NETW_ZONE_NC("display role verdict", colors::INTERP);
        role = role_verdict(p_runtime, p_hooks);
    }
    const Decl &config = p_runtime->get_config();
    const int pump = config.pump_for(role);
    p_runtime->set_role(role);
    if (pump == p_runtime->get_pump_mode()) {
        return;
    }
    const int previous = int(p_runtime->get_pump_mode());
    const RID entity = p_runtime->entity_rid();
    p_runtime->set_display_offset_limit(p_hooks.clamp_for(entity));
    p_runtime->display_playhead().forget_arrivals();

    const LocalVector<Channel *> &states = p_runtime->channels();
    if (netw::display::pump_clears_history(previous, pump)) {
        for (int at = 0; at < int(states.size()); ++at) {
            Channel *state = states[at];
            state->display_history().clear();
        }
    }
    if (netw::display::pump_arms_offsets(previous, pump)) {
        for (int at = 0; at < int(states.size()); ++at) {
            Channel *state = states[at];
            state->render_offset().armed = !state->get_self_feedback();
        }
    }

    p_runtime->set_pump_mode(pump);
    if (previous == netw::display::PUMP_DISABLED) {
        const LocalVector<Channel *> &order = p_runtime->write_order();
        for (int at = 0; at < int(order.size()); ++at) {
            Channel *state = order[at];
            state->set_last_written(state->current_source_value());
            state->write(state->get_last_written());
        }
    }
    if (netw::display::pump_is_predicted(pump)) {
        warn_self_feedback(p_runtime);
    }
    if (pump == netw::display::PUMP_REMOTE) {
        p_hooks.compute_sync_intervals(entity);
    }
    p_hooks.chase_hook(entity, pump == netw::display::PUMP_CHASE);
    if (pump == netw::display::PUMP_DISABLED) {
        for (int at = 0; at < int(states.size()); ++at) {
            Channel *state = states[at];
            state->render_offset().clear();
        }
    }
}

} // namespace netw::display
