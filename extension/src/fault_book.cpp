#include "netw/fault_book.hpp"

#include "godot/object.hpp"
#include "godot/templates.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/decl_sites.hpp"
#include "netw/log.hpp"

using namespace godot;

namespace netw {

namespace {

uint64_t mixed(uint64_t p_hash, uint64_t p_value) {
    p_hash ^= p_value + 0x9e3779b97f4a7c15ULL + (p_hash << 6) + (p_hash >> 2);
    return p_hash;
}

uint64_t key_of(const FaultKey &p_key) {
    uint64_t hash = mixed(0, uint64_t(p_key.code));
    hash = mixed(hash, p_key.subject);
    hash = mixed(hash, uint64_t(p_key.peer));
    return mixed(hash, p_key.arg);
}

struct SpelledConstant {
    const char *gdscript;
    const char *csharp;
};

const SpelledConstant CSHARP_CONSTANTS[] = {
    {"NetwEntity.AUTHORITY_SERVER", "NetwEntity.AuthorityEnum.Server"},
    {"NetwEntity.AUTHORITY_CONTROLLER", "NetwEntity.AuthorityEnum.Controller"},
    {"NetwEntity.TRANSFER_FIXED", "NetwEntity.TransferEnum.Fixed"},
    {"NetwEntity.TRANSFER_REQUESTABLE", "NetwEntity.TransferEnum.Requestable"},
    {"NetwEntity.TRANSFER_IMMEDIATE", "NetwEntity.TransferEnum.Immediate"},
    {"NetwSimulation.SCHEDULE_TICK", "NetwSimulation.ScheduleEnum.Tick"},
    {"NetwSimulation.SCHEDULE_FRAME", "NetwSimulation.ScheduleEnum.Frame"},
    {"NetwSimulation.SCHEDULE_STEPPED", "NetwSimulation.ScheduleEnum.Stepped"},
    {"NetwSimulation.SCHEDULE_AUTO", "NetwSimulation.ScheduleEnum.Auto"},
    {"NetwPrediction.RECOVERY_POLICY_REBASE_REPLAY",
     "NetwPrediction.RecoveryPolicyEnum.RebaseReplay"},
    {"NetwPrediction.RECOVERY_POLICY_REBASE_RECOVER",
     "NetwPrediction.RecoveryPolicyEnum.RebaseRecover"},
    {"NetwPrediction.RECOVERY_POLICY_DELAY_CLOSED",
     "NetwPrediction.RecoveryPolicyEnum.DelayClosed"},
    {"NetwPrediction.RECOVERY_POLICY_OBSERVE",
     "NetwPrediction.RecoveryPolicyEnum.Observe"},
    {"NetwPrediction.RECOVERY_POLICY_AUTO",
     "NetwPrediction.RecoveryPolicyEnum.Auto"},
    {"NetwPrediction.RECONCILE_INDEPENDENT",
     "NetwPrediction.Reconcile.Independent"},
    {"NetwPrediction.RECONCILE_JOINT", "NetwPrediction.Reconcile.Joint"},
    {"NetwSession.ROLE_CLIENT", "NetwSession.RoleEnum.Client"},
    {"NetwSession.ROLE_DEDICATED_SERVER",
     "NetwSession.RoleEnum.DedicatedServer"},
    {"NetwSession.ROLE_LISTEN_SERVER", "NetwSession.RoleEnum.ListenServer"},
};

String pascal(const String &p_snake) {
    const PackedStringArray parts = p_snake.split("_");
    String spelled;
    for (int index = 0; index < parts.size(); ++index) {
        const String part = parts[index];
        if (part.is_empty()) {
            spelled += "_";
            continue;
        }
        spelled += part.substr(0, 1).to_upper();
        for (int at = 1; at < part.length(); ++at) {
            const String letter = part.substr(at, 1);
            spelled += is_digit(part[at - 1]) ? letter.to_upper() : letter;
        }
    }
    return spelled;
}

String csharp_name(const String &p_name) {
    for (const SpelledConstant &constant : CSHARP_CONSTANTS) {
        if (p_name == constant.gdscript) {
            return String(constant.csharp);
        }
    }
    const int dot = p_name.rfind(".");
    if (dot < 0) {
        return p_name;
    }
    return p_name.substr(0, dot + 1) + pascal(p_name.substr(dot + 1));
}

} // namespace

FaultBook::Admit FaultBook::admit(const FaultKey &p_key, int64_t p_tick) {
    Entry *found = entries.getptr(key_of(p_key));
    if (found != nullptr) {
        found->repeats += 1;
        found->last_tick = p_tick;
        return Admit();
    }
    if (held >= KEYS) {
        return count_on({p_key.code, 0, 0, 0}, p_tick);
    }
    if (p_key.peer != 0) {
        PeerKeys &peer = peers[p_key.peer];
        if (peer.opened == 0
            || p_tick - peer.window_start >= NEW_PER_PEER_TICKS) {
            peer.window_start = p_tick;
            peer.opened = 0;
        }
        if (peer.held >= KEYS_PER_PEER || peer.opened >= NEW_PER_PEER) {
            return count_on({p_key.code, 0, p_key.peer, 0}, p_tick);
        }
        peer.held += 1;
        peer.opened += 1;
    }
    held += 1;
    Entry &entry
        = entries.insert(key_of(p_key), {p_key.code, 0, p_tick, String()})
              ->value;
    return {true, &entry.what};
}

FaultBook::Admit FaultBook::count_on(const FaultKey &p_key, int64_t p_tick) {
    Entry *found = entries.getptr(key_of(p_key));
    if (found != nullptr) {
        found->repeats += 1;
        found->last_tick = p_tick;
        return Admit();
    }
    Entry &entry
        = entries.insert(key_of(p_key), {p_key.code, 1, p_tick, String()})
              ->value;
    return {false, &entry.what};
}

PackedStringArray FaultBook::end_session() {
    PackedStringArray lines;
    for (const KeyValue<uint64_t, Entry> &row : entries) {
        if (row.value.repeats == 0) {
            continue;
        }
        lines.push_back(vformat(
            "NW%04d happened %d more times during the session, last at tick "
            "%d. %s",
            int(row.value.code),
            int64_t(row.value.repeats),
            row.value.last_tick,
            row.value.what
        ));
    }
    entries.clear();
    peers.clear();
    held = 0;
    return lines;
}

netw_fault_row fault_row(netw_fault_code p_code) {
    switch (p_code) {
#define NETW_FAULT_ROW(m_name, m_id, m_class, m_aud, m_subj, m_site, m_fwd) \
    case NETW_FAULT_##m_name: \
        return { \
            uint8_t(NETW_FAULT_##m_class), \
            uint8_t(NETW_AUDIENCE_##m_aud), \
            uint8_t(NETW_SUBJECT_##m_subj), \
            uint8_t(NETW_SITE_##m_site), \
            uint8_t(m_fwd##_FORWARD) \
        };
        NETW_FAULTS(NETW_FAULT_ROW)
#undef NETW_FAULT_ROW
    }
    return {};
}

String fault_spell(const String &p_text, FaultLang p_lang) {
    String spelled;
    int from = 0;
    while (true) {
        const int open = p_text.find("{", from);
        const int close = open < 0 ? -1 : p_text.find("}", open);
        if (close < 0) {
            return spelled + p_text.substr(from);
        }
        const String name = p_text.substr(open + 1, close - open - 1);
        spelled += p_text.substr(from, open - from);
        spelled += p_lang == FaultLang::CSHARP ? csharp_name(name) : name;
        from = close + 1;
    }
}

namespace fault {

void raise(
    NetwMultiplayer *p_session,
    netw_fault_code p_code,
    const FaultValues &p_values,
    uint64_t p_subject
) {
    FaultBook::Admit admit = {true, nullptr};
    if (p_session != nullptr) {
        admit = p_session->fault_book().admit(
            {p_code, p_subject, p_values.peer, p_values.name.hash()},
            p_session->clock_get_tick()
        );
        if (admit.what == nullptr) {
            return;
        }
    }

    const netw_fault_row row = fault_row(p_code);
    Object *subject
        = p_subject == 0 ? nullptr : gd::object_of(ObjectID(p_subject));
    const gd::ScriptSite site = decl_sites::find(
        netw_fault_site(row.site),
        subject,
        StringName(p_values.name)
    );
    const FaultLang lang = site.file.ends_with(".cs") ? FaultLang::CSHARP
                                                      : FaultLang::GDSCRIPT;
    const FaultText text = fault_text(p_code, p_values);
    const String what = fault_spell(text.what, lang);
    if (admit.what != nullptr) {
        *admit.what = what;
    }
    if (!admit.print) {
        return;
    }

    const String line = what + String(" ") + fault_spell(text.fix, lang)
        + vformat(" [NW%04d]", int(p_code));
    const log::Level level = row.klass == NETW_FAULT_MISUSE ? log::Level::ERROR
                                                            : log::Level::WARN;
    if (site.file.is_empty()) {
        log::report(level, line, "", "Networked", 0);
        return;
    }
    const CharString file = site.file.utf8();
    const CharString function = site.function.utf8();
    log::report(level, line, function.get_data(), file.get_data(), site.line);
}

} // namespace fault

} // namespace netw
