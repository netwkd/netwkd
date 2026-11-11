#pragma once

#include "tests/compliance/world.h"
#include "tests/support/netw_cells.h"

#include "godot/file_access.hpp"
#include "godot/os.hpp"

#if defined(NETW_TIER_HOSTED)

namespace netw_test {

constexpr int NEVER = 0;
constexpr int ONCE = 1;

inline std::string said(const godot::Variant &p_value) {
    return std::string(p_value.stringify().utf8().get_data());
}

inline LawVerdict reads(
    const Seen &p_seen,
    int p_peer,
    const char *p_name,
    const char *p_fact,
    const godot::Variant &p_value,
    int p_tick = LAST
) {
    const int tick = p_tick == LAST ? p_seen.ticks : p_tick;
    const Sight *held = p_seen.sight(p_peer, p_name, tick);
    if (held == nullptr) {
        return law_broken(
            "%s holds no '%s' at tick %d",
            who(p_peer).c_str(),
            p_name,
            tick
        );
    }
    if (held->facts.get(p_fact, godot::Variant()) != p_value) {
        return law_broken(
            "%s reads '%s' %s as %s at tick %d, not %s",
            who(p_peer).c_str(),
            p_name,
            p_fact,
            said(held->facts.get(p_fact, godot::Variant())).c_str(),
            tick,
            said(p_value).c_str()
        );
    }
    return law_held();
}

inline LawVerdict agrees(const Seen &p_seen, int p_client, int p_tick = LAST) {
    const int tick = p_tick == LAST ? p_seen.ticks : p_tick;
    for (const Sight &held : p_seen.sights) {
        if (held.tick != tick
            || (held.peer != SERVER && held.peer != p_client)) {
            continue;
        }
        const int other = held.peer == SERVER ? p_client : SERVER;
        const Sight *twin = p_seen.sight(other, held.name, tick);
        if (twin == nullptr) {
            return law_broken(
                "%s holds '%s' at tick %d and %s does not",
                who(held.peer).c_str(),
                said(held.name).c_str(),
                tick,
                who(other).c_str()
            );
        }
        for (const godot::Variant &fact : held.facts.keys()) {
            if (twin->facts[fact] == held.facts[fact]) {
                continue;
            }
            return law_broken(
                "%s reads '%s' %s as %s at tick %d and %s as %s",
                who(held.peer).c_str(),
                said(held.name).c_str(),
                said(fact).c_str(),
                said(held.facts[fact]).c_str(),
                tick,
                who(other).c_str(),
                said(twin->facts[fact]).c_str()
            );
        }
    }
    return law_held();
}

inline LawVerdict same_node(
    const Seen &p_seen,
    int p_peer,
    const char *p_name
) {
    uint64_t node = 0;
    int reached = 0;
    for (const Sight &held : p_seen.sights) {
        if (held.peer != p_peer || held.name != godot::String(p_name)) {
            continue;
        }
        if (node != 0 && (held.node != node || held.tick != reached + 1)) {
            return law_broken(
                "%s freed its '%s' at tick %d and made another",
                who(p_peer).c_str(),
                p_name,
                reached + 1
            );
        }
        node = held.node;
        reached = held.tick;
    }
    if (reached != p_seen.ticks) {
        return law_broken(
            "%s holds no '%s' after tick %d",
            who(p_peer).c_str(),
            p_name,
            reached
        );
    }
    return law_held();
}

inline LawVerdict absent(
    const Seen &p_seen,
    int p_peer,
    const char *p_name,
    int p_from = LAST
) {
    const int from = p_from == LAST ? p_seen.ticks : p_from;
    for (const Sight &held : p_seen.sights) {
        if (held.peer == p_peer && held.tick >= from
            && held.name == godot::String(p_name)) {
            return law_broken(
                "%s holds '%s' at tick %d",
                who(p_peer).c_str(),
                p_name,
                held.tick
            );
        }
    }
    return law_held();
}

inline LawVerdict hears(
    const Seen &p_seen,
    int p_peer,
    const char *p_node,
    const char *p_signal,
    int p_times,
    int p_from = 1,
    int p_to = LAST
) {
    const int to = p_to == LAST ? p_seen.ticks : p_to;
    const int heard = p_seen.times(p_peer, p_node, p_signal, p_from, to);
    if (heard != p_times) {
        return law_broken(
            "%s heard '%s' %s %d times from tick %d to %d, not %d",
            who(p_peer).c_str(),
            p_node,
            p_signal,
            heard,
            p_from,
            to,
            p_times
        );
    }
    return law_held();
}

inline LawVerdict hears_with(
    const Seen &p_seen,
    int p_peer,
    const char *p_node,
    const char *p_signal,
    const godot::Array &p_args
) {
    std::string other = "nothing";
    for (const Heard &row : p_seen.heard) {
        if (row.peer != p_peer || row.node != godot::String(p_node)
            || row.signal != godot::String(p_signal)) {
            continue;
        }
        if (row.args == p_args) {
            return law_held();
        }
        other = said(row.args);
    }
    return law_broken(
        "%s heard '%s' %s with %s, not %s",
        who(p_peer).c_str(),
        p_node,
        p_signal,
        other.c_str(),
        said(p_args).c_str()
    );
}

inline LawVerdict quiet(const Seen &p_seen) {
    if (!p_seen.faults.is_empty()) {
        return law_broken(
            "%d errors or warnings were printed, the first '%s'",
            int(p_seen.faults.size()),
            said(p_seen.faults[0]).c_str()
        );
    }
    return law_held();
}

inline godot::String golden_path(const char *p_label, int p_peer) {
    return godot::vformat(
        "%sgolden/%s.w%d.netwcap",
        WORLDS,
        p_label,
        p_peer + 1
    );
}

inline LawVerdict golden(const Seen &p_seen, const char *p_label) {
    using godot::FileAccess;
    const bool recording
        = godot::OS::get_singleton()->get_cmdline_user_args().has("--record");
    if (p_seen.wire.empty()) {
        return law_broken("the world captured nothing. Declare capture()");
    }
    for (int peer = SERVER; peer + 1 < int(p_seen.wire.size()); ++peer) {
        const godot::String path = golden_path(p_label, peer);
        const godot::Ref<FileAccess> file = recording
            ? FileAccess::open(path, FileAccess::WRITE)
            : godot::Ref<FileAccess>();
        if (file.is_valid()) {
            file->store_buffer(p_seen.wire[peer + 1]);
            file->close();
        }
        if (!FileAccess::file_exists(path)) {
            return law_broken(
                "%s has no golden. Record one with -- --record",
                who(peer).c_str()
            );
        }
        const godot::String drift = capture::difference(
            FileAccess::get_file_as_bytes(path),
            p_seen.wire[peer + 1]
        );
        if (!drift.is_empty()) {
            return law_broken(
                "%s put other bytes on the wire than its golden. %s",
                who(peer).c_str(),
                said(drift).c_str()
            );
        }
    }
    const int beyond = int(p_seen.wire.size()) - 1;
    if (FileAccess::file_exists(golden_path(p_label, beyond))) {
        return law_broken("a golden exists for a peer this world never built");
    }
    return law_held();
}

} // namespace netw_test

#define NETW_HOLDS(m_law) \
    do { \
        const netw_test::LawVerdict netw_verdict = (m_law); \
        NETW_FORMAT_TEXT(broken, netw_verdict.detail); \
        CAPTURE(broken); \
        CHECK_MESSAGE(netw_verdict.held, #m_law); \
    } while (0)

#endif
