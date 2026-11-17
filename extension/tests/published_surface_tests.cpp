#include "support/netw_cells.h"
#include "support/netw_test.h"

#if defined(NETW_GDEXTENSION)

#include <godot_cpp/classes/class_db_singleton.hpp>

#include <cstdint>

#include "godot/class_db.hpp"
#include "godot/extension.hpp"
#include "godot/templates.hpp"
#include "godot/variant.hpp"
#include "netw/api/clock_config.hpp"
#include "netw/api/native_api.hpp"
#include "support/published_classes.h"

namespace TestNetwPublishedSurface {

using namespace godot;
using netw::NetwClockConfig;
using netw_test::law_broken;
using netw_test::law_held;
using netw_test::LawVerdict;

struct Stand {
    String label;
};

typedef netw_test::LawRowFor<Stand> Row;

using MethodBindFn = void *(*)(const char *, const char *, uint64_t);
using Ptrcall0Fn = void (*)(void *, void *, void *);
using Call0Fn = void (*)(void *, void *, void *);
using Call1Fn = void (*)(void *, void *, const void *, void *);
using Call2Fn = void (*)(void *, void *, const void *, const void *, void *);
using RetainFn = void (*)(void *);
using ReleaseFn = void (*)(void *);

constexpr uint64_t REFERENCE_COUNT_HASH = 3905245786;
constexpr uint64_t SET_META_HASH = 3776071444;
constexpr uint64_t GET_META_HASH = 3990617847;
constexpr uint64_t GET_CLASS_HASH = 201670096;

constexpr const char *EXPORTED[] = {
    "method_bind",
    "ptrcall0",
    "ptrcall1",
    "ptrcall2",
    "ptrcall3",
    "call0",
    "call1",
    "call2",
    "call3",
    "call4",
    "args_new",
    "args_set",
    "call_pack",
    "args_free",
    "retain",
    "release",
};

const char *STOCK = "SceneMultiplayer";
const char *SUBSTITUTE = "NetwMultiplayer";

template <typename T> T function_at(const char *p_name) {
    const Dictionary functions = netw::native_api_table()["functions"];
    return reinterpret_cast<T>(
        static_cast<intptr_t>(int64_t(functions[p_name]))
    );
}

LawVerdict first_of(const PackedStringArray &p_found) {
    if (p_found.is_empty()) {
        return law_held();
    }
    return law_broken(
        "%d gaps, first %s",
        int(p_found.size()),
        String(p_found[0]).utf8().get_data()
    );
}

LawVerdict table_addresses(const Stand &) {
    const Dictionary table = netw::native_api_table();
    if (int64_t(table["version"]) != 3 || int64_t(table["hash"]) == 0) {
        return law_broken("the table carries no version or hash");
    }
    const Dictionary functions = table["functions"];
    if (functions.size() != int(sizeof(EXPORTED) / sizeof(EXPORTED[0]))) {
        return law_broken("the table publishes %d functions", functions.size());
    }
    for (const char *name : EXPORTED) {
        if (int64_t(functions[name]) == 0) {
            return law_broken("%s has no address", name);
        }
    }
    return law_held();
}

LawVerdict live_object(const Stand &) {
    const auto bind_of = function_at<MethodBindFn>("method_bind");
    const auto ptrcall0 = function_at<Ptrcall0Fn>("ptrcall0");
    const auto retain = function_at<RetainFn>("retain");
    const auto release = function_at<ReleaseFn>("release");

    Ref<NetwClockConfig> config;
    config.instantiate();
    void *raw = netw::gd::engine_object(config.ptr());
    void *count
        = bind_of("RefCounted", "get_reference_count", REFERENCE_COUNT_HASH);
    if (count == nullptr) {
        return law_broken("a real method has no bind");
    }

    int64_t before = 0;
    ptrcall0(count, raw, &before);
    if (before != int64_t(config->get_reference_count())) {
        return law_broken("a ptrcall read another object");
    }
    retain(raw);
    int64_t held = 0;
    ptrcall0(count, raw, &held);
    release(raw);
    int64_t settled = 0;
    ptrcall0(count, raw, &settled);
    if (held != before + 1 || settled != before) {
        return law_broken("a retain and a release did not balance");
    }
    return law_held();
}

LawVerdict variant_call(const Stand &) {
    const auto bind_of = function_at<MethodBindFn>("method_bind");
    const auto call0 = function_at<Call0Fn>("call0");
    const auto call1 = function_at<Call1Fn>("call1");
    const auto call2 = function_at<Call2Fn>("call2");

    Ref<NetwClockConfig> config;
    config.instantiate();
    void *raw = netw::gd::engine_object(config.ptr());
    void *setter = bind_of("Object", "set_meta", SET_META_HASH);
    void *getter = bind_of("Object", "get_meta", GET_META_HASH);
    void *named = bind_of("Object", "get_class", GET_CLASS_HASH);
    if (setter == nullptr || getter == nullptr || named == nullptr) {
        return law_broken("a real method has no bind");
    }

    const Variant name = StringName("probe");
    const Variant written = 7;
    Variant discarded;
    call2(setter, raw, &name, &written, &discarded);
    Variant answered;
    call1(getter, raw, &name, &answered);
    if (int64_t(answered) != 7) {
        return law_broken("a StringName argument did not arrive");
    }

    Variant class_name;
    call0(named, raw, &class_name);
    if (String(class_name) != String("NetwClockConfig")) {
        return law_broken("a String answer did not come back");
    }
    return law_held();
}

bool names_a_concrete_class(const String &p_class_name) {
    return !p_class_name.is_empty() && p_class_name != String("Object")
        && p_class_name != String("RefCounted")
        && p_class_name != String("Variant");
}

bool is_layout_row(int64_t p_usage) {
    return (p_usage
            & (PROPERTY_USAGE_GROUP | PROPERTY_USAGE_SUBGROUP
               | PROPERTY_USAGE_CATEGORY))
        != 0;
}

String untyped_property(const String &p_class, const Dictionary &p_property) {
    const int64_t usage = p_property.get("usage", int64_t(0));
    if (is_layout_row(usage)) {
        return String();
    }
    const int64_t type = p_property.get("type", int64_t(0));
    const String named = p_property.get("class_name", String());
    const String where
        = p_class + String(".") + String(p_property.get("name", ""));
    if (type == int64_t(Variant::OBJECT) && !names_a_concrete_class(named)) {
        return where + String(" is OBJECT but names '") + named + String("'");
    }
    if (type == int64_t(Variant::NIL)
        && (usage & PROPERTY_USAGE_NIL_IS_VARIANT) == 0) {
        return where + String(" is NIL without NIL_IS_VARIANT");
    }
    return String();
}

String untyped_return(const String &p_class, const Dictionary &p_method) {
    const Dictionary answered = p_method.get("return", Dictionary());
    if (int64_t(answered.get("type", int64_t(0))) != int64_t(Variant::OBJECT)) {
        return String();
    }
    const String named = answered.get("class_name", String());
    if (names_a_concrete_class(named)) {
        return String();
    }
    return p_class + String(".") + String(p_method.get("name", ""))
        + String("() answers '") + named + String("'");
}

LawVerdict typed_members(const Stand &) {
    ClassDBSingleton *classes = ClassDBSingleton::get_singleton();
    const PackedStringArray published = netw_test::published_classes();
    if (!published.has(String("NetwMultiplayer"))) {
        return law_broken("the published classes were not read");
    }
    PackedStringArray found;
    for (int at = 0; at < published.size(); ++at) {
        const String named = published[at];
        const TypedArray<Dictionary> properties
            = classes->class_get_property_list(named, true);
        for (int row = 0; row < properties.size(); ++row) {
            const String verdict = untyped_property(named, properties[row]);
            if (!verdict.is_empty()) {
                found.push_back(verdict);
            }
        }
        const TypedArray<Dictionary> methods
            = classes->class_get_method_list(named, true);
        for (int row = 0; row < methods.size(); ++row) {
            const String verdict = untyped_return(named, methods[row]);
            if (!verdict.is_empty()) {
                found.push_back(verdict);
            }
        }
    }
    return first_of(found);
}

String type_name(int64_t p_type) {
    return Variant::get_type_name(Variant::Type(p_type));
}

String signature(const Dictionary &p_method) {
    String out = String(p_method.get("name", "")) + String("(");
    const Array arguments = p_method.get("args", Array());
    for (int at = 0; at < arguments.size(); ++at) {
        const Dictionary argument = arguments[at];
        if (at > 0) {
            out += String(", ");
        }
        out += String(argument.get("name", "")) + String(": ")
            + type_name(argument.get("type", int64_t(0)));
    }
    const Array defaults = p_method.get("default_args", Array());
    out += String(") default ") + String::num_int64(defaults.size());
    const Dictionary answered = p_method.get("return", Dictionary());
    return out + String(" -> ") + type_name(answered.get("type", int64_t(0)));
}

Dictionary method_named(const String &p_class, const String &p_name) {
    const TypedArray<Dictionary> methods
        = ClassDBSingleton::get_singleton()->class_get_method_list(
            p_class,
            false
        );
    for (int row = 0; row < methods.size(); ++row) {
        const Dictionary found = methods[row];
        if (String(found.get("name", "")) == p_name) {
            return found;
        }
    }
    return Dictionary();
}

void method_gaps(PackedStringArray &r_gaps) {
    ClassDBSingleton *classes = ClassDBSingleton::get_singleton();
    const TypedArray<Dictionary> stock
        = classes->class_get_method_list(STOCK, true);
    for (int row = 0; row < stock.size(); ++row) {
        const Dictionary declared = stock[row];
        const String named = declared.get("name", "");
        if (!classes->class_has_method(SUBSTITUTE, named)) {
            r_gaps.push_back(named + String("() is missing"));
            continue;
        }
        const String mine = signature(method_named(SUBSTITUTE, named));
        const String theirs = signature(declared);
        if (mine != theirs) {
            r_gaps.push_back(theirs + String(" is ") + mine);
        }
    }
}

void property_gaps(PackedStringArray &r_gaps) {
    ClassDBSingleton *classes = ClassDBSingleton::get_singleton();
    const TypedArray<Dictionary> stock
        = classes->class_get_property_list(STOCK, true);
    for (int row = 0; row < stock.size(); ++row) {
        const Dictionary declared = stock[row];
        if (is_layout_row(declared.get("usage", int64_t(0)))) {
            continue;
        }
        const String named = declared.get("name", "");
        const String setter
            = classes->class_get_property_setter(SUBSTITUTE, named);
        const String getter
            = classes->class_get_property_getter(SUBSTITUTE, named);
        if ((setter.is_empty() && getter.is_empty())
            || setter != classes->class_get_property_setter(STOCK, named)
            || getter != classes->class_get_property_getter(STOCK, named)) {
            r_gaps.push_back(named + String(" has other accessors or none"));
        }
    }
}

void signal_gaps(PackedStringArray &r_gaps) {
    ClassDBSingleton *classes = ClassDBSingleton::get_singleton();
    const TypedArray<Dictionary> stock
        = classes->class_get_signal_list(STOCK, true);
    for (int row = 0; row < stock.size(); ++row) {
        const String named = Dictionary(stock[row]).get("name", "");
        if (!classes->class_has_signal(SUBSTITUTE, named)) {
            r_gaps.push_back(named + String(" is not emitted"));
        }
    }
}

LawVerdict stock_members(const Stand &) {
    ClassDBSingleton *classes = ClassDBSingleton::get_singleton();
    if (classes->class_get_method_list(STOCK, true).is_empty()
        || classes->class_get_signal_list(STOCK, true).is_empty()) {
        return law_broken("SceneMultiplayer was not read");
    }
    PackedStringArray found;
    method_gaps(found);
    property_gaps(found);
    signal_gaps(found);
    return first_of(found);
}

const Row ROWS[] = {
    {"a C# binding reading the native call table",
     "every function the bindings call has an address",
     &table_addresses},
    {"a C# call on a live object it holds and releases",
     "the call reaches that object and a retain balances a release",
     &live_object},
    {"a C# call passing a StringName and reading a String",
     "the argument arrives and the answer comes back",
     &variant_call},
    {"a typed GDScript member of a published class",
     "an object property or return names its class, and a Variant property "
     "says so",
     &typed_members},
    {"a game written against SceneMultiplayer",
     "every method, property and signal is on NetwMultiplayer with the same "
     "name and signature",
     &stock_members},
};

TEST_CASE("[Netw][Surface] a game calls the published classes as typed") {
    const Stand stand = {String("the published classes")};
    for (const Row &row : ROWS) {
        NETW_CELL(row, stand);
        NETW_LAW_HOLDS(row, stand);
    }
}

} // namespace TestNetwPublishedSurface

#endif
