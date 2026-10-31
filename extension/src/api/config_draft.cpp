#include "netw/api/config_draft.hpp"

#include "godot/math.hpp"
#include "netw/fault_book.hpp"

using namespace godot;

namespace netw {

namespace config_draft {

namespace {

bool late(Guard &p_guard, const char *p_field) {
    if (!p_guard.sealed) {
        return false;
    }
    if (p_guard.late_reported) {
        return true;
    }
    p_guard.late_reported = true;
    fault::raise(
        nullptr,
        NETW_FAULT_DECLARED_ONLINE,
        {vformat("{Netw.%s}", p_guard.verb),
         String(),
         vformat("'%s' on '%s'", p_field, p_guard.scope)}
    );
    return true;
}

bool refuse(const Guard &p_guard, const char *p_field, const String &p_saw) {
    fault::raise(
        nullptr,
        NETW_FAULT_BAD_VALUE,
        {vformat("{Netw.%s}", p_guard.verb),
         vformat("%s for '%s'", p_saw, p_field),
         String("a value in the range its documentation gives")}
    );
    return false;
}

} // namespace

void Guard::seal(const String &p_scope) {
    sealed = true;
    scope = p_scope;
}

bool Guard::takes(const char *p_field) {
    return !late(*this, p_field);
}

bool Guard::takes_enum(const char *p_field, int64_t p_value, int64_t p_count) {
    if (late(*this, p_field)) {
        return false;
    }
    if (p_value < 0 || p_value >= p_count) {
        return refuse(*this, p_field, String::num_int64(p_value));
    }
    return true;
}

bool Guard::takes_count(const char *p_field, int64_t p_value) {
    if (late(*this, p_field)) {
        return false;
    }
    if (p_value < 0) {
        return refuse(*this, p_field, String::num_int64(p_value));
    }
    return true;
}

bool Guard::takes_positive_count(const char *p_field, int64_t p_value) {
    if (late(*this, p_field)) {
        return false;
    }
    if (p_value <= 0) {
        return refuse(*this, p_field, String::num_int64(p_value));
    }
    return true;
}

bool Guard::takes_ratio(const char *p_field, double p_value) {
    if (late(*this, p_field)) {
        return false;
    }
    if (!Math::is_finite(p_value) || p_value < 0.0) {
        return refuse(*this, p_field, String::num(p_value));
    }
    return true;
}

bool Guard::takes_positive(const char *p_field, double p_value) {
    if (late(*this, p_field)) {
        return false;
    }
    if (!Math::is_finite(p_value) || p_value <= 0.0) {
        return refuse(*this, p_field, String::num(p_value));
    }
    return true;
}

} // namespace config_draft

} // namespace netw
