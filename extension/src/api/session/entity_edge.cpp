#include "netw/api/netw_multiplayer.hpp"

#include "godot/node.hpp"
#include "godot/object.hpp"
#include "godot/utility.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/entity_record.hpp"
#include "netw/api/interest.hpp"
#include "netw/api/prediction.hpp"
#include "netw/colors.hpp"
#include "netw/entity/stage.hpp"
#include "netw/fault_book.hpp"
#include "netw/interest/decl.hpp"
#include "netw/log.hpp"
#include "netw/persist/binding.hpp"
#include "netw/predict/decl.hpp"
#include "netw/profile.hpp"
#include "netw/scene_decl.hpp"
#include "netw/script/model.hpp"
#include "netw/spawn/record.hpp"
#include "netw/synchronizers.hpp"
#include "netw/tree_hook.hpp"

using namespace godot;
using namespace netw;

namespace netw {

namespace {

SceneDecl declared_scene_of(Node *p_owner) {
    if (p_owner == nullptr) {
        return SceneDecl();
    }
    return script::model::get_scene_decl(
        Ref<Script>(Object::cast_to<Script>(p_owner->get_script()))
    );
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
        entity->emit_announcement(p_row);
        return;
    }
    home->announce(p_row);
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
    const predict::Decl *decl = p_prediction->get_entity()
                                    ->get_record()
                                    ->find_predict_declaration();
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

void NetwMultiplayer::entity_go_live(const RID &p_entity) {
    NetwEntity *held = views.holder_of(p_entity.get_id());
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
    held->_handle_tree_entered();
    owner = held->get_owner();
    if (owner == nullptr || owner->is_queued_for_deletion()
        || held->get_stage() != int64_t(entity::Stage::LIVE)) {
        return;
    }
    if (gd::node_ready(owner) && !held->ready_once_fired) {
        held->_on_owner_ready();
    }
}

void NetwMultiplayer::entity_remote_despawn(
    const RID &p_entity,
    const StringName &p_reason,
    double p_linger_seconds
) {
    NetwEntity *held = views.holder_of(p_entity.get_id());
    if (held == nullptr) {
        return;
    }
    Ref<NetwDespawnOpts> opts = NetwDespawnOpts::create(p_reason);
    opts->set_linger(p_linger_seconds > 0.0);
    opts->set_linger_seconds(p_linger_seconds);
    const Ref<NetwEntity> keep(held);
    if (!held->begin_despawn(*held->get_record(), opts)) {
        return;
    }
    if (held->get_stage() != int64_t(entity::Stage::DESPAWNING)) {
        return;
    }
    if (NetwMultiplayer *api = held->session_core()) {
        api->scene_root_despawning(p_entity, false);
    }
    if (opts->get_linger()) {
        held->transition(int64_t(entity::Stage::LINGERING));
    }
}

void NetwMultiplayer::entity_deactivate(Node *p_owner) {
    if (p_owner == nullptr) {
        return;
    }
    p_owner->set_process_mode(Node::PROCESS_MODE_DISABLED);
    p_owner->set(StringName("visible"), false);
    synchronizers::sync_only_server(p_owner);
}

void NetwMultiplayer::entity_hydrate_identity(
    NetwEntityRecord &p_record,
    Node *p_owner
) {
    if (p_owner == nullptr) {
        return;
    }
    const SceneDecl decl = declared_scene_of(p_owner);
    if (!decl.declared) {
        return;
    }
    p_record.set_declares_scene(true);
    if (p_record.get_scene_label() == StringName()) {
        p_record.set_scene_label(
            decl.label.is_empty() ? p_owner->get_name() : decl.label
        );
    }
    if (p_record.get_scene_isolation() < 0) {
        p_record.set_scene_isolation(decl.isolation);
    }
}

StringName NetwMultiplayer::entity_scene_label(
    const NetwEntityRecord &p_record,
    Node *p_owner
) {
    if (p_record.get_scene_label() != StringName()) {
        return p_record.get_scene_label();
    }
    const SceneDecl decl = declared_scene_of(p_owner);
    return decl.declared && decl.label.is_empty() && p_owner != nullptr
        ? p_owner->get_name()
        : decl.label;
}

int64_t NetwMultiplayer::entity_scene_isolation(
    const NetwEntityRecord &p_record,
    Node *p_owner
) {
    if (p_record.get_scene_isolation() >= 0) {
        return p_record.get_scene_isolation();
    }
    return declared_scene_of(p_owner).isolation;
}

} // namespace netw
