#include "support/netw_test.h"

#include <vector>

#include "godot/variant.hpp"
#include "netw/fault_book.hpp"

namespace TestFaultBookLaws {

using godot::PackedStringArray;
using godot::String;
using netw::FaultBook;
using netw::FaultLang;

struct Step {
    int64_t peer = 1;
    uint64_t subject = 0;
    int64_t tick = 0;
    int times = 1;
    int64_t ticks_apart = 0;
    bool last_prints = true;
    bool after_session_end = false;
};

struct BookRow {
    const char *law;
    std::vector<Step> steps;
    int summaries = 0;
    const char *text = nullptr;
    const char *csharp = nullptr;
};

const BookRow BOOK_ROWS[] = {
    {"a repeat counts and does not print", {{}, {1, 0, 0, 1, 0, false}}, 1},
    {"another peer prints", {{}, {2}}, 0},
    {"another subject prints", {{1, 5}, {1, 6}}, 0},
    {"a new session starts clean", {{}, {1, 0, 0, 1, 0, true, true}}, 0},
    {"a peer past 32 keys lands on its overflow key",
     {{1, 0, 0, 33, 30, false}},
     1},
    {"a peer past 8 new keys in 30 ticks lands on its overflow key",
     {{1, 0, 0, 9, 0, false}},
     1},
    {"a full book counts on the world's key", {{0, 0, 0, 1025, 0, false}}, 1},
    {"session end writes one summary per repeated key",
     {{1}, {1, 0, 0, 1, 0, false}, {2}, {2, 0, 0, 1, 0, false}, {3}},
     2},
    {"a member spells in PascalCase",
     {},
     0,
     "Call {Netw.configure_clock} first.",
     "Call Netw.ConfigureClock first."},
    {"a constant spells through the table",
     {},
     0,
     "Set {NetwEntity.TRANSFER_REQUESTABLE}.",
     "Set NetwEntity.TransferEnum.Requestable."},
    {"an override keeps its underscore",
     {},
     0,
     "Override {NetwTransport._peer_class}.",
     "Override NetwTransport._PeerClass."},
    {"a class name stays",
     {},
     0,
     "Assign a {MultiplayerPeer}.",
     "Assign a MultiplayerPeer."},
};

TEST_CASE("[Netw][Faults][Hosted] the fault book prints each key once") {
    for (const BookRow &row : BOOK_ROWS) {
        CAPTURE(row.law);
        FaultBook book;
        for (const Step &step : row.steps) {
            if (step.after_session_end) {
                book.end_session();
            }
            int printed = 0;
            bool last = false;
            for (int at = 0; at < step.times; ++at) {
                const FaultBook::Admit admit = book.admit(
                    {NETW_FAULT_MALFORMED_DATAGRAM,
                     step.subject,
                     step.peer,
                     String("k").hash() + uint32_t(at)},
                    step.tick + at * step.ticks_apart
                );
                if (admit.what != nullptr) {
                    *admit.what = "Peer sent a packet.";
                }
                printed += admit.print ? 1 : 0;
                last = admit.print;
            }
            NETW_CHECK_EQ(last, step.last_prints);
            NETW_CHECK_EQ(printed, step.times - (step.last_prints ? 0 : 1));
        }
        const PackedStringArray summaries = book.end_session();
        NETW_CHECK_EQ(summaries.size(), row.summaries);
        for (int at = 0; at < summaries.size(); ++at) {
            const String summary = summaries[at];
            NETW_CHECK_EQ(summary.begins_with("NW0007 happened "), true);
            NETW_CHECK_EQ(summary.ends_with(" Peer sent a packet."), true);
        }
        if (row.text != nullptr) {
            const String text = row.text;
            const String gdscript
                = netw::fault_spell(text, FaultLang::GDSCRIPT);
            const String csharp = netw::fault_spell(text, FaultLang::CSHARP);
            NETW_CHECK_EQ(
                gdscript == text.replace("{", "").replace("}", ""),
                true
            );
            NETW_CHECK_EQ(csharp == String(row.csharp), true);
        }
    }
}

} // namespace TestFaultBookLaws
