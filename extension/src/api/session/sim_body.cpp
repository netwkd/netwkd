#include "netw/api/netw_multiplayer.hpp"

#include "godot/callable.hpp"
#include "godot/node.hpp"
#include "godot/object.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/replication_core.hpp"
#include "netw/display/book.hpp"
#include "netw/display/roles.hpp"
#include "netw/sim/body.hpp"
#include "netw/sim/row.hpp"
#include "netw/sync_authoring.hpp"

using namespace godot;

namespace netw {

namespace {

const StringName &network_process_name() {
    static const StringName name("_network_process");
    return name;
}

} // namespace

sim::Facts NetwMultiplayer::sim_body_facts(
    NetwEntityRecord &p_record,
    Node *p_owner
) {
    const entity::Control *control = p_record.get_control();
    const int64_t local = p_owner != nullptr ? sim_local_peer() : 0;
    const bool here = control->controlled_by(local);
    sim::Facts facts;
    facts.declared = true;
    facts.state_rows = entity_record_binding(
                           &p_record,
                           p_owner,
                           repl::SET_RECORD_STATE,
                           p_record.get_handle()
                       )
        != nullptr;
    facts.server_here = is_host();
    facts.controller_is_nobody = control->resolve() == entity::Control::SERVER;
    facts.controller_here = !facts.controller_is_nobody && here;
    facts.pending_claim_here = control->runs_ahead() && !here;
    return facts;
}

void NetwMultiplayer::sim_follow_claim(const RID &p_entity) {
    sim_settle_body(p_entity);
    display_mark_role_dirty(p_entity);
}

void NetwMultiplayer::sim_settle_body(const RID &p_entity) {
    NetwEntityRecord *record = entity_record(p_entity);
    Node *owner = wrapper_owner(p_entity);
    const RID entity = p_entity;
    if (record == nullptr || owner == nullptr || !owner->is_inside_tree()
        || !entity.is_valid()) {
        return;
    }
    const sim::Row *standing = sim_rows.row_of(entity);
    const bool declares_bodies
        = standing != nullptr && !standing->declaration.bodies.is_empty();
    const bool implicit = sim::is_solver_body(owner)
        && display_book.runtime_of(entity) != nullptr;
    if (!declares_bodies && !implicit) {
        return;
    }
    const bool seated = standing != nullptr && standing->facts.predicted;
    const sim::Mode settled
        = standing != nullptr ? standing->mode : sim::Mode::NONE;
    sim::Mode mode = sim::Mode::NONE;
    if (seated) {
        mode = standing->mode;
    } else if (
        predict_engine_for(entity) != nullptr
        || authoring::declares_prediction(owner)
    ) {
        return;
    } else {
        mode = sim_rows.resolve(entity, sim_body_facts(*record, owner));
    }
    sim::Row &row = sim_rows.ensure(entity);
    if (!row.bodies.recorded) {
        sim::record_authored(
            row.bodies,
            owner,
            row.declaration.bodies,
            display_config_for(entity, record).visual_root
        );
        row.bodies.residents = &store_pull.residents;
        row.owner_ticks = owner->has_method(network_process_name());
        row.follows_control = true;
        sim_watch_sleep(row, true);
        if (display::Runtime *runtime = display_book.runtime_of(entity)) {
            display_seat_bodies(runtime);
        }
    }
    if (sim::transition(row.bodies, mode)
        && (!seated || mode == sim::Mode::ACTIVE)) {
        sim_reinstall(entity);
    }
    if (!seated) {
        sim_announce(entity);
    }
    if (row.mode != settled) {
        display_follow_mode(entity);
    }
}

void NetwMultiplayer::display_follow_mode(const RID &p_entity) {
    display::Runtime *runtime = display_book.runtime_of(p_entity);
    if (runtime == nullptr) {
        return;
    }
#if defined(NETW_TESTS)
    idle_work.role_resolves += 1;
#endif
    display::resolve_role(runtime, display_hooks);
    display_book.wake(runtime);
}

void NetwMultiplayer::sim_settle_body_of(const RID &p_entity) {
    if (entity_get_view(p_entity).is_valid()) {
        sim_settle_body(p_entity);
    }
}

void NetwMultiplayer::sim_follow_control(const RID &p_entity) {
    const sim::Row *row = sim_rows.row_of(p_entity);
    if (row != nullptr && row->follows_control) {
        sim_settle_body_of(p_entity);
    }
}

void NetwMultiplayer::sim_reinstall(const RID &p_entity) {
    ReplicationCore *plane = get_replication_plane();
    const NetwEntityRecord *record = entity_record(p_entity);
    if (plane == nullptr || record == nullptr) {
        return;
    }
    const sim::Row *row = sim_rows.row_of(p_entity);
    const bool buffered = row != nullptr && row->mode == sim::Mode::ACTIVE
        && row->declaration.restore == sim::Restore::BUFFERED;
    const int64_t display = buffered ? sim_display_tick() : -1;
    const int64_t live = liveness_core.route_of(p_entity);
    const int64_t route = live > 0 ? live : record->get_route();
    const LocalVector<repl::PropertyBindingPtr> bindings
        = plane->derived_group(route);
    for (const repl::PropertyBindingPtr &binding : bindings) {
        if (binding != nullptr) {
            store_reinstall(binding, display);
        }
    }
}

void NetwMultiplayer::sim_release_body(const RID &p_entity, bool p_restore) {
    sim::Row *row = sim_rows.find(p_entity);
    if (row == nullptr) {
        return;
    }
    sim_watch_sleep(*row, false);
    if (p_restore) {
        sim::release(row->bodies);
    } else {
        row->bodies = sim::Bodies();
    }
    row->follows_control = false;
    if (!row->facts.predicted) {
        sim_release_row(p_entity);
    }
}

void NetwMultiplayer::sim_hold_poses(
    const RID &p_space,
    const LocalVector<RID> &p_members,
    LocalVector<sim::HeldPose> &r_poses
) const {
    sim_rows.hold_poses(p_space, p_members, r_poses);
}

} // namespace netw
