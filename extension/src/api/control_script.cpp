#include "netw/api/control_script.hpp"

#include "godot/class_db.hpp"
#include "godot/node.hpp"
#include "godot/script.hpp"

namespace netw::entity {

using namespace godot;

void declare_policy(
    Object *p_script,
    const StringName &p_name,
    int64_t p_policy,
    bool p_is_signal
) {
    if (p_script == nullptr || p_name == StringName()) {
        return;
    }
    const StringName key
        = p_is_signal ? Control::emit_book_key() : Control::write_book_key();
    Dictionary book = p_script->has_meta(key)
        ? Dictionary(p_script->get_meta(key))
        : Dictionary();
    book[p_name] = p_policy;
    p_script->set_meta(key, book);
}

bool script_admits(
    Object *p_node,
    const StringName &p_name,
    bool p_is_signal,
    int64_t p_sender,
    int64_t p_controller
) {
    if (p_sender == SERVER_PEER) {
        return true;
    }
    Node *node = Object::cast_to<Node>(p_node);
    if (node == nullptr) {
        return false;
    }
    const Ref<Script> script = node->get_script();
    if (script.is_null()) {
        return false;
    }
    const int64_t authority = int64_t(node->get_multiplayer_authority());
    const StringName key
        = p_is_signal ? Control::emit_book_key() : Control::write_book_key();
    if (!script->has_meta(key)) {
        return p_sender == authority;
    }
    const Dictionary book = script->get_meta(key);
    if (!book.has(p_name)) {
        return p_sender == authority;
    }
    return Control::policy_admits(
        int(int64_t(book[p_name])),
        p_sender,
        authority,
        p_controller
    );
}

} // namespace netw::entity
