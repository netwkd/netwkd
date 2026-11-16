#include "netw/api/netw_multiplayer.hpp"

#include "godot/node.hpp"
#include "godot/object.hpp"
#include "godot/utility.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/entity_record.hpp"
#include "netw/api/interest.hpp"
#include "netw/api/lifecycle_request.hpp"
#include "netw/api/persistence.hpp"
#include "netw/api/prediction.hpp"
#include "netw/api/simulation.hpp"
#include "netw/colors.hpp"
#include "netw/entity/stage.hpp"
#include "netw/fault_book.hpp"
#include "netw/interest/decl.hpp"
#include "netw/log.hpp"
#include "netw/persist/binding.hpp"
#include "netw/predict/decl.hpp"
#include "netw/profile.hpp"
#include "netw/spawn/record.hpp"
#include "netw/tree_hook.hpp"

using namespace godot;
using namespace netw;

namespace netw {

namespace {

void collect_despawning_parts(
    Node *p_node,
    LocalVector<Ref<NetwEntity>> &r_parts
) {
    for (int at = 0; at < p_node->get_child_count(); ++at) {
        Node *child = p_node->get_child(at);
        const Ref<NetwEntity> part = tree::entity_object_at(child) != nullptr
            ? NetwEntity::of(child)
            : Ref<NetwEntity>();
        if (part.is_valid() && part->get_route() > 0
            && part->get_on_parent_despawn()
                == NetwEntity::PARENT_DESPAWN_DETACH) {
            continue;
        }
        collect_despawning_parts(child, r_parts);
        if (part.is_valid()) {
            r_parts.push_back(part);
        }
    }
}

} // namespace

void NetwMultiplayer::entity_announce(Object *p_wrapper, Announcement p_row) {
    NetwEntity *entity = Object::cast_to<NetwEntity>(p_wrapper);
    if (entity == nullptr) {
        return;
    }
    p_row.entity = entity->get_rid_handle();
    NetwMultiplayer *home
        = entity->home != nullptr ? entity->home : entity->session_core();
    if (p_row.kind == AnnouncementKind::CONTROL_CHANGED && home != nullptr
        && p_row.entity.is_valid()) {
        home->control_follow(p_row.entity);
    }
    if (home == nullptr || !p_row.entity.is_valid()
        || home->slot_pinned_entity(p_row.entity).ptr() != entity) {
        entity_emit(entity, p_row);
        return;
    }
    home->announce(p_row);
}

void NetwMultiplayer::entity_emit(
    NetwEntity *p_entity,
    const Announcement &p_row
) {
    if (p_entity == nullptr) {
        return;
    }
    switch (p_row.kind) {
        case AnnouncementKind::CONTROL_CHANGED:
            p_entity->emit_signal(
                StringName("control_changed"),
                p_row.from,
                p_row.to
            );
            break;
        case AnnouncementKind::DESPAWNING:
            p_entity->emit_signal(
                StringName("despawning"),
                StringName(p_row.reason)
            );
            break;
        case AnnouncementKind::DESPAWNED:
            p_entity->emit_signal(StringName("despawned"));
            break;
        case AnnouncementKind::LIFECYCLE_REFUSED: {
            const Ref<NetwLifecycleRequest> request
                = NetwLifecycleRequest::create(
                    lifecycle::Kind(p_row.code),
                    p_row.requester,
                    Ref<NetwEntity>(p_entity),
                    Object::cast_to<Node>(gd::object_of(p_row.node))
                );
            request->deny(
                p_row.reason,
                p_row.refusal != OK ? p_row.refusal : ERR_UNAUTHORIZED
            );
            p_entity->emit_signal(StringName("lifecycle_refused"), request);
        } break;
        case AnnouncementKind::REPARENTED:
            p_entity->emit_signal(StringName("reparented"));
            break;
        case AnnouncementKind::HIDDEN:
            p_entity->emit_signal(StringName("hidden"));
            break;
        case AnnouncementKind::MODE_CHANGED: {
            const Ref<NetwSimulation> handle
                = p_entity->simulation_if_minted();
            if (handle.is_valid()) {
                handle->emit_signal(
                    StringName("mode_changed"),
                    NetwSimulation::Mode(int(p_row.from)),
                    NetwSimulation::Mode(int(p_row.to))
                );
            }
        } break;
        case AnnouncementKind::STATE_EVALUATED:
        case AnnouncementKind::EPISODE_CHANGED:
        case AnnouncementKind::RECOVERED:
        case AnnouncementKind::DIVERGENCE_DETECTED:
            prediction_emit(p_entity->get_prediction().ptr(), p_row);
            break;
        case AnnouncementKind::PERSIST_LOADED:
        case AnnouncementKind::PERSIST_SAVED: {
            const NetwEntityRecord *record = record_of_wrapper(p_entity);
            const Ref<NetwPersistence> view = record != nullptr
                ? Ref<NetwPersistence>(
                      record->minted_part(NetwEntityRecord::PART_PERSISTENCE)
                  )
                : Ref<NetwPersistence>();
            if (view.is_valid()
                && p_row.kind == AnnouncementKind::PERSIST_LOADED) {
                view->announce_loaded(p_row.entity, p_row.flag);
            } else if (view.is_valid()) {
                view->announce_saved(p_row.entity);
            }
        } break;
        default:
            break;
    }
}

void NetwMultiplayer::prediction_emit(
    NetwPrediction *p_prediction,
    const Announcement &p_row
) {
    if (p_prediction == nullptr) {
        return;
    }
    if (p_row.kind == AnnouncementKind::RECOVERED) {
        p_prediction->emit_signal(
            StringName("corrected"),
            p_row.from,
            p_row.report,
            p_row.flag
        );
    }
#if defined(NETW_TESTS)
    const predict::Decl *decl
        = predict_find_declaration(p_prediction->get_entity().ptr());
    if (decl == nullptr || !decl->test_report) {
        return;
    }
    Array said;
    switch (p_row.kind) {
        case AnnouncementKind::STATE_EVALUATED:
            said.push_back(p_row.from);
            said.push_back(p_row.to);
            said.push_back(p_row.amount);
            said.push_back(p_row.flag);
            decl->test_report(StringName("state_evaluated"), said);
            break;
        case AnnouncementKind::EPISODE_CHANGED:
            said.push_back(p_row.code);
            said.push_back(p_row.report);
            decl->test_report(StringName("episode_changed"), said);
            break;
        case AnnouncementKind::RECOVERED:
            said.push_back(p_row.from);
            said.push_back(p_row.report);
            said.push_back(p_row.flag);
            said.push_back(p_row.code);
            decl->test_report(StringName("corrected"), said);
            break;
        default:
            said.push_back(p_row.from);
            said.push_back(p_row.code);
            decl->test_report(StringName("divergence_detected"), said);
            break;
    }
#endif
}

NetwEntity *NetwMultiplayer::entity_holder(uint64_t p_id) const {
    const EntitySlot *slot = entity_slots.find(p_id);
    if (slot == nullptr) {
        return nullptr;
    }
    return slot->holder != nullptr ? slot->holder : slot->live.ptr();
}

bool NetwMultiplayer::entity_readmit(NetwEntity *p_entity) {
    const Ref<NetwEntity> keep(p_entity);
    control_abandon_claims(p_entity);
    const int64_t from = p_entity->get_stage();
    p_entity->owner_exiting_tree = false;
    p_entity->despawning_watched = false;
    p_entity->scene_subject = RID();
    p_entity->synchronizers_dirty = true;
    p_entity->synchronizers_absent = false;
    p_entity->ready_once_fired = true;
    if (p_entity->home != nullptr) {
        p_entity->home->wrapper_detach(p_entity);
    }
    p_entity->draft->readmit();
    if (NetwMultiplayer *core = p_entity->session_core()) {
        core->slot_attach(p_entity);
    }
    if (!p_entity->get_record()->publish_stage(p_entity, from)) {
        return true;
    }
    callable_mp(p_entity, &NetwEntity::readmitted_ready).call_deferred();
    NetwMultiplayer *core = p_entity->session_core();
    Node *owner = p_entity->get_owner();
    NetwEntityRecord *const record = p_entity->get_record();
    if (spawn::Record *readmission = record->take_readmission()) {
        const spawn::Record recipe = *readmission;
        memdelete(readmission);
        if (core != nullptr && owner != nullptr && core->is_host()
            && core->spawn_readmit(owner, recipe)) {
            return true;
        }
    }
    if (core != nullptr) {
        p_entity->arm(Ref<NetwMultiplayer>(core));
    }
    return false;
}

void NetwMultiplayer::entity_readmitted_ready(NetwEntity *p_entity) {
    const Ref<NetwEntity> keep(p_entity);
    Node *owner = p_entity->get_owner();
    if (!p_entity->ready_once_fired || owner == nullptr
        || !owner->is_inside_tree()
        || p_entity->get_stage() != int64_t(entity::Stage::LIVE)) {
        return;
    }
    p_entity->ready_once_fired = false;
    entity_owner_ready(p_entity);
    NetwEntityRecord *const held = p_entity->get_record();
    if (!held->get_predicted_at_departure()) {
        return;
    }
    held->set_predicted_at_departure(false);
    if (NetwMultiplayer *core = p_entity->session_core()) {
        core->predict_readmit(p_entity);
    }
}

void NetwMultiplayer::entity_tree_entered(NetwEntity *p_entity) {
    const Ref<NetwEntity> keep(p_entity);
    const bool respawned
        = p_entity->get_stage() == int64_t(entity::Stage::FREED)
        && entity_readmit(p_entity);
    NetwMultiplayer *core = p_entity->session_core();
    const int64_t route = p_entity->get_route();
    const bool hidden = !respawned && core != nullptr && route > 0
        && p_entity->get_stage() == int64_t(entity::Stage::LIVE)
        && core->liveness_route_state(route) == ENTITY_STATE_ABSENT;
    if (!respawned) {
        entity_handle_entered(p_entity);
    }
    Node *owner = p_entity->get_owner();
    if (owner == nullptr || owner->is_queued_for_deletion()
        || entity::stage_is_leaving(p_entity->get_stage())) {
        return;
    }
    if (hidden && p_entity->ready_once_fired) {
        callable_mp(p_entity, &NetwEntity::readmitted_ready).call_deferred();
    }
    const interest::Facet *facet
        = p_entity->get_record()->find_interest_facet();
    if (facet != nullptr && facet->declaration()->has_labels()) {
        if (NetwMultiplayer *session = session_holding(owner)) {
            session->interest_activate(p_entity);
        }
    }
    core = p_entity->session_core();
    if (core != nullptr && p_entity->scene_subject.is_valid()) {
        core->scene_report_live_entity_edge(
            p_entity,
            p_entity->scene_subject,
            true
        );
    }
}

void NetwMultiplayer::entity_tree_exiting(NetwEntity *p_entity) {
    const Ref<NetwEntity> keep(p_entity);
    entity_handle_exiting(p_entity);
    const interest::Facet *facet
        = p_entity->get_record()->find_interest_facet();
    if (facet != nullptr && facet->declaration()->has_labels()) {
        NetwMultiplayer *session
            = session_holding(p_entity->get_owner(), TreeState::EXITING);
        if (session != nullptr) {
            session->interest_deactivate(p_entity);
        }
    }
    NetwMultiplayer *core = p_entity->session_core();
    if (core != nullptr && p_entity->scene_subject.is_valid()) {
        core->scene_report_live_entity_edge(
            p_entity,
            p_entity->scene_subject,
            false
        );
    }
}

void NetwMultiplayer::entity_handle_entered(NetwEntity *p_entity) {
    const Ref<NetwEntity> keep(p_entity);
    NETW_ZONE_NC("NetwEntity tree entered", colors::LIVENESS);
    p_entity->owner_exiting_tree = false;
    p_entity->synchronizers_absent = false;
    control_moved(p_entity);
    Node *owner = p_entity->get_owner();
    if (p_entity->session_core() == nullptr) {
        p_entity->stamp_multiplayer(session_holding(owner));
    }
    entity_enter_tree(
        p_entity,
        owner,
        p_entity->get_record(),
        session_holding(owner),
        p_entity->is_on_server()
    );
}

void NetwMultiplayer::entity_handle_exiting(NetwEntity *p_entity) {
    const Ref<NetwEntity> keep(p_entity);
    p_entity->owner_exiting_tree = true;
    control_moved(p_entity);
    if (NetwMultiplayer *core = p_entity->session_core()) {
        core->entity_capture_exit(p_entity);
        return;
    }
    p_entity->get_record()->complete_departure(p_entity);
}

void NetwMultiplayer::entity_go_live(const RID &p_entity) {
    NetwEntity *held = entity_holder(p_entity.get_id());
    if (held == nullptr) {
        return;
    }
    const Ref<NetwEntity> keep(held);
    if (held->get_stage() != int64_t(entity::Stage::ARMED)) {
        return;
    }
    Node *owner = held->get_owner();
    if (owner == nullptr || !owner->is_inside_tree()) {
        return;
    }
    entity_handle_entered(held);
    owner = held->get_owner();
    if (owner == nullptr || owner->is_queued_for_deletion()
        || held->get_stage() != int64_t(entity::Stage::LIVE)) {
        return;
    }
    if (gd::node_ready(owner) && !held->ready_once_fired) {
        entity_owner_ready(held);
    }
}

void NetwMultiplayer::entity_owner_ready(NetwEntity *p_entity) {
    const Ref<NetwEntity> keep(p_entity);
    if (p_entity->ready_once_fired
        || p_entity->get_stage() != int64_t(entity::Stage::LIVE)) {
        return;
    }
    p_entity->ready_once_fired = true;
    NETW_ZONE_NC("NetwEntity owner ready", colors::LIVENESS);
    if (p_entity->steering() != entity::Control::SERVER) {
        control_apply(p_entity);
    }
    {
        NETW_ZONE_NC("NetwEntity hydrate components", colors::LIVENESS);
        p_entity->hydrate_components();
    }
    NetwMultiplayer *core = p_entity->session_core();
    if (core != nullptr && core->predict_lacks_state_rows(p_entity)) {
        Node *owner = p_entity->get_owner();
        fault::raise(
            core,
            NETW_FAULT_PREDICTION_OFF,
            {owner != nullptr ? String(owner->get_path())
                              : String(p_entity->get_name()),
             String(),
             String(),
             0,
             NETW_OFF_NO_STATE},
            uint64_t(gd::instance_id(owner))
        );
    }
    {
        NETW_ZONE_NC("NetwEntity persist and predict", colors::LIVENESS);
        if (core != nullptr && !core->persist_enroll(p_entity->get_owner())) {
            core->predict_reconcile_declaration(p_entity);
        }
    }
    if (core != nullptr) {
        NETW_ZONE_NC("NetwEntity sim declare", colors::LIVENESS);
        core->sim_declare(p_entity);
    }
    if (core != nullptr) {
        NETW_ZONE_NC("NetwEntity adopt effects", colors::LIVENESS);
        core->observe_entity_spawned(int64_t(p_entity->get_instance_id()));
    }
}

void NetwMultiplayer::entity_linger_then_free(
    NetwEntity *p_entity,
    const Ref<NetwDespawnOpts> &p_opts
) {
    Node *owner = p_entity->get_owner();
    NetwMultiplayer *core = p_entity->session_core();
    if (core == nullptr) {
        p_entity->get_record()->deactivate(owner);
        if (owner != nullptr) {
            owner->queue_free();
        }
        return;
    }
    core->entity_linger(
        p_entity->get_record(),
        owner,
        core->linger_pumps(p_opts->get_linger_seconds())
    );
}

void NetwMultiplayer::entity_despawn_admitted(
    NetwEntity *p_entity,
    const Ref<NetwDespawnOpts> &p_opts
) {
    Ref<NetwDespawnOpts> opts = p_opts;
    if (opts.is_null()) {
        opts.instantiate();
    }
    NetwMultiplayer *api = p_entity->session_core();
    if (api != nullptr && !api->is_host()) {
        api->entity_settle_captured(p_entity);
        if (api->lifecycle_withhold_despawn(p_entity, opts)) {
            return;
        }
    }
    entity_despawn_here(p_entity, opts);
}

void NetwMultiplayer::entity_despawn_here(
    NetwEntity *p_entity,
    const Ref<NetwDespawnOpts> &p_opts
) {
    const Ref<NetwDespawnOpts> opts = p_opts;
    NetwMultiplayer *api = p_entity->session_core();
    Node *owner = p_entity->get_owner();
    const Ref<NetwEntity> keep(p_entity);
    const ObjectID captured_owner = p_entity->owner_id;
    const ObjectID captured_session = p_entity->session_id;
    const RID captured_handle = p_entity->get_rid_handle();
    const auto unchanged = [&](int64_t p_stage) {
        return p_entity->owner_id == captured_owner
            && p_entity->session_id == captured_session
            && p_entity->get_rid_handle() == captured_handle
            && p_entity->get_stage() == p_stage;
    };
    if (api != nullptr && owner != nullptr
        && entity::stage_can_begin_despawn(p_entity->get_stage())) {
        LocalVector<Ref<NetwEntity>> parts;
        collect_despawning_parts(owner, parts);
        for (const Ref<NetwEntity> &part : parts) {
            part->get_record()->set_despawn_reason(opts->get_reason());
            if (part->get_owner() != nullptr) {
                Announcement row;
                row.kind = AnnouncementKind::DESPAWNING;
                row.reason = opts->get_reason();
                entity_announce(part.ptr(), row);
            }
        }
    }
    if (!p_entity->get_record()->begin_despawn(p_entity, owner, opts)) {
        return;
    }
    if (!unchanged(int64_t(entity::Stage::DESPAWNING))) {
        return;
    }
    owner = p_entity->get_owner();
    api = p_entity->session_core();
    if (owner == nullptr || owner->is_queued_for_deletion()) {
        return;
    }
    if (api != nullptr) {
        api->spawn_detach_before_despawn(p_entity);
        api->persist_depart(
            api->get_bindings()->find(owner),
            opts->get_flush_save()
        );
        api->scene_root_despawning(
            p_entity,
            api->is_host() && !opts->get_linger()
        );
    }
    if (owner->get_multiplayer_authority() != 1) {
        owner->set_multiplayer_authority(1);
    }
    if (opts->get_linger()) {
        p_entity->transition(int64_t(entity::Stage::LINGERING));
        if (unchanged(int64_t(entity::Stage::LINGERING))) {
            entity_linger_then_free(p_entity, opts);
        }
        return;
    }
    if (opts->get_defer_free()) {
        Callable(owner, StringName("queue_free")).call_deferred();
    } else {
        owner->queue_free();
    }
}

void NetwMultiplayer::entity_remote_despawn(
    const RID &p_entity,
    const StringName &p_reason,
    double p_linger_seconds
) {
    NetwEntity *held = entity_holder(p_entity.get_id());
    if (held == nullptr) {
        return;
    }
    Ref<NetwDespawnOpts> opts = NetwDespawnOpts::create(p_reason);
    opts->set_linger(p_linger_seconds > 0.0);
    opts->set_linger_seconds(p_linger_seconds);
    const Ref<NetwEntity> keep(held);
    if (!held->get_record()->begin_despawn(held, held->get_owner(), opts)) {
        return;
    }
    if (held->get_stage() != int64_t(entity::Stage::DESPAWNING)) {
        return;
    }
    if (NetwMultiplayer *api = held->session_core()) {
        api->scene_root_despawning(held, false);
    }
    if (opts->get_linger()) {
        held->transition(int64_t(entity::Stage::LINGERING));
    }
}

} // namespace netw
