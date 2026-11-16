#include "netw/game_call.hpp"

#include "godot/class_db.hpp"
#include "godot/object.hpp"
#include "godot/templates.hpp"
#include "netw/fault_book.hpp"

using namespace godot;

namespace netw {

namespace {

HashSet<uint64_t> &reported_calls() {
    static HashSet<uint64_t> keys;
    return keys;
}

bool first_report(const Callable &p_fn, const GameCall &p_site) {
    const uint64_t key
        = (uint64_t(p_fn.hash()) << 32) ^ uint64_t(String(p_site.what).hash());
    if (reported_calls().has(key)) {
        return false;
    }
    reported_calls().insert(key);
    return true;
}

struct RowType {
    enum Kind { ANY, BUILTIN, ERROR_CODE, ARRAY, CLASS };
    Kind kind = ANY;
    Variant::Type builtin = Variant::NIL;
    StringName class_name;
};

RowType row_type(const String &p_name) {
    RowType type;
    if (p_name == "Variant") {
        return type;
    }
    if (p_name == "Error") {
        type.kind = RowType::ERROR_CODE;
        return type;
    }
    if (p_name == "Array" || p_name.begins_with("Array[")) {
        type.kind = RowType::ARRAY;
        return type;
    }
    for (int at = Variant::NIL + 1; at < Variant::VARIANT_MAX; ++at) {
        if (Variant::get_type_name(Variant::Type(at)) == p_name) {
            type.kind = RowType::BUILTIN;
            type.builtin = Variant::Type(at);
            return type;
        }
    }
    type.kind = RowType::CLASS;
    type.class_name = StringName(p_name);
    return type;
}

Vector<RowType> parse_row(const String &p_signature) {
    Vector<RowType> types;
    const int open = p_signature.find("(");
    const int close = p_signature.find(")");
    if (open < 0 || close < open) {
        return types;
    }
    int depth = 0;
    int start = open + 1;
    for (int at = open + 1; at <= close; ++at) {
        const char32_t letter = p_signature[at];
        if (letter == '[') {
            ++depth;
        } else if (letter == ']') {
            --depth;
        }
        if (at != close && (letter != ',' || depth != 0)) {
            continue;
        }
        const String parameter
            = p_signature.substr(start, at - start).strip_edges();
        start = at + 1;
        const int colon = parameter.find(":");
        if (parameter == "..." || colon < 0) {
            break;
        }
        types.push_back(row_type(parameter.substr(colon + 1).strip_edges()));
    }
    return types;
}

const Vector<RowType> &row_types(const GameCall &p_site) {
    static HashMap<uint64_t, Vector<RowType>> rows;
    const uint64_t key = uint64_t(uintptr_t(p_site.signature));
    const Vector<RowType> *known = rows.getptr(key);
    if (known != nullptr) {
        return *known;
    }
    rows.insert(key, parse_row(String(p_site.signature)));
    return *rows.getptr(key);
}

bool class_fits(const StringName &p_row, const StringName &p_declared) {
    if (!ClassDB::class_exists(p_declared)) {
        return true;
    }
    for (StringName at = p_row; at != StringName();
         at = ClassDB::get_parent_class(at)) {
        if (at == p_declared) {
            return true;
        }
    }
    return false;
}

bool type_fits(const RowType &p_row, const Dictionary &p_declared) {
    const Variant::Type declared = Variant::Type(int64_t(p_declared["type"]));
    if (declared == Variant::NIL) {
        return true;
    }
    switch (p_row.kind) {
        case RowType::ANY:
            return true;
        case RowType::BUILTIN:
            return declared == p_row.builtin
                || (p_row.builtin == Variant::INT
                    && declared == Variant::FLOAT);
        case RowType::ERROR_CODE:
            return declared == Variant::INT;
        case RowType::ARRAY:
            return declared == Variant::ARRAY;
        case RowType::CLASS: {
            const StringName declared_class
                = StringName(p_declared["class_name"]);
            return declared == Variant::OBJECT
                && class_fits(p_row.class_name, declared_class);
        }
    }
    return false;
}

bool find_method(const Callable &p_fn, Dictionary &r_info) {
    if (!p_fn.is_valid() || p_fn.is_custom()) {
        return false;
    }
    Object *object = p_fn.get_object();
    if (object == nullptr) {
        return false;
    }
    const StringName method = p_fn.get_method();
    const Array methods = gd::method_list(object);
    for (int at = 0; at < methods.size(); ++at) {
        const Dictionary info = methods[at];
        if (StringName(info["name"]) == method) {
            r_info = info;
            return true;
        }
    }
    return false;
}

bool count_fits(const Dictionary &p_info, int64_t p_count) {
    const int64_t total = Array(p_info["args"]).size();
    const int64_t optional = Array(p_info["default_args"]).size();
    const bool vararg
        = (int64_t(p_info["flags"]) & int64_t(METHOD_FLAG_VARARG)) != 0;
    return p_count >= total - optional && (vararg || p_count <= total);
}

bool declaration_fits(const Callable &p_fn, const GameCall &p_site) {
    Dictionary info;
    if (!find_method(p_fn, info)) {
        return true;
    }
    const Vector<RowType> &row = row_types(p_site);
    const Array args = info["args"];
    if (p_site.argument_count < 0) {
        const bool vararg
            = (int64_t(info["flags"]) & int64_t(METHOD_FLAG_VARARG)) != 0;
        if (!vararg && args.size() < row.size()) {
            return false;
        }
    } else if (!count_fits(info, p_site.argument_count)) {
        return false;
    }
    for (int at = 0; at < row.size() && at < args.size(); ++at) {
        if (!type_fits(row[at], Dictionary(args[at]))) {
            return false;
        }
    }
    return true;
}

} // namespace

bool game_call_accepts(const Callable &p_fn, int64_t p_count) {
    Dictionary info;
    return !find_method(p_fn, info) || count_fits(info, p_count);
}

bool game_call_declared(const Callable &p_fn, const GameCall &p_site) {
    if (!p_fn.is_null() && !p_fn.is_valid()) {
        if (first_report(p_fn, p_site)) {
            fault::raise(
                nullptr,
                NETW_FAULT_BAD_CALLABLE,
                {String(p_site.what),
                 String(),
                 String("a method its object has")}
            );
        }
        return false;
    }
    if (declaration_fits(p_fn, p_site)) {
        return true;
    }
    if (first_report(p_fn, p_site)) {
        fault::raise(
            nullptr,
            NETW_FAULT_HANDLER_UNCALLABLE,
            {String(p_site.what), String(), String(p_site.signature)}
        );
    }
    return false;
}

Variant game_call(
    const Callable &p_fn,
    const Array &p_args,
    const GameCall &p_site,
    bool *r_called
) {
    bool called = false;
    const Variant result = gd::call_checked(p_fn, p_args, called);
    if (r_called != nullptr) {
        *r_called = called;
    }
    if (!called && p_fn.is_valid() && first_report(p_fn, p_site)) {
        fault::raise(
            nullptr,
            NETW_FAULT_HANDLER_UNCALLABLE,
            {String(p_site.what), String(), String(p_site.signature)}
        );
    }
    return result;
}

} // namespace netw
