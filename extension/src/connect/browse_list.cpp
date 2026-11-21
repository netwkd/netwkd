#include "netw/connect/browse_list.hpp"

#include <limits>

#include "godot/object.hpp"
#include "godot/rid.hpp"

using namespace godot;

namespace netw::connect {

namespace {

const char *TARGET_ADDRESS = "address";
const char *TARGET_DISPLAY_NAME = "display_name";
const char *TARGET_INFO = "info";

int index_of(const LocalVector<RID> &p_rows, const RID &p_target) {
    for (uint32_t at = 0; at < p_rows.size(); at++) {
        if (p_rows[at] == p_target) {
            return int(at);
        }
    }
    return -1;
}

} // namespace

TypedArray<Dictionary> targets_of(
    const PackedStringArray &p_addresses,
    const PackedStringArray &p_names,
    const Array &p_infos
) {
    TypedArray<Dictionary> targets;
    for (int64_t at = 0; at < p_addresses.size(); at++) {
        Dictionary target;
        target[TARGET_ADDRESS] = p_addresses[at];
        target[TARGET_DISPLAY_NAME]
            = at < p_names.size() ? p_names[at] : String();
        target[TARGET_INFO] = at < p_infos.size() ? p_infos[at] : Variant();
        targets.push_back(target);
    }
    return targets;
}

BrowseList::BrowseList() {
    targets.set_description("netw::connect target");
}

BrowseList::~BrowseList() {
    for (uint32_t at = 0; at < caller_rows.size(); at++) {
        release(caller_rows[at]);
    }
    for (uint32_t at = 0; at < directory_rows.size(); at++) {
        release(directory_rows[at]);
    }
    release(current);
}

void BrowseList::set_local_app_id(const StringName &p_app_id) {
    local_app_id = p_app_id;
}

RID BrowseList::mint(const TargetRow &p_row) {
    return targets.make_rid(p_row);
}

void BrowseList::release(const RID &p_target) {
    if (targets.owns(p_target)) {
        targets.free(p_target);
    }
}

TargetRow *BrowseList::row(const RID &p_target) {
    return targets.get_or_null(p_target);
}

const TargetRow *BrowseList::row(const RID &p_target) const {
    return targets.get_or_null(p_target);
}

RID BrowseList::find(const RID &p_transport, const String &p_address) const {
    for (uint32_t at = 0; at < caller_rows.size(); at++) {
        const TargetRow *held = row(caller_rows[at]);
        if (held != nullptr && held->transport == p_transport
            && held->address == p_address) {
            return caller_rows[at];
        }
    }
    for (uint32_t at = 0; at < directory_rows.size(); at++) {
        const TargetRow *held = row(directory_rows[at]);
        if (held != nullptr && held->transport == p_transport
            && held->address == p_address) {
            return directory_rows[at];
        }
    }
    return RID();
}

bool BrowseList::is_removable(const RID &p_target) const {
    if (index_of(caller_rows, p_target) >= 0) {
        return true;
    }
    const TargetRow *held = row(p_target);
    return held != nullptr && held->invited;
}

bool BrowseList::holds(const RID &p_target) const {
    return p_target.is_valid()
        && (p_target == current || index_of(caller_rows, p_target) >= 0
            || index_of(directory_rows, p_target) >= 0);
}

RID BrowseList::hold_current(
    const RID &p_transport,
    const String &p_address,
    RID &r_dropped
) {
    const TargetRow *held = row(current);
    if (held != nullptr && held->transport == p_transport
        && held->address == p_address) {
        return current;
    }
    r_dropped = drop_current();
    TargetRow made;
    made.transport = p_transport;
    made.address = p_address;
    current = mint(made);
    return current;
}

RID BrowseList::drop_current() {
    const RID dropped = current;
    current = RID();
    release(dropped);
    return dropped;
}

RID BrowseList::add(
    const RID &p_transport,
    const String &p_address,
    const String &p_display_name
) {
    const RID held = find(p_transport, p_address);
    if (held.is_valid()) {
        return held;
    }
    TargetRow made;
    made.transport = p_transport;
    made.address = p_address;
    made.display_name = p_display_name;
    const RID minted = mint(made);
    caller_rows.push_back(minted);
    return minted;
}

bool BrowseList::remove(const RID &p_target) {
    if (!is_removable(p_target)) {
        return false;
    }
    const int caller = index_of(caller_rows, p_target);
    if (caller >= 0) {
        caller_rows.remove_at(uint32_t(caller));
    }
    const int listed = index_of(directory_rows, p_target);
    if (listed >= 0) {
        directory_rows.remove_at(uint32_t(listed));
    }
    const int queued = index_of(queue, p_target);
    if (queued >= 0) {
        queue.remove_at(uint32_t(queued));
    }
    release(p_target);
    return true;
}

TypedArray<RID> BrowseList::list() const {
    TypedArray<RID> out;
    for (uint32_t at = 0; at < caller_rows.size(); at++) {
        out.push_back(caller_rows[at]);
    }
    for (uint32_t at = 0; at < directory_rows.size(); at++) {
        out.push_back(directory_rows[at]);
    }
    return out;
}

int BrowseList::size() const {
    return int(caller_rows.size() + directory_rows.size());
}

void BrowseList::publish_directory(
    const StringName &p_directory,
    ObjectID p_source,
    const RID &p_transport,
    const TypedArray<Dictionary> &p_targets,
    LocalVector<RID> &r_dropped,
    LocalVector<RID> &r_added,
    LocalVector<RID> &r_updated
) {
    LocalVector<RID> standing;
    for (int64_t at = 0; at < p_targets.size(); at++) {
        const Dictionary target = p_targets[at];
        const String address = target.get(TARGET_ADDRESS, String());
        const String named = target.get(TARGET_DISPLAY_NAME, String());
        const Ref<NetwServerInfo> advertised = Ref<NetwServerInfo>(
            Object::cast_to<NetwServerInfo>(
                gd::live_object(target.get(TARGET_INFO, Variant()))
            )
        );
        RID held = find(p_transport, address);
        const bool minted = !held.is_valid();
        if (minted) {
            TargetRow made;
            made.transport = p_transport;
            made.address = address;
            held = mint(made);
            directory_rows.push_back(held);
        }
        TargetRow *seat = row(held);
        if (seat == nullptr) {
            continue;
        }
        seat->display_name = named;
        seat->directory = p_directory;
        seat->source = p_source;
        seat->advertised = advertised;
        if (advertised.is_valid()) {
            seat->status = classify(advertised, OK);
            seat->info = advertised;
            seat->observed = true;
        }
        standing.push_back(held);
        if (minted) {
            r_added.push_back(held);
        } else {
            r_updated.push_back(held);
        }
    }
    for (int32_t at = int32_t(directory_rows.size()) - 1; at >= 0; at--) {
        const RID held = directory_rows[uint32_t(at)];
        const TargetRow *seat = row(held);
        if (seat == nullptr || seat->invited || seat->directory != p_directory
            || seat->source != p_source) {
            continue;
        }
        if (index_of(standing, held) >= 0) {
            continue;
        }
        r_dropped.push_back(held);
        directory_rows.remove_at(uint32_t(at));
        release(held);
    }
}

RID BrowseList::invite(
    const StringName &p_directory,
    ObjectID p_source,
    const RID &p_transport,
    const String &p_address,
    bool &r_minted
) {
    const RID held = find(p_transport, p_address);
    r_minted = !held.is_valid();
    if (!r_minted) {
        TargetRow *seat = row(held);
        if (seat != nullptr) {
            seat->invited = true;
        }
        return held;
    }
    TargetRow made;
    made.transport = p_transport;
    made.address = p_address;
    made.directory = p_directory;
    made.source = p_source;
    made.invited = true;
    const RID minted = mint(made);
    directory_rows.push_back(minted);
    return minted;
}

void BrowseList::forget_source(ObjectID p_source, LocalVector<RID> &r_dropped) {
    for (int32_t at = int32_t(directory_rows.size()) - 1; at >= 0; at--) {
        const RID held = directory_rows[uint32_t(at)];
        const TargetRow *seat = row(held);
        if (seat != nullptr && seat->source != p_source) {
            continue;
        }
        r_dropped.push_back(held);
        directory_rows.remove_at(uint32_t(at));
        release(held);
    }
}

void BrowseList::enqueue(const RID &p_target) {
    queue.push_back(p_target);
}

void BrowseList::arm_caller_rows() {
    for (uint32_t at = 0; at < caller_rows.size(); at++) {
        TargetRow *held = row(caller_rows[at]);
        if (held != nullptr) {
            held->next_probe = 0.0;
        }
    }
}

void BrowseList::enqueue_due(double p_elapsed, double p_interval) {
    for (uint32_t at = 0; at < caller_rows.size(); at++) {
        TargetRow *held = row(caller_rows[at]);
        if (held == nullptr) {
            continue;
        }
        held->next_probe -= p_elapsed;
        if (held->next_probe > 0.0 || held->probing
            || index_of(queue, caller_rows[at]) >= 0) {
            continue;
        }
        queue.push_back(caller_rows[at]);
        held->next_probe = p_interval > 0.0
            ? p_interval
            : std::numeric_limits<double>::infinity();
    }
}

void BrowseList::drop_probes() {
    queue.clear();
    probes_in_flight = 0;
    for (uint32_t at = 0; at < caller_rows.size(); at++) {
        TargetRow *held = row(caller_rows[at]);
        if (held != nullptr) {
            held->probing = false;
        }
    }
}

bool BrowseList::next_to_probe(RID &r_target) {
    while (!queue.is_empty() && probes_in_flight < MAX_IN_FLIGHT_PROBES) {
        const RID held = queue[0];
        queue.remove_at(0);
        TargetRow *seat = row(held);
        if (seat == nullptr) {
            continue;
        }
        seat->probing = true;
        r_target = held;
        probes_in_flight++;
        return true;
    }
    return false;
}

bool BrowseList::begin_probe(const RID &p_target) {
    TargetRow *seat = row(p_target);
    if (seat == nullptr || seat->probing || p_target == current) {
        return false;
    }
    const int queued = index_of(queue, p_target);
    if (queued >= 0) {
        queue.remove_at(uint32_t(queued));
    }
    seat->probing = true;
    probes_in_flight++;
    return true;
}

void BrowseList::probe_settled(
    const RID &p_target,
    int64_t p_error,
    const Ref<NetwServerInfo> &p_info
) {
    if (probes_in_flight > 0) {
        probes_in_flight--;
    }
    TargetRow *seat = row(p_target);
    if (seat == nullptr) {
        return;
    }
    seat->probing = false;
    seat->status = classify(p_info, p_error);
    seat->info = p_info;
    seat->observed = true;
}

int64_t BrowseList::classify(
    const Ref<NetwServerInfo> &p_info,
    int64_t p_error
) const {
    if (p_error != OK) {
        return p_error;
    }
    if (p_info.is_valid() && local_app_id != p_info->get_app_id()) {
        return ERR_UNAUTHORIZED;
    }
    return OK;
}

} // namespace netw::connect
