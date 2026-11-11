#include "support/fault_log.h"
#include "support/netw_cells.h"
#include "support/netw_test.h"

#include <cmath>
#include <cstdint>

#include "godot/variant.hpp"
#include "netw/log.hpp"
#include "netw/monitor.hpp"

#if defined(DEBUG_ENABLED)
#include "godot/performance.hpp"
#include "netw/diag/hints.hpp"
#endif

namespace TestNetwDiagnostics {

using namespace godot;
using netw::log::Level;
using netw_test::law_broken;
using netw_test::law_held;
using netw_test::LawVerdict;

struct Stand {
    String label;
};

typedef netw_test::LawRowFor<Stand> Row;

struct LevelGuard {
    Level held = netw::log::level();

    ~LevelGuard() {
        netw::log::set_level(held);
    }
};

LawVerdict faults_at_every_level(const Stand &) {
    LevelGuard guard;
    netw_test::FaultWatch faults;
    faults.expect_warning("*a fault at every level*", 3);
    faults.expect_error("*a fault at every level*", 3);

    for (const Level each : {Level::TRACE, Level::WARN, Level::NONE}) {
        netw::log::set_level(each);
        netw::log::write(
            Level::WARN,
            netw::sys::SESSION,
            "a fault at every level"
        );
        netw::log::write(
            Level::ERROR,
            netw::sys::SESSION,
            "a fault at every level"
        );
    }

    if (faults.warnings_containing("a fault at every level") != 3) {
        return law_broken("a log level silenced a warning");
    }
    if (faults.errors_containing("a fault at every level") != 3) {
        return law_broken("a log level silenced an error");
    }
    return law_held();
}

LawVerdict typed_log_level(const Stand &) {
    struct Named {
        const char *typed;
        Level read;
    };
    const Named NAMES[] = {
        {"trace", Level::TRACE},
        {"debug", Level::DEBUG},
        {"info", Level::INFO},
        {"warn", Level::WARN},
        {"error", Level::ERROR},
        {"none", Level::NONE},
        {"  TRACE  ", Level::TRACE},
        {"core.network=trace", Level::INFO},
        {"", Level::INFO},
    };
    for (const Named &name : NAMES) {
        if (netw::log::level_named(name.typed) != name.read) {
            return law_broken(
                "'%s' read as level %d",
                name.typed,
                int(netw::log::level_named(name.typed))
            );
        }
    }
    return law_held();
}

#if defined(DEBUG_ENABLED)

PackedStringArray args_with(const String &p_flag) {
    PackedStringArray args;
    args.push_back("--headless");
    args.push_back(p_flag);
    return args;
}

uint32_t mask_of(const char *p_flag) {
    return netw::diag::parse_hints("", args_with(p_flag)).mask;
}

LawVerdict typed_hints(const Stand &) {
    const uint32_t prediction = netw::diag::HINT_PREDICTION;
    const uint32_t interpolation = netw::diag::HINT_INTERPOLATION;
    if (mask_of("--netw-hints=prediction") != prediction
        || mask_of("--netw-hints= Prediction ,") != prediction
        || mask_of("--netw-hints=interpolation") != interpolation
        || mask_of("--netw-hints=prediction,interpolation")
            != (prediction | interpolation)) {
        return law_broken("a hint name did not turn on its overlay");
    }
    if (mask_of("--netw-hints=") != 0 || mask_of("--netw-hints=none") != 0
        || netw::diag::parse_hints("", PackedStringArray()).mask != 0) {
        return law_broken("an overlay turned on that nobody asked for");
    }
    const netw::diag::HintRequest typo = netw::diag::parse_hints(
        "",
        args_with("--netw-hints=prediction,rings")
    );
    if (typo.mask != prediction || typo.unknown.size() != 1
        || typo.unknown[0] != String("rings")) {
        return law_broken("an unknown hint name was not reported");
    }
    return law_held();
}

netw::monitor::Upload upload_of(int64_t p_peer, int64_t p_sent) {
    netw::monitor::Upload upload;
    upload.peer = p_peer;
    upload.sent_bytes = p_sent;
    upload.budget_per_second = 4000.0;
    return upload;
}

bool near(double p_read, double p_want) {
    return std::abs(p_read - p_want) < 1.0e-9;
}

LawVerdict monitor_rates(const Stand &) {
    netw::monitor::Sample before;
    before.late_inputs = 10;
    before.corrections = 5;
    before.uploads.push_back(upload_of(2, 1000));
    netw::monitor::Sample now;
    now.peers = 2;
    now.late_inputs = 40;
    now.corrections = 2;
    now.replayed_ticks = 30;
    now.uploads.push_back(upload_of(2, 3000));
    now.uploads.push_back(upload_of(3, 500));

    double values[netw::monitor::COUNT] = {};
    netw::monitor::read(before, now, 0.5, values);
    if (!near(values[netw::monitor::PEERS], 2.0)
        || !near(values[netw::monitor::LATE_INPUTS], 60.0)
        || !near(values[netw::monitor::REPLAYED_TICKS], 60.0)
        || !near(values[netw::monitor::UPLOAD_BUDGET_USED], 1.0)) {
        return law_broken("a monitor did not read its rate per second");
    }
    if (!near(values[netw::monitor::CORRECTIONS], 0.0)) {
        return law_broken("a monitor ran backwards");
    }

    netw::monitor::read(before, now, 0.0, values);
    if (!near(values[netw::monitor::LATE_INPUTS], 0.0)
        || !near(values[netw::monitor::UPLOAD_BUDGET_USED], 0.0)) {
        return law_broken("a sample with no time between read a rate");
    }
    return law_held();
}

#if defined(NETW_TIER_HOSTED)
LawVerdict monitors_listed(const Stand &) {
    for (int at = 0; at < netw::monitor::COUNT; ++at) {
        const StringName id(netw::monitor::SPECS[at].id);
        if (!netw::gd::has_custom_monitor(id)) {
            return law_broken(
                "%s is not in Performance",
                netw::monitor::SPECS[at].id
            );
        }
    }
    return law_held();
}
#endif

#endif

const Row ROWS[] = {
    {"a warning or an error raised at the trace, warn and none levels",
     "every one reaches the console, since a level never silences a fault",
     &faults_at_every_level},
    {"a --netw-log= level a developer types",
     "each name reads its level, case and spaces aside, and anything else "
     "reads info",
     &typed_log_level},
#if defined(DEBUG_ENABLED)
    {"a --netw-hints= list a developer types",
     "each name turns on its overlay, none or nothing turns on none, and a "
     "typo is reported",
     &typed_hints},
    {"the Networked monitors over two samples",
     "each rate is per second from the change between them and never runs "
     "backwards",
     &monitor_rates},
#if defined(NETW_TIER_HOSTED)
    {"the Monitors tab of a debug build",
     "it lists every Networked monitor",
     &monitors_listed},
#endif
#endif
};

TEST_CASE(
    "[Networked][Diag] what a developer types and reads while debugging"
) {
    const Stand stand = {String("a debug build")};
    for (const Row &row : ROWS) {
        NETW_CELL(row, stand);
        NETW_LAW_HOLDS(row, stand);
    }
}

} // namespace TestNetwDiagnostics
