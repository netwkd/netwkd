#include "netw/interest/decl.hpp"

#include "netw/fault_book.hpp"
#include "netw/game_call.hpp"
#include "netw/log.hpp"

using namespace godot;

namespace netw::interest {

namespace {

const GameCall INTEREST_ENTER
    = {"The NetwInterest.on_enter callback",
       "func(layer_id: StringName, peer_id: int)",
       2};
const GameCall INTEREST_LEAVE
    = {"The NetwInterest.on_leave callback",
       "func(layer_id: StringName, peer_id: int)",
       2};

void fire_each(
    const Array &callbacks,
    const StringName &layer_id,
    int64_t peer,
    const GameCall &site
) {
    Array args;
    args.push_back(layer_id);
    args.push_back(peer);
    for (int at = 0; at < callbacks.size(); ++at) {
        const Callable held = callbacks[at];
        if (held.is_valid()) {
            game_call(held, args, site);
        }
    }
}

int index_of(const LocalVector<StringName> &names, const StringName &name) {
    for (uint32_t index = 0; index < names.size(); ++index) {
        if (names[index] == name) {
            return int(index);
        }
    }
    return -1;
}

bool guard_pair(
    const StringName &layer_id,
    int policy,
    int custom_policy,
    const Callable &custom,
    const char *verb
) {
    const String name = vformat("{%s}", verb);
    if (layer_id.is_empty()) {
        fault::raise(
            nullptr,
            NETW_FAULT_EMPTY_ARGUMENT,
            {name, String(), String("a layer name")}
        );
        return false;
    }
    if (policy < 0 || policy > custom_policy) {
        fault::raise(
            nullptr,
            NETW_FAULT_BAD_VALUE,
            {name, String::num_int64(policy), String("a policy constant")}
        );
        return false;
    }
    if (policy == custom_policy && !custom.is_valid()) {
        fault::raise(
            nullptr,
            NETW_FAULT_BAD_CALLABLE,
            {name, String(), String("the callback that decides a CUSTOM policy")}
        );
        return false;
    }
    if (policy != custom_policy && custom.is_valid()) {
        fault::raise(
            nullptr,
            NETW_FAULT_SETTING_REFUSED,
            {String("A callback on ") + name,
             String("a policy other than CUSTOM"),
             String("the CUSTOM policy, or no callback")}
        );
        return false;
    }
    return true;
}

} // namespace

bool Decl::join(const StringName &layer_id) {
    if (layer_id.is_empty()) {
        fault::raise(
            nullptr,
            NETW_FAULT_EMPTY_ARGUMENT,
            {String("{NetwInterest.join}"), String(), String("a layer name")}
        );
        return false;
    }
    if (index_of(label_ids, layer_id) >= 0) {
        return false;
    }
    label_ids.push_back(layer_id);
    return true;
}

bool Decl::leave(const StringName &layer_id) {
    const int at = index_of(label_ids, layer_id);
    if (at < 0) {
        return false;
    }
    label_ids.remove_at(uint32_t(at));
    return true;
}

bool Decl::has_label(const StringName &layer_id) const {
    return index_of(label_ids, layer_id) >= 0;
}

Array Decl::labels() const {
    Array out;
    for (uint32_t index = 0; index < label_ids.size(); ++index) {
        out.push_back(label_ids[index]);
    }
    return out;
}

bool Decl::set_leave_policy(
    const StringName &layer_id,
    int policy,
    const Callable &custom
) {
    if (!guard_pair(
            layer_id,
            policy,
            LEAVE_CUSTOM,
            custom,
            "NetwInterest.on_leave_policy"
        )) {
        return false;
    }
    if (policy == LEAVE_CUSTOM) {
        leave_callbacks[layer_id] = custom;
    } else {
        leave_callbacks.erase(layer_id);
    }
    leave_policies[layer_id] = policy;
    return true;
}

bool Decl::set_perception_policy(
    const StringName &layer_id,
    int policy,
    const Callable &custom
) {
    if (!guard_pair(
            layer_id,
            policy,
            PERCEPTION_CUSTOM,
            custom,
            "NetwInterest.on_perception_policy"
        )) {
        return false;
    }
    if (policy == PERCEPTION_CUSTOM) {
        perception_callbacks[layer_id] = custom;
    } else {
        perception_callbacks.erase(layer_id);
    }
    perception_policies[layer_id] = policy;
    return true;
}

int Decl::leave_policy_for(const StringName &layer_id, int fallback) const {
    const HashMap<StringName, int32_t>::ConstIterator found
        = leave_policies.find(layer_id);
    return found ? found->value : fallback;
}

int Decl::perception_policy_for(
    const StringName &layer_id,
    int fallback
) const {
    const HashMap<StringName, int32_t>::ConstIterator found
        = perception_policies.find(layer_id);
    return found ? found->value : fallback;
}

Callable Decl::custom_leave_for(const StringName &layer_id) const {
    const HashMap<StringName, Callable>::ConstIterator found
        = leave_callbacks.find(layer_id);
    return found ? found->value : Callable();
}

Callable Decl::custom_perception_for(const StringName &layer_id) const {
    const HashMap<StringName, Callable>::ConstIterator found
        = perception_callbacks.find(layer_id);
    return found ? found->value : Callable();
}

void Decl::clear() {
    label_ids.clear();
    leave_policies.clear();
    leave_callbacks.clear();
    perception_policies.clear();
    perception_callbacks.clear();
    reports_observers = false;
}

void Facet::remember(
    HashMap<StringName, Array> &book,
    const StringName &layer_id,
    const Callable &callback
) {
    Array *held = book.getptr(layer_id);
    if (held == nullptr) {
        book.insert(layer_id, Array());
        held = book.getptr(layer_id);
    }
    if (!held->has(callback)) {
        held->push_back(callback);
    }
}

bool Facet::join(const StringName &layer_id) {
    return decl.join(layer_id);
}

bool Facet::leave(const StringName &layer_id) {
    return decl.leave(layer_id);
}

TypedArray<StringName> Facet::layer_ids() const {
    TypedArray<StringName> out;
    const Array labels = decl.labels();
    for (int at = 0; at < labels.size(); ++at) {
        out.push_back(labels[at]);
    }
    return out;
}

bool Facet::on_enter(const StringName &layer_id, const Callable &callback) {
    if (!callback.is_valid()) {
        fault::raise(
            nullptr,
            NETW_FAULT_BAD_CALLABLE,
            {String("{NetwInterest.on_enter}"),
             String(),
             String("a method of a live object")}
        );
        return false;
    }
    game_call_declared(callback, INTEREST_ENTER);
    remember(enter_callbacks, layer_id, callback);
    return true;
}

bool Facet::on_leave(const StringName &layer_id, const Callable &callback) {
    if (!callback.is_valid()) {
        fault::raise(
            nullptr,
            NETW_FAULT_BAD_CALLABLE,
            {String("{NetwInterest.on_leave}"),
             String(),
             String("a method of a live object")}
        );
        return false;
    }
    game_call_declared(callback, INTEREST_LEAVE);
    remember(leave_callbacks, layer_id, callback);
    return true;
}

bool Facet::on_observed(const Callable &callback) {
    if (!callback.is_valid()) {
        fault::raise(
            nullptr,
            NETW_FAULT_BAD_CALLABLE,
            {String("{NetwInterest.on_observed}"),
             String(),
             String("a method of a live object")}
        );
        return false;
    }
    decl.set_reports_observers(true);
    if (!observed_callbacks.has(callback)) {
        observed_callbacks.push_back(callback);
    }
    return true;
}

bool Facet::on_unobserved(const Callable &callback) {
    if (!callback.is_valid()) {
        fault::raise(
            nullptr,
            NETW_FAULT_BAD_CALLABLE,
            {String("{NetwInterest.on_unobserved}"),
             String(),
             String("a method of a live object")}
        );
        return false;
    }
    decl.set_reports_observers(true);
    if (!unobserved_callbacks.has(callback)) {
        unobserved_callbacks.push_back(callback);
    }
    return true;
}

bool Facet::set_leave_policy(
    const StringName &layer_id,
    int policy,
    const Callable &custom
) {
    return decl.set_leave_policy(layer_id, policy, custom);
}

bool Facet::set_perception_policy(
    const StringName &layer_id,
    int policy,
    const Callable &custom
) {
    return decl.set_perception_policy(layer_id, policy, custom);
}

void Facet::dispatch_enter(const StringName &layer_id, int64_t peer) {
    const Array *held = enter_callbacks.getptr(layer_id);
    if (held != nullptr) {
        fire_each(*held, layer_id, peer, INTEREST_ENTER);
    }
}

void Facet::dispatch_leave(const StringName &layer_id, int64_t peer) {
    const Array *held = leave_callbacks.getptr(layer_id);
    if (held != nullptr) {
        fire_each(*held, layer_id, peer, INTEREST_LEAVE);
    }
}

void Facet::dispatch_observed(int64_t peer) {
    for (int at = 0; at < observed_callbacks.size(); ++at) {
        const Callable held = observed_callbacks[at];
        if (held.is_valid()) {
            held.call(peer);
        }
    }
}

void Facet::dispatch_unobserved(int64_t peer) {
    for (int at = 0; at < unobserved_callbacks.size(); ++at) {
        const Callable held = unobserved_callbacks[at];
        if (held.is_valid()) {
            held.call(peer);
        }
    }
}

} // namespace netw::interest
