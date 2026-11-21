#pragma once

#include <cstdint>

#include "godot/hash_map.hpp"
#include "godot/variant.hpp"
#include "netw/fault_catalog.hpp"

namespace netw {

class NetwMultiplayer;

struct FaultValues {
    godot::String name;
    godot::String owner;
    godot::String detail;
    int64_t peer = 0;
    int64_t a = 0;
    int64_t b = 0;
};

struct FaultText {
    godot::String what;
    godot::String fix;
};

enum class FaultLang {
    GDSCRIPT,
    CSHARP,
};

struct FaultKey {
    netw_fault_code code = NETW_FAULT_MALFORMED_DATAGRAM;
    uint64_t subject = 0;
    int64_t peer = 0;
    uint32_t arg = 0;
};

class FaultBook {
public:
    static constexpr uint32_t KEYS = 1024;
    static constexpr uint32_t KEYS_PER_PEER = 32;
    static constexpr uint32_t NEW_PER_PEER = 8;
    static constexpr int64_t NEW_PER_PEER_TICKS = 30;

    struct Admit {
        bool print = false;
        godot::String *what = nullptr;
    };

    Admit admit(const FaultKey &p_key, int64_t p_tick);
    godot::PackedStringArray end_session();

private:
    struct Entry {
        netw_fault_code code = NETW_FAULT_MALFORMED_DATAGRAM;
        uint32_t repeats = 0;
        int64_t last_tick = 0;
        godot::String what;
    };

    struct PeerKeys {
        uint32_t held = 0;
        uint32_t opened = 0;
        int64_t window_start = 0;
    };

    Admit count_on(const FaultKey &p_key, int64_t p_tick);

    godot::HashMap<uint64_t, Entry> entries;
    godot::HashMap<int64_t, PeerKeys> peers;
    uint32_t held = 0;
};

netw_fault_row fault_row(netw_fault_code p_code);
FaultText fault_text(netw_fault_code p_code, const FaultValues &p_values);
FaultText bug_text(const godot::String &p_where);
godot::String fault_spell(const godot::String &p_text, FaultLang p_lang);

namespace fault {

void raise(
    NetwMultiplayer *p_session,
    netw_fault_code p_code,
    const FaultValues &p_values,
    uint64_t p_subject = 0
);
void forget();

} // namespace fault

} // namespace netw
