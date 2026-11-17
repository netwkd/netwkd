#define DOCTEST_CONFIG_IMPLEMENT
#include "support/netw_test.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "godot/class_db.hpp"
#include "godot/project_settings.hpp"
#include "netw/api/context.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/interest.hpp"
#include "netw/api/interpolate.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/prediction.hpp"
#include "netw/api/tests.hpp"
#include "netw/colors.hpp"
#include "netw/connect/tracker_client.hpp"
#include "netw/display/pump_stats.hpp"
#include "netw/display/timing.hpp"
#include "netw/interest/relay.hpp"
#include "netw/log.hpp"
#include "netw/predict/frames.hpp"
#include "netw/profile.hpp"
#include "netw/property_set_builder.hpp"
#include "netw/scene_membership.hpp"
#include "netw/schema_core.hpp"
#include "netw/schema_model.hpp"
#include "netw/script/model.hpp"
#include "netw/session/frames.hpp"
#include "netw/spawn/record.hpp"
#include "netw/sync_kernel.hpp"
#include "netw/wire/frame.hpp"
#include "netw/wire/registry.hpp"
#include "netw/wire/spec.hpp"
#include "support/fault_log.h"
#include "support/frame_drive.h"
#include "support/internals.h"
#include "support/netw_cells.h"
#include "support/netw_reset.h"

using namespace godot;
using netw_test::Internals;

NETW_INSTALL_RESET_LISTENER();

namespace netw {

namespace {

void probe_default_zone(int64_t value) {
    NETW_ZONE();
    NETW_ZONE_COLOR(colors::TICK);
    NETW_ZONE_VALUE(value);
    NETW_ZONE_TEXT_F("value=%lld", static_cast<long long>(value));
    NETW_ZONE_NAME_F("probe %lld", static_cast<long long>(value));
}

void probe_named_zone() {
    NETW_ZONE_N("netwkd named instrumentation probe");
}

void probe_colored_zone() {
    NETW_ZONE_C(colors::TICK);
}

void compile_ambient_profile_macros() {
    if (false) {
        NETW_THREAD("netwkd test", 1);
        NETW_LOCKABLE(std::mutex, lock);
        void *pointer = &lock;
        NETW_ALLOC_N(pointer, sizeof(lock), "netwkd test lock");
        NETW_FREE_N(pointer, "netwkd test lock");
        NETW_APP_INFO("netwkd native tests", 22);
    }
}

std::string escape_xml(const char *text) {
    std::string escaped;
    if (text == nullptr) {
        return escaped;
    }
    for (const char *cursor = text; *cursor != '\0'; ++cursor) {
        switch (*cursor) {
            case '&':
                escaped += "&amp;";
                break;
            case '<':
                escaped += "&lt;";
                break;
            case '>':
                escaped += "&gt;";
                break;
            case '\"':
                escaped += "&quot;";
                break;
            case '\'':
                escaped += "&apos;";
                break;
            default:
                escaped += *cursor;
                break;
        }
    }
    return escaped;
}

constexpr int FAILING_SHOWN = 10;
constexpr int CENSUS_SHOWN = 10;
constexpr int FAMILIES_SHOWN = 5;

struct Failing {
    std::string name;
    std::string detail;
};

struct RunSummary {
    int64_t cases = 0;
    int64_t failed = 0;
    std::vector<Failing> failing;
};

RunSummary &run_summary() {
    static RunSummary summary;
    return summary;
}

std::string first_line(const std::string &p_text) {
    return p_text.substr(0, p_text.find('\n'));
}

std::vector<std::string> &checks_failed_outside_a_case() {
    static std::vector<std::string> rows;
    return rows;
}

void note_check_outside_a_case(const doctest::AssertData &p_assert) {
    std::ostringstream row;
    row << p_assert.m_expr << " at " << p_assert.m_file << ":"
        << p_assert.m_line;
    const int count = doctest::IReporter::get_num_active_contexts();
    const doctest::IContextScope *const *scopes
        = doctest::IReporter::get_active_contexts();
    for (int index = 0; index < count; ++index) {
        row << ", ";
        scopes[index]->stringify(&row);
    }
    checks_failed_outside_a_case().push_back(row.str());
}

Ref<netw_test::FaultLog> &run_faults() {
    return netw_test::run_fault_log();
}

void open_run_faults() {
    netw_test::begin_fault_capture();
}

struct RunFaults {
    int64_t printed = 0;
    int64_t expected = 0;
    PackedStringArray unexpected;
};

RunFaults close_run_faults() {
    RunFaults split;
    if (run_faults().is_null()) {
        return split;
    }
    const std::vector<netw_test::FaultLine> printed = run_faults()->recorded();
    netw_test::write_fault_report(run_summary().failed == 0);
    netw_test::end_fault_capture();
    netw_test::FaultLedger &ledger = netw_test::fault_ledger();
    const std::lock_guard<std::mutex> held(ledger.lock);
    split.printed = int64_t(printed.size());
    for (const netw_test::FaultLine &line : printed) {
        if (ledger.asserted.count(line.serial) > 0) {
            split.expected += 1;
        } else {
            split.unexpected.push_back(line.text);
        }
    }
    return split;
}

std::string fault_family(const String &p_line) {
    const CharString text = p_line.strip_edges().utf8();
    std::string folded;
    bool quoted = false;
    bool in_number = false;
    for (const char *cursor = text.get_data(); *cursor != '\0'; ++cursor) {
        const char each = *cursor;
        if (each == '\'') {
            folded += quoted ? "...'" : "'";
            quoted = !quoted;
            in_number = false;
            continue;
        }
        if (quoted) {
            continue;
        }
        const bool digit = each >= '0' && each <= '9';
        if (digit && !in_number) {
            folded += 'N';
        } else if (!digit) {
            folded += each;
        }
        in_number = digit;
    }
    return folded;
}

Array top_fault_families(const PackedStringArray &p_faults, int64_t &r_count) {
    std::map<std::string, int64_t> counted;
    for (const String &line : p_faults) {
        counted[fault_family(line)] += 1;
    }
    r_count = int64_t(counted.size());
    std::vector<std::pair<std::string, int64_t>> ranked(
        counted.begin(),
        counted.end()
    );
    std::stable_sort(
        ranked.begin(),
        ranked.end(),
        [](const auto &p_a, const auto &p_b) { return p_a.second > p_b.second; }
    );
    Array top;
    for (int at = 0; at < int(ranked.size()) && at < FAMILIES_SHOWN; ++at) {
        Dictionary family;
        family[StringName("count")] = ranked[at].second;
        family[StringName("line")] = String::utf8(ranked[at].first.c_str());
        top.push_back(family);
    }
    return top;
}

String write_census(const String &p_report_path) {
    const String census_path
        = p_report_path.get_base_dir().path_join("census.txt");
    const CharString global
        = ProjectSettings::get_singleton()->globalize_path(census_path).utf8();
    FILE *output = std::fopen(global.get_data(), "wb");
    if (output == nullptr) {
        return String();
    }
    for (const netw_test::CensusRow &row : netw_test::census_ledger().rows) {
        std::fprintf(
            output,
            "objects %+lld resources %+lld freed by reset %lld  %s\n",
            (long long)row.objects,
            (long long)row.resources,
            (long long)row.freed_by_reset,
            row.name.c_str()
        );
    }
    std::fclose(output);
    return census_path;
}

PackedStringArray top_census_rows() {
    std::vector<netw_test::CensusRow> ranked = netw_test::census_ledger().rows;
    std::stable_sort(
        ranked.begin(),
        ranked.end(),
        [](const netw_test::CensusRow &p_a, const netw_test::CensusRow &p_b) {
            return p_a.objects > p_b.objects;
        }
    );
    PackedStringArray top;
    for (int at = 0; at < int(ranked.size()) && at < CENSUS_SHOWN; ++at) {
        top.push_back(vformat(
            "objects %+d resources %+d  %s",
            ranked[at].objects,
            ranked[at].resources,
            String::utf8(ranked[at].name.c_str())
        ));
    }
    return top;
}

class NetwJunitReporter final : public doctest::IReporter {
    struct Case {
        std::string file;
        std::string name;
        std::string detail;
        double seconds = 0.0;
        bool failure = false;
        bool error = false;
    };

    const doctest::ContextOptions &options;
    std::vector<Case> cases;
    std::vector<std::string> subcases;
    int64_t store_faults_opened = 0;
    int64_t store_faults_in_cases = 0;

    Case *current() {
        return cases.empty() ? nullptr : &cases.back();
    }

    void append_detail(const std::string &text) {
        Case *test_case = current();
        if (test_case == nullptr || text.empty()) {
            return;
        }
        if (!test_case->detail.empty()) {
            test_case->detail += "\n";
        }
        test_case->detail += text;
    }

    std::string subcase_prefix() const {
        std::string prefix;
        for (const std::string &name : subcases) {
            prefix += name;
            prefix += " / ";
        }
        return prefix;
    }

    std::string active_context() const {
        const int count = get_num_active_contexts();
        if (count == 0) {
            return std::string();
        }
        const doctest::IContextScope *const *scopes = get_active_contexts();
        std::ostringstream stream;
        for (int index = 0; index < count; ++index) {
            stream << "\n  logged: ";
            scopes[index]->stringify(&stream);
        }
        return stream.str();
    }

public:
    explicit NetwJunitReporter(const doctest::ContextOptions &context)
        : options(context) {
    }

    void report_query(const doctest::QueryData &) override {
    }
    void test_run_start() override {
    }

    void test_run_end(const doctest::TestRunStats &stats) override {
        int failures = 0;
        int errors = 0;
        for (const Case &test_case : cases) {
            failures += test_case.failure ? 1 : 0;
            errors += test_case.error ? 1 : 0;
        }
        const residency_audit::Ledger &audited = residency_audit::ledger();
        const bool diverged = audited.divergences > 0;
        const netw::store::validation_audit::Ledger &validated
            = netw::store::validation_audit::ledger();
        const int64_t stray = validated.faults - store_faults_in_cases;
        const std::vector<std::string> &outside
            = checks_failed_outside_a_case();
        const unsigned int reported = stats.numTestCasesPassingFilters
            + (diverged ? 1u : 0u) + (stray > 0 ? 1u : 0u)
            + (outside.empty() ? 0u : 1u);
        failures += (diverged ? 1 : 0) + (stray > 0 ? 1 : 0)
            + (outside.empty() ? 0 : 1);
        RunSummary &summary = run_summary();
        summary.cases = int64_t(reported);
        summary.failed = int64_t(failures + errors);
        summary.failing.clear();
        if (diverged) {
            summary.failing.push_back(
                {"JR0 scene residency", "an entity reason disagreed"}
            );
        }
        if (stray > 0) {
            summary.failing.push_back(
                {"SV0 store validation", validated.first.c_str()}
            );
        }
        if (!outside.empty()) {
            summary.failing.push_back(
                {"FS0 frame scenario checks", outside.front()}
            );
        }
        for (const Case &test_case : cases) {
            if (test_case.failure || test_case.error) {
                summary.failing.push_back(
                    {test_case.name, first_line(test_case.detail)}
                );
            }
        }
        FILE *output = std::fopen(options.out.c_str(), "wb");
        if (output == nullptr) {
            return;
        }
        std::fprintf(output, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
        std::fprintf(
            output,
            "<testsuites tests=\"%u\" failures=\"%d\">\n",
            reported,
            failures
        );
        std::fprintf(
            output,
            "  <testsuite name=\"netwkd-native\" tests=\"%u\" "
            "failures=\"%d\" errors=\"%d\">\n",
            reported,
            failures,
            errors
        );
        if (diverged) {
            std::fprintf(
                output,
                "    <testcase classname=\"scene_residency_laws.cpp\" "
                "name=\"JR0 every entity reason names an entity that resides "
                "in that scene, and every resident entity holds one, in every "
                "case the run executed\" time=\"0.0\"><failure>%lld of %lld "
                "readings disagreed. first membership %lld scene %lld entity "
                "%lld</failure></testcase>\n",
                (long long)audited.divergences,
                (long long)audited.observations,
                (long long)audited.member_of_first,
                (long long)audited.scene_of_first,
                (long long)audited.entity_of_first
            );
        }
        if (stray > 0) {
            const std::string first = escape_xml(validated.first.c_str());
            std::fprintf(
                output,
                "    <testcase classname=\"store_validation\" name=\"SV0 the "
                "store misses no change outside a case\" time=\"0.0\">"
                "<failure>%lld missed changes outside any case, of %lld "
                "checks. first %s</failure></testcase>\n",
                (long long)stray,
                (long long)validated.checks,
                first.c_str()
            );
        }
        if (!outside.empty()) {
            const std::string first = escape_xml(outside.front().c_str());
            std::fprintf(
                output,
                "    <testcase classname=\"frame_drive.h\" name=\"FS0 every "
                "check a frame scenario makes while it runs holds\" "
                "time=\"0.0\"><failure>%lld checks failed outside any case. "
                "first %s</failure></testcase>\n",
                (long long)outside.size(),
                first.c_str()
            );
        }
        for (const Case &test_case : cases) {
            const std::string file = escape_xml(test_case.file.c_str());
            const std::string name = escape_xml(test_case.name.c_str());
            std::fprintf(
                output,
                "    <testcase classname=\"%s\" name=\"%s\" "
                "time=\"%.9f\">",
                file.c_str(),
                name.c_str(),
                test_case.seconds
            );
            if (test_case.failure || test_case.error) {
                const char *tag = test_case.error ? "error" : "failure";
                const std::string detail = escape_xml(test_case.detail.c_str());
                std::fprintf(output, "<%s>%s</%s>", tag, detail.c_str(), tag);
            }
            std::fprintf(output, "</testcase>\n");
        }
        std::fprintf(output, "  </testsuite>\n</testsuites>\n");
        std::fclose(output);
    }

    void test_case_start(const doctest::TestCaseData &data) override {
        cases.push_back({data.m_file.c_str(), data.m_name});
        subcases.clear();
        store_faults_opened = netw::store::validation_audit::ledger().faults;
    }

    void test_case_reenter(const doctest::TestCaseData &) override {
        subcases.clear();
    }

    void test_case_end(const doctest::CurrentTestCaseStats &stats) override {
        Case *test_case = current();
        if (test_case == nullptr) {
            return;
        }
        test_case->seconds = stats.seconds;
        if (!stats.testCaseSuccess && !test_case->error) {
            test_case->failure = true;
        }
        const netw::store::validation_audit::Ledger &validated
            = netw::store::validation_audit::ledger();
        const int64_t missed = validated.faults - store_faults_opened;
        if (missed > 0) {
            store_faults_in_cases += missed;
            test_case->failure = test_case->failure || !test_case->error;
            append_detail(
                "the store missed " + std::to_string(missed)
                + " changes during this case. first of the run "
                + validated.first
            );
        }
    }

    void test_case_exception(
        const doctest::TestCaseException &exception
    ) override {
        Case *test_case = current();
        if (test_case == nullptr) {
            return;
        }
        test_case->error = true;
        append_detail(
            subcase_prefix() + exception.error_string.c_str() + active_context()
        );
    }

    void subcase_start(const doctest::SubcaseSignature &signature) override {
        subcases.push_back(signature.m_name.c_str());
    }

    void subcase_end() override {
        if (!subcases.empty()) {
            subcases.pop_back();
        }
    }

    void log_assert(const doctest::AssertData &assertion) override {
        if (!assertion.m_failed) {
            return;
        }
        Case *test_case = current();
        if (test_case == nullptr) {
            return;
        }
        test_case->failure = true;
        std::string text = subcase_prefix();
        text += assertion.m_expr;
        text += ": ";
        text += assertion.m_decomp.c_str();
        append_detail(text + active_context());
    }

    void log_message(const doctest::MessageData &message) override {
        if ((message.m_severity & doctest::assertType::is_warn) != 0) {
            return;
        }
        Case *test_case = current();
        if (test_case == nullptr) {
            return;
        }
        test_case->failure = true;
        append_detail(
            subcase_prefix() + message.m_string.c_str() + active_context()
        );
    }

    void test_case_skipped(const doctest::TestCaseData &) override {
    }
};

DOCTEST_REGISTER_REPORTER("netw-junit", 0, NetwJunitReporter);

} // namespace

int run_native_tests(
    const String &filter,
    const String &report_path,
    const String &cells_path
) {
    const String global_path
        = ProjectSettings::get_singleton()->globalize_path(report_path);
    const CharString path = global_path.utf8();
    std::filesystem::path file_path(path.get_data());
    std::filesystem::create_directories(file_path.parent_path());

    doctest::Context context;
    context.setOption("reporters", "netw-junit");
    context.setOption("out", path.get_data());
    context.setOption("no-version", true);
    if (!filter.is_empty()) {
        const CharString filter_text = filter.utf8();
        context.setOption("test-case", filter_text.get_data());
    }
    const int code = context.run();
    if (!cells_path.is_empty()) {
        const CharString cells = ProjectSettings::get_singleton()
                                     ->globalize_path(cells_path)
                                     .utf8();
        std::filesystem::create_directories(
            std::filesystem::path(cells.get_data()).parent_path()
        );
        netw_test::Cells::write(cells.get_data());
    }
    return code;
}

void NetwNativeTests::_bind_methods() {
    ClassDB::bind_method(
        D_METHOD("run", "filter", "report_path", "cells_path"),
        &NetwNativeTests::run,
        DEFVAL(String()),
        DEFVAL(String("res://reports/native/results.xml")),
        DEFVAL(String())
    );
    ClassDB::bind_method(
        D_METHOD("frame_advance"),
        &NetwNativeTests::frame_advance
    );
    ClassDB::bind_method(
        D_METHOD("instrumentation_probe", "value"),
        &NetwNativeTests::instrumentation_probe
    );
    ClassDB::bind_method(D_METHOD("wire_spec"), &NetwNativeTests::wire_spec);
    ClassDB::bind_static_method(
        "NetwNativeTests",
        D_METHOD("schema_model_clear"),
        &NetwNativeTests::schema_model_clear
    );
    ClassDB::bind_static_method(
        "NetwNativeTests",
        D_METHOD("tracker_book_clear"),
        &NetwNativeTests::tracker_book_clear
    );
    ClassDB::bind_static_method(
        "NetwNativeTests",
        D_METHOD("fault_book_clear"),
        &NetwNativeTests::fault_book_clear
    );
    ClassDB::bind_static_method(
        "NetwNativeTests",
        D_METHOD("prediction_stats", "handle"),
        &NetwNativeTests::prediction_stats
    );
    ClassDB::bind_static_method(
        "NetwNativeTests",
        D_METHOD("attribution_arm", "session"),
        &NetwNativeTests::attribution_arm
    );
    ClassDB::bind_static_method(
        "NetwNativeTests",
        D_METHOD("attribution_snapshot", "session"),
        &NetwNativeTests::attribution_snapshot
    );
    ClassDB::bind_static_method(
        "NetwNativeTests",
        D_METHOD("predict_tap_flush", "session"),
        &NetwNativeTests::predict_tap_flush
    );
    ClassDB::bind_static_method(
        "NetwNativeTests",
        D_METHOD("predict_tap_cost", "session"),
        &NetwNativeTests::predict_tap_cost
    );
    ClassDB::bind_static_method(
        "NetwNativeTests",
        D_METHOD("scene_enter", "session", "scene", "stem", "owns_its_world"),
        &NetwNativeTests::scene_enter
    );
    ClassDB::bind_static_method(
        "NetwNativeTests",
        D_METHOD("scene_container_meta"),
        &NetwNativeTests::scene_container_meta
    );
    ClassDB::bind_static_method(
        "NetwNativeTests",
        D_METHOD("display_clock_tick", "session", "delta", "tick"),
        &NetwNativeTests::display_clock_tick
    );
    ClassDB::bind_static_method(
        "NetwNativeTests",
        D_METHOD("display_pump", "session", "delta"),
        &NetwNativeTests::display_pump
    );
    ClassDB::bind_static_method(
        "NetwNativeTests",
        D_METHOD(
            "display_record",
            "session",
            "node",
            "target_property",
            "value",
            "tick",
            "spec",
            "authoring_tick"
        ),
        &NetwNativeTests::display_record
    );
    ClassDB::bind_static_method(
        "NetwNativeTests",
        D_METHOD("display_mark_role_dirty", "session", "entity"),
        &NetwNativeTests::display_mark_role_dirty
    );
    ClassDB::bind_static_method(
        "NetwNativeTests",
        D_METHOD("observe_node_entity_ref", "session", "node_ref"),
        &NetwNativeTests::observe_node_entity_ref
    );
}

Dictionary NetwNativeTests::run(
    const String &filter,
    const String &report_path,
    const String &cells_path
) {
    Dictionary result;
    result[StringName("filter")] = filter;
    result[StringName("report_path")] = report_path;
    result[StringName("cells_path")] = cells_path;
    open_run_faults();
    netw_test::census_ledger().read_by_runner = true;
    const std::chrono::steady_clock::time_point started
        = std::chrono::steady_clock::now();
    result[StringName("exit_code")]
        = run_native_tests(filter, report_path, cells_path);
    result[StringName("seconds")]
        = std::chrono::duration<double>(
              std::chrono::steady_clock::now() - started
        )
              .count();
    result[StringName("cells")] = netw_test::Cells::count();

    const RunSummary &summary = run_summary();
    result[StringName("cases")] = summary.cases;
    result[StringName("failed")] = summary.failed;
    Array failing;
    for (int at = 0; at < int(summary.failing.size()) && at < FAILING_SHOWN;
         ++at) {
        Dictionary row;
        row[StringName("name")]
            = String::utf8(summary.failing[at].name.c_str());
        row[StringName("detail")]
            = String::utf8(summary.failing[at].detail.c_str());
        failing.push_back(row);
    }
    result[StringName("failing")] = failing;

    const std::vector<netw_test::CensusRow> &held
        = netw_test::census_ledger().rows;
    result[StringName("census_cases")] = int64_t(held.size());
    result[StringName("census_reset_only")] = int64_t(
        std::count_if(
            held.begin(),
            held.end(),
            [](const netw_test::CensusRow &p_row) {
                return p_row.freed_by_reset > 0;
            }
        )
    );
    result[StringName("census_top")] = top_census_rows();
    result[StringName("census_path")] = write_census(report_path);

    const RunFaults faults = close_run_faults();
    int64_t family_count = 0;
    result[StringName("faults")] = faults.printed;
    result[StringName("faults_expected")] = faults.expected;
    result[StringName("faults_unexpected")] = int64_t(faults.unexpected.size());
    result[StringName("fault_families")]
        = top_fault_families(faults.unexpected, family_count);
    result[StringName("fault_family_count")] = family_count;
    return result;
}

bool NetwNativeTests::frame_advance() {
    static doctest::Context outside_a_case;
    outside_a_case.setAsDefaultForAssertsOutOfTestCases();
    outside_a_case.setAssertHandler(&note_check_outside_a_case);
    open_run_faults();
    netw_test::smooth_when_headless(true);
    return netw_test::frame_drive_advance();
}

void NetwNativeTests::schema_model_clear() {
    schema_model::clear();
}

void NetwNativeTests::tracker_book_clear() {
    netw::connect::TrackerBook::shared().clear();
}

void NetwNativeTests::fault_book_clear() {
    netw::fault::forget();
}

Dictionary NetwNativeTests::prediction_stats(Object *p_handle) {
    const NetwPrediction *handle = Object::cast_to<NetwPrediction>(p_handle);
    const predict::Decl *decl = handle != nullptr
        ? NetwMultiplayer::predict_find_declaration(handle->get_entity().ptr())
        : nullptr;
    return decl != nullptr ? decl->facts.to_dictionary() : Dictionary();
}

void NetwNativeTests::attribution_arm(NetwMultiplayer *p_session) {
    if (p_session != nullptr) {
        p_session->attribution_set_armed(true);
    }
}

Dictionary NetwNativeTests::attribution_snapshot(NetwMultiplayer *p_session) {
    return p_session != nullptr ? p_session->attribution_snapshot()
                                : Dictionary();
}

void NetwNativeTests::predict_tap_flush(NetwMultiplayer *p_session) {
    if (p_session != nullptr) {
        p_session->predict_flush_tap();
    }
}

Dictionary NetwNativeTests::predict_tap_cost(NetwMultiplayer *p_session) {
    return p_session != nullptr ? p_session->predict_get_tap_cost()
                                : Dictionary();
}

void NetwNativeTests::scene_enter(
    NetwMultiplayer *session,
    const RID &scene,
    const StringName &stem,
    bool owns_its_world
) {
    if (session != nullptr) {
        session->get_scene_core()->scene_enter(scene, stem, owns_its_world);
    }
}

bool NetwNativeTests::rpc_sender_admits(
    NetwMultiplayer *p_session,
    Node *p_node,
    const StringName &p_method,
    int64_t p_sender
) {
    if (p_session == nullptr || p_node == nullptr) {
        return false;
    }
    const Ref<Script> script = p_node->get_script();
    return p_session->rpc_sender_allowed(
        netw::script::model::get_rpc_options(script, p_method),
        script,
        p_method,
        NetwEntity::of(p_node),
        p_node,
        p_sender
    );
}

StringName NetwNativeTests::scene_container_meta() {
    return NetwMultiplayer::scene_container_meta();
}

display::Runtime *NetwNativeTests::display_runtime_of(
    NetwMultiplayer *session,
    const RID &entity
) {
    if (session == nullptr) {
        return nullptr;
    }
    return session->get_display_book()->runtime_of(entity);
}

display::Runtime *NetwNativeTests::display_runtime_at(
    NetwMultiplayer *session,
    int64_t route
) {
    if (session == nullptr) {
        return nullptr;
    }
    return session->get_display_book()->runtime_at(route);
}

LocalVector<display::Runtime *> NetwNativeTests::display_runtimes(
    NetwMultiplayer *session
) {
    if (session == nullptr) {
        return LocalVector<display::Runtime *>();
    }
    return session->get_display_book()->runtimes();
}

int64_t NetwNativeTests::display_route_of(
    NetwMultiplayer *session,
    const RID &entity
) {
    if (session == nullptr) {
        return 0;
    }
    return session->get_display_book()->route_of(entity);
}

void NetwNativeTests::display_clock_tick(
    NetwMultiplayer *session,
    const double p_delta,
    const int64_t p_tick
) {
    if (session != nullptr) {
        session->display_on_clock_tick(p_delta, p_tick);
    }
}

Error NetwNativeTests::display_pump(NetwMultiplayer *session, double delta) {
    return session != nullptr ? session->display_pump(delta) : OK;
}

void NetwNativeTests::display_pump_runtime(
    NetwMultiplayer *session,
    display::Runtime *runtime,
    const display::Timing &timing
) {
    if (session != nullptr) {
        session->display_pump_runtime(
            runtime,
            timing,
            session->get_display_book()->get_stats()
        );
    }
}

void NetwNativeTests::display_record(
    NetwMultiplayer *session,
    Node *node,
    const StringName &target_property,
    const Variant &value,
    int64_t tick,
    const Ref<NetwInterpolate> &spec,
    bool authoring_tick
) {
    if (session != nullptr) {
        session->display_record(
            node,
            target_property,
            value,
            tick,
            spec,
            authoring_tick
        );
    }
}

bool NetwNativeTests::display_wants_runtime(
    NetwMultiplayer *session,
    Node *owner
) {
    return session != nullptr && session->display_wants_runtime(owner);
}

double NetwNativeTests::display_chase_smooth_time(
    NetwMultiplayer *session,
    display::Runtime *runtime,
    const display::Timing &timing
) {
    return session != nullptr
        ? session->display_chase_smooth_time(runtime, timing)
        : 0.0;
}

void NetwNativeTests::display_absorb_recovery(
    NetwMultiplayer *session,
    display::Runtime *runtime,
    const Dictionary &deltas,
    bool teleported
) {
    if (session != nullptr) {
        session->display_absorb_recovery(runtime, deltas, teleported);
    }
}

void NetwNativeTests::display_mark_role_dirty(
    NetwMultiplayer *session,
    const RID &entity
) {
    if (session != nullptr) {
        session->display_mark_role_dirty(entity);
    }
}

void NetwNativeTests::observe_node_entity_ref(
    NetwMultiplayer *session,
    const Variant &p_node_ref
) {
    if (session != nullptr) {
        session->observe_node_entity_ref(p_node_ref);
    }
}

Dictionary NetwNativeTests::wire_spec() const {
    return wire::spec_document();
}

void NetwNativeTests::instrumentation_probe(int64_t value) const {
    NETW_FRAME(profile::names::TICK_FRAME);
    NETW_ZONE_NC("netwkd instrumentation probe", colors::TICK);
    NETW_ZONE_VALUE(value);
#if defined(NETW_PROFILING)
    const CharString text = String::num_int64(value).utf8();
    NETW_ZONE_TEXT(text.get_data(), text.length());
#endif
    NETW_TRACE(netw::sys::TEST, "netwkd native trace probe");
    probe_default_zone(value);
    probe_named_zone();
    probe_colored_zone();
    compile_ambient_profile_macros();
}

} // namespace netw
