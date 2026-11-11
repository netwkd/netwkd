#include "support/netw_test.h"

#if defined(NETW_TIER_HOSTED)

#include "support/fault_log.h"

#include "godot/node.hpp"
#include "godot/object.hpp"
#include "godot/script.hpp"
#include "netw/fault_book.hpp"

namespace TestFaultSiteLaws {

using godot::Callable;
using godot::Node;
using godot::Object;
using godot::String;
using netw::FaultValues;

const char *FIXTURE = "res://tests/native/fault_site_fixture.gd";

struct SiteRow {
    const char *site;
    netw_fault_code code;
    const char *name;
    const char *file;
    int line;
};

const SiteRow SITE_ROWS[] = {
    {"CALL", NETW_FAULT_SERVER_ONLY, "kick", FIXTURE, 22},
    {"COLUMN", NETW_FAULT_WRITE_REFUSED, "speed", FIXTURE, 7},
    {"RPC", NETW_FAULT_RPC_REFUSED, "hit", FIXTURE, 8},
    {"CLOCK", NETW_FAULT_TICK_SPLITS_PHYSICS, "", FIXTURE, 9},
    {"HANDLER", NETW_FAULT_SPAWN_RETURN, "make", FIXTURE, 10},
    {"NONE", NETW_FAULT_MALFORMED_DATAGRAM, "", "Networked", 0},
};

const SiteRow *calling_row = nullptr;
uint64_t calling_subject = 0;

void raise_from_call() {
    netw::fault::raise(
        nullptr,
        calling_row->code,
        {calling_row->name},
        calling_subject
    );
}

TEST_CASE("[Networked][Faults] a fault lands on the line a game wrote") {
    const godot::Ref<godot::Script> script = netw::gd::load_script(FIXTURE);
    REQUIRE(script.is_valid());
    Object *fixture = script->call("new");
    REQUIRE(fixture != nullptr);
    const uint64_t subject = uint64_t(fixture->get_instance_id());

    for (const SiteRow &row : SITE_ROWS) {
        CAPTURE(row.site);
        netw_test::FaultWatch watch;
        const String tag = godot::vformat("[NW%04d]", int(row.code));
        if (String(row.site) == "CALL") {
            calling_row = &row;
            calling_subject = subject;
            fixture->call("call_back", callable_mp_static(&raise_from_call));
        } else {
            netw::fault::raise(nullptr, row.code, {row.name}, subject);
        }
        int landed = 0;
        for (const netw_test::FaultLine &line : watch.log->recorded()) {
            if (!line.message.ends_with(tag)) {
                continue;
            }
            landed += 1;
            NETW_CHECK_EQ(line.file == String(row.file), true);
            NETW_CHECK_EQ(line.line, row.line);
        }
        NETW_CHECK_EQ(landed, 1);
    }

    fixture->call("free");
}

} // namespace TestFaultSiteLaws

#endif
