#include "netw/member_packing.hpp"

#include "netw/api/interpolate.hpp"
#include "netw/api/quantize.hpp"
#include "netw/fault_book.hpp"
#include "netw/log.hpp"

using namespace godot;

namespace netw {

namespace {

String script_file(const Ref<Script> &p_script) {
    return p_script.is_valid() ? p_script->get_path().get_file() : String();
}

int declared_type(const Array &p_types, int p_index) {
    return p_index < p_types.size() ? int(p_types[p_index]) : int(Variant::NIL);
}

String type_label(int p_type) {
    return Variant::get_type_name(Variant::Type(p_type));
}

template <typename T>
bool declared_slots_are(
    const Array &p_declared,
    const String &p_verb,
    const String &p_expected,
    const StringName &p_member
) {
    for (int index = 0; index < p_declared.size(); index++) {
        const Variant slot = p_declared[index];
        if (slot.get_type() == Variant::NIL || Ref<T>(slot).is_valid()) {
            continue;
        }
        fault::raise(
            nullptr,
            NETW_FAULT_BAD_VALUE,
            {p_verb,
             vformat(
                 "a %s for value %d of '%s'",
                 type_label(int(slot.get_type())),
                 index,
                 String(p_member)
             ),
             vformat("a %s or null there", p_expected)}
        );
        return false;
    }
    return true;
}

} // namespace

bool declared_count_fits_arity(
    int p_declared_count,
    const Array &p_types,
    const String &p_verb,
    const StringName &p_member,
    const Ref<Script> &p_script
) {
    if (p_types.is_empty() || p_declared_count == p_types.size()) {
        return true;
    }
    fault::raise(
        nullptr,
        NETW_FAULT_BAD_VALUE,
        {p_verb,
         vformat(
             "%d entries for the %d values of '%s' in '%s'",
             p_declared_count,
             p_types.size(),
             String(p_member),
             script_file(p_script)
         ),
         "one entry per value, and null for a value to leave as it is"}
    );
    return false;
}

bool declared_are_quantizers(
    const Array &p_declared,
    const String &p_verb,
    const StringName &p_member
) {
    return declared_slots_are<NetwQuantize>(
        p_declared,
        p_verb,
        "NetwQuantize",
        p_member
    );
}

bool declared_are_interpolators(
    const Array &p_declared,
    const String &p_verb,
    const StringName &p_member
) {
    return declared_slots_are<NetwInterpolate>(
        p_declared,
        p_verb,
        "NetwInterpolate",
        p_member
    );
}

bool quantizers_fit_types(
    const Array &p_quantizers,
    const Array &p_types,
    const StringName &p_member,
    const Ref<Script> &p_script
) {
    for (int index = 0; index < p_quantizers.size(); index++) {
        const Ref<NetwQuantize> packer = p_quantizers[index];
        if (packer.is_null()) {
            continue;
        }
        const int type = declared_type(p_types, index);
        if (type == int(Variant::NIL)
            || packer->supports_type(static_cast<Variant::Type>(type))) {
            continue;
        }
        fault::raise(
            nullptr,
            NETW_FAULT_UNSENDABLE_VALUE,
            {packer->get_class(),
             vformat(
                 "a %s value of '%s' in '%s'",
                 type_label(type),
                 String(p_member),
                 script_file(p_script)
             ),
             vformat("a quantizer that supports %s", type_label(type))}
        );
        return false;
    }
    return true;
}

bool interpolators_fit_types(
    const Array &p_interpolators,
    const Array &p_types,
    const StringName &p_member,
    const Ref<Script> &p_script
) {
    for (int index = 0; index < p_interpolators.size(); index++) {
        const Ref<NetwInterpolate> smoother = p_interpolators[index];
        if (smoother.is_null()) {
            continue;
        }
        const int type = declared_type(p_types, index);
        if (type == int(Variant::NIL) || smoother->supports_type(type)) {
            continue;
        }
        fault::raise(
            nullptr,
            NETW_FAULT_INTERPOLATION_IGNORED,
            {vformat("%s:%s", script_file(p_script), String(p_member)),
             vformat("an interpolator that cannot blend a %s", type_label(type)),
             vformat("an interpolator that supports %s", type_label(type))}
        );
        return false;
    }
    return true;
}

} // namespace netw
