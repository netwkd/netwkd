#include "netw/api/nodes/multiplayer_viewport.hpp"

#include "godot/class_db.hpp"
#include "godot/engine.hpp"
#include "godot/object.hpp"
#include "godot/scene_tree.hpp"
#include "netw/api/context.hpp"
#include "netw/fault_book.hpp"
#include "netw/log.hpp"

using namespace godot;

namespace netw {

namespace {

bool in_editor() {
    Engine *engine = Engine::get_singleton();
    return engine != nullptr && engine->is_editor_hint();
}

void drop_placeholder(
    const Ref<MultiplayerAPI> &p_placeholder,
    const NodePath &p_path
) {
    SceneTree *tree = gd::scene_tree();
    if (tree == nullptr) {
        return;
    }
    const Ref<MultiplayerAPI> mounted = tree->get_multiplayer(p_path);
    if (mounted == p_placeholder) {
        tree->set_multiplayer(Ref<MultiplayerAPI>(), p_path);
    }
}

void release_api_peer(const Ref<NetwMultiplayer> &p_api) {
    if (p_api->has_multiplayer_peer()) {
        p_api->get_multiplayer_peer()->close();
        p_api->peer_assign(Ref<MultiplayerPeer>());
    }
}

} // namespace

MultiplayerViewport::MultiplayerViewport() {
    set_use_own_world_3d(true);
    if (!in_editor()) {
        api = NetwMultiplayer::create();
    }
}

void MultiplayerViewport::set_api(const Ref<NetwMultiplayer> &p_value) {
    if (is_inside_tree()) {
        fault::raise(
            api.ptr(),
            NETW_FAULT_TOO_LATE,
            {"{MultiplayerViewport.api}",
             vformat("'%s'", String(get_name())),
             "the node enters the tree"},
            gd::instance_id(this)
        );
        return;
    }
    if (p_value.is_null()) {
        fault::raise(
            api.ptr(),
            NETW_FAULT_EMPTY_ARGUMENT,
            {"{MultiplayerViewport.api}", String(), "{NetwMultiplayer}"},
            gd::instance_id(this)
        );
        return;
    }
    if (api == p_value) {
        return;
    }
    if (api.is_valid()) {
        api->embed_dispose();
    }
    api = p_value;
}

void MultiplayerViewport::enter_tree() {
    if (in_editor()) {
        return;
    }
    mount_api();
}

void MultiplayerViewport::mount_api() {
    if (api.is_null()) {
        return;
    }
    register_path();
}

void MultiplayerViewport::unmount_api() {
    if (api.is_null()) {
        return;
    }
    unregister_path();
}

void MultiplayerViewport::register_path() {
    const NodePath root_path = get_path();
    api->session_get_inner()->set_root_path(root_path);
    get_tree()->set_multiplayer(api, root_path);
    const Ref<MultiplayerAPI> mounted = get_tree()->get_multiplayer(root_path);
    NETW_ASSERT(mounted == api);
}

void MultiplayerViewport::unregister_path() {
    const NodePath mounted = api->session_get_inner()->get_root_path();
    if (mounted.is_empty()) {
        return;
    }
    if (!api->is_polling()) {
        get_tree()->set_multiplayer(Ref<MultiplayerAPI>(), mounted);
        return;
    }
    Ref<SceneMultiplayer> placeholder;
    placeholder.instantiate();
    get_tree()->set_multiplayer(placeholder, mounted);
    callable_mp_static(&drop_placeholder)
        .bind(placeholder, mounted)
        .call_deferred();
}

void MultiplayerViewport::follow_rename() {
    if (in_editor() || api.is_null() || !is_inside_tree()) {
        return;
    }
    if (api->session_get_inner()->get_root_path() == get_path()) {
        return;
    }
    unregister_path();
    register_path();
}

void MultiplayerViewport::release_peer() {
    if (api.is_null()) {
        return;
    }
    if (api->is_polling()) {
        callable_mp_static(&release_api_peer).bind(api).call_deferred();
        return;
    }
    release_api_peer(api);
}

void MultiplayerViewport::exiting() {
    NETW_TRACE(sys::SESSION, "The MultiplayerViewport is leaving the tree.");
    if (!is_queued_for_deletion()) {
        unmount_api();
        return;
    }
    release_peer();
    unmount_api();
    deletion_finalized = true;
}

void MultiplayerViewport::close_peer_on_delete() {
    if (deletion_finalized || in_editor() || is_inside_tree()) {
        return;
    }
    deletion_finalized = true;
    release_peer();
}

void MultiplayerViewport::_notification(int p_what) {
    switch (p_what) {
        case NOTIFICATION_ENTER_TREE:
            enter_tree();
            break;
        case NOTIFICATION_PATH_RENAMED:
            follow_rename();
            break;
        case NOTIFICATION_EXIT_TREE:
            exiting();
            break;
        case NOTIFICATION_PREDELETE:
            close_peer_on_delete();
            if (api.is_valid()) {
                api->embed_dispose();
            }
            break;
        default:
            break;
    }
}

void MultiplayerViewport::_bind_methods() {
    ClassDB::bind_method(
        D_METHOD("set_api", "value"),
        &MultiplayerViewport::set_api
    );
    ClassDB::bind_method(D_METHOD("get_api"), &MultiplayerViewport::get_api);
    ADD_PROPERTY(
        PropertyInfo(
            Variant::OBJECT,
            "api",
            PROPERTY_HINT_RESOURCE_TYPE,
            "NetwMultiplayer",
            PROPERTY_USAGE_NONE
        ),
        "set_api",
        "get_api"
    );
}

} // namespace netw
