#include "netw/decl_sites.hpp"

#include "godot/hash_map.hpp"
#include "godot/script.hpp"

using namespace godot;

namespace netw::decl_sites {

#if defined(DEBUG_ENABLED)

namespace {

const char *ADDON_PREFIX = "res://addons/networked/";

HashMap<String, gd::ScriptSite> &book() {
    static HashMap<String, gd::ScriptSite> sites;
    return sites;
}

String key_of(
    netw_fault_site p_kind,
    Object *p_declarer,
    const StringName &p_member
) {
    String owner;
    if (p_kind != NETW_SITE_CLOCK && p_declarer != nullptr) {
        const Ref<Script> script = p_declarer->get_script();
        owner = script.is_valid() ? script->get_path()
                                  : String(p_declarer->get_class());
    }
    const String member = p_kind == NETW_SITE_CLOCK ? String()
                                                    : String(p_member);
    return itos(int64_t(p_kind)) + "|" + owner + "|" + member;
}

} // namespace

void record(
    netw_fault_site p_kind,
    Object *p_declarer,
    const StringName &p_member
) {
    const String key = key_of(p_kind, p_declarer, p_member);
    if (p_kind != NETW_SITE_CLOCK && book().has(key)) {
        return;
    }
    const gd::ScriptSite site = gd::capture_script_site(ADDON_PREFIX);
    if (site.file.is_empty()) {
        return;
    }
    book()[key] = site;
    const String unowned = key_of(p_kind, nullptr, p_member);
    if (p_kind == NETW_SITE_HANDLER && !book().has(unowned)) {
        book()[unowned] = site;
    }
}

void record_handler(const Callable &p_handler) {
    if (!p_handler.is_custom()) {
        record(
            NETW_SITE_HANDLER,
            p_handler.get_object(),
            p_handler.get_method()
        );
    }
}

gd::ScriptSite find(
    netw_fault_site p_kind,
    Object *p_declarer,
    const StringName &p_member
) {
    if (p_kind == NETW_SITE_CALL) {
        return gd::capture_script_site(ADDON_PREFIX);
    }
    const gd::ScriptSite *held
        = book().getptr(key_of(p_kind, p_declarer, p_member));
    if (held == nullptr && p_kind == NETW_SITE_HANDLER) {
        held = book().getptr(key_of(p_kind, nullptr, p_member));
    }
    return held != nullptr ? *held : gd::ScriptSite();
}

void clear() {
    book().clear();
}

#else

void record(netw_fault_site, Object *, const StringName &) {
}

void record_handler(const Callable &) {
}

gd::ScriptSite find(netw_fault_site, Object *, const StringName &) {
    return gd::ScriptSite();
}

void clear() {
}

#endif

} // namespace netw::decl_sites
