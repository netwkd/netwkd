#include "netw/fault_book.hpp"

#include "godot/node.hpp"
#include "godot/object.hpp"
#include "godot/script.hpp"
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
    {"NetwPrediction.MODE_ROLLBACK", "NetwPrediction.ModeEnum.Rollback"},
    {"NetwPrediction.MODE_OFFSET", "NetwPrediction.ModeEnum.Offset"},
    {"NetwPrediction.MODE_DISABLED", "NetwPrediction.ModeEnum.Disabled"},
    {"NetwPrediction.MODE_AUTO", "NetwPrediction.ModeEnum.Auto"},
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

namespace {

FaultBook &process_book() {
    static FaultBook book;
    return book;
}

Ref<Script> script_of(Object *p_subject) {
    return p_subject == nullptr
        ? Ref<Script>()
        : Ref<Script>(Object::cast_to<Script>(p_subject->get_script()));
}

bool written_by_the_game(const netw_fault_row &p_row) {
    return p_row.klass == NETW_FAULT_MISUSE
        || p_row.klass == NETW_FAULT_PRACTICE;
}

uint64_t mixed_value(
    uint64_t p_hash,
    const String &p_value,
    const String &p_node_name
) {
    const bool names_the_node = !p_node_name.is_empty()
        && (p_value == p_node_name
            || p_value == String("'") + p_node_name + String("'"));
    return names_the_node ? p_hash : mixed(p_hash, p_value.hash());
}

FaultKey process_key(
    netw_fault_code p_code,
    const FaultValues &p_values,
    Object *p_subject,
    uint64_t p_subject_id
) {
    const Ref<Script> script = script_of(p_subject);
    const Node *node = Object::cast_to<Node>(p_subject);
    const String node_name
        = node != nullptr ? String(node->get_name()) : String();
    uint64_t hash = mixed(0, uint64_t(p_values.peer));
    hash = mixed_value(hash, p_values.name, node_name);
    hash = mixed_value(hash, p_values.owner, node_name);
    hash = mixed_value(hash, p_values.detail, node_name);
    return {
        p_code,
        script.is_valid() ? uint64_t(gd::instance_id(script.ptr()))
                          : p_subject_id,
        0,
        uint32_t(hash ^ (hash >> 32))
    };
}

String unnamed_node_shown(Object *p_subject) {
    Node *node = Object::cast_to<Node>(p_subject);
    if (node == nullptr || !String(node->get_name()).is_empty()) {
        return String();
    }
    const Ref<Script> script = script_of(node);
    const String file
        = script.is_valid() ? script->get_path().get_file() : String();
    return file.is_empty() ? String(node->get_class()) : file;
}

String named(const String &p_text, const String &p_unnamed) {
    return p_unnamed.is_empty()
        ? p_text
        : p_text.replace("''", String("'") + p_unnamed + String("'"));
}

} // namespace

namespace fault {

void forget() {
    process_book().end_session();
}

void raise(
    NetwMultiplayer *p_session,
    netw_fault_code p_code,
    const FaultValues &p_values,
    uint64_t p_subject
) {
    Object *subject
        = p_subject == 0 ? nullptr : gd::object_of(ObjectID(p_subject));
    const netw_fault_row row = fault_row(p_code);
    FaultBook::Admit admit = {true, nullptr};
    if (p_session != nullptr) {
        admit = p_session->fault_admit(
            {p_code, p_subject, p_values.peer, p_values.name.hash()}
        );
    } else if (written_by_the_game(row)) {
        admit = process_book().admit(
            process_key(p_code, p_values, subject, p_subject),
            0
        );
    }
    if (!admit.print && admit.what == nullptr) {
        return;
    }

    const gd::ScriptSite site = decl_sites::find(
        netw_fault_site(row.site),
        subject,
        StringName(p_values.name)
    );
    const FaultLang lang = site.file.ends_with(".cs") ? FaultLang::CSHARP
                                                      : FaultLang::GDSCRIPT;
    const FaultText text = fault_text(p_code, p_values);
    const String unnamed = unnamed_node_shown(subject);
    const String what = named(fault_spell(text.what, lang), unnamed);
    if (admit.what != nullptr) {
        *admit.what = what;
    }
    if (!admit.print) {
        return;
    }

    const String line = what + String(" ")
        + named(fault_spell(text.fix, lang), unnamed)
        + vformat(" [NW%04d]", int(p_code));
    const log::Level level = row.klass == NETW_FAULT_MISUSE ? log::Level::ERROR
                                                            : log::Level::WARN;
    if (site.file.is_empty()) {
        log::report(level, line, "", "netwkd", 0);
        return;
    }
    const CharString file = site.file.utf8();
    const CharString function = site.function.utf8();
    log::report(level, line, function.get_data(), file.get_data(), site.line);
}

} // namespace fault

} // namespace netw
