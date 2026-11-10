#include "netw/store/port.hpp"

#include "netw/object_port.hpp"

using namespace godot;

namespace netw::store {

Variant object_value(Object *p_node, const StringName &p_name) {
    return named_get(p_node, p_name);
}

int push_rank(const Column &p_column) {
    switch (p_column.body) {
        case BodyKey::LINEAR:
        case BodyKey::ANGULAR:
            return 1;
        case BodyKey::SLEEPING:
            return 2;
        default:
            return 0;
    }
}

} // namespace netw::store
