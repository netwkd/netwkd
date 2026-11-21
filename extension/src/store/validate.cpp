#include "netw/sealed.hpp"

#include "netw/store/validate.hpp"

#include <atomic>

using namespace godot;

namespace netw::store {

namespace {

std::atomic<int64_t> interval{0};

} // namespace

void configure(const String &p_setting) {
    const int64_t every = p_setting.is_valid_int() ? p_setting.to_int() : 0;
    interval.store(every > 0 ? every : 0, std::memory_order_relaxed);
}

int64_t validation_interval() {
    return interval.load(std::memory_order_relaxed);
}

void report_miss(const Miss &p_miss) {
    NETW_BUG_REPORT();
#if defined(NETW_TESTS)
    validation_audit::Ledger &book = validation_audit::ledger();
    if (book.faults == 0) {
        const String detail = vformat(
            "route %d slot %d column '%s' tick %d gate %s",
            int64_t(p_miss.route),
            int64_t(p_miss.slot),
            String(p_miss.column),
            p_miss.tick,
            String(p_miss.gate)
        );
        book.first = std::string(detail.utf8().get_data());
    }
    book.faults += 1;
#endif
}

#if defined(NETW_TESTS)

namespace validation_audit {

Ledger &ledger() {
    static Ledger held;
    return held;
}

} // namespace validation_audit

int64_t &allocations_seen() {
    static int64_t seen = 0;
    return seen;
}

int64_t &dictionary_writes_seen() {
    static int64_t seen = 0;
    return seen;
}

int64_t &peer_reads_seen() {
    static int64_t seen = 0;
    return seen;
}

#endif

} // namespace netw::store
