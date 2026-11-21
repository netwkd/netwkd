#include "godot/class_db.hpp"

#include "godot/object.hpp"

#if defined(NETW_MODULE)
#include "core/object/script_language.h"
#else
#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#endif

using namespace godot;

namespace netw::gd {

Variant instantiate_class(const StringName &p_class) {
#if defined(NETW_MODULE)
    Object *made = ::ClassDB::instantiate(p_class);
    if (made == nullptr) {
        return Variant();
    }
    RefCounted *counted = Object::cast_to<RefCounted>(made);
    if (counted != nullptr) {
        return Variant(Ref<RefCounted>(counted));
    }
    return Variant(made);
#else
    ClassDBSingleton *registry = ClassDBSingleton::get_singleton();
    return registry == nullptr ? Variant() : registry->instantiate(p_class);
#endif
}

bool names_a_class(const StringName &p_name) {
#if defined(NETW_MODULE)
    return ::ClassDB::class_exists(p_name)
        || ScriptServer::is_global_class(p_name);
#else
    ClassDBSingleton *registry = ClassDBSingleton::get_singleton();
    if (registry != nullptr && registry->class_exists(p_name)) {
        return true;
    }
    ProjectSettings *settings = ProjectSettings::get_singleton();
    if (settings == nullptr) {
        return false;
    }
    const TypedArray<Dictionary> scripts = settings->get_global_class_list();
    for (int at = 0; at < scripts.size(); ++at) {
        const Dictionary entry = scripts[at];
        if (StringName(entry.get("class", StringName())) == p_name) {
            return true;
        }
    }
    return false;
#endif
}

} // namespace netw::gd
