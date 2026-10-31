#include "support/netw_test.h"

#include "support/scenario_text.h"

namespace TestScenarioText {

using namespace netw_test;

const char *const SAMPLE = "label s2_late_parent\n"
                           "clients 2\n"
                           "ticks 30\n"
                           "seed 7\n"
                           "laws S4 S9 S19 S28 S29\n"
                           "1 spawn car scene:car parent:World name:Car\n"
                           "1 spawn wheel scene:wheel parent:car name:Wheel\n"
                           "1 host_delay c1 car 1\n"
                           "3 drop s>c2 reliable 0\n";

bool step_is(
    const Scenario::Step &p_step,
    int p_tick,
    const char *p_verb,
    const char *p_subject,
    const char *p_value
) {
    return p_step.tick == p_tick
        && p_step.verb == godot::StringName(p_verb)
        && p_step.subject == godot::StringName(p_subject)
        && godot::String(p_step.value) == godot::String(p_value);
}

TEST_CASE(
    "[Networked][Scenario][Hosted] a scenario file reads back its header and "
    "every step, with the rest of a step's line as its value"
) {
    const ScenarioText text = read_scenario(SAMPLE);
    NETW_REQUIRE_EQ(text.refused_line, 0);
    const Scenario &scenario = text.scenario;
    const bool labelled = scenario.label == godot::String("s2_late_parent");
    CHECK(labelled);
    NETW_CHECK_EQ(scenario.clients, 2);
    NETW_CHECK_EQ(scenario.run_ticks, 30);
    NETW_CHECK_EQ(scenario.seed, 7);
    NETW_REQUIRE_EQ(scenario.laws.size(), 5);
    const bool laws_read = scenario.laws[0] == godot::StringName("S4")
        && scenario.laws[4] == godot::StringName("S29");
    CHECK(laws_read);
    NETW_REQUIRE_EQ(scenario.steps.size(), 4);
    CHECK(step_is(
        scenario.steps[0],
        1,
        "spawn",
        "car",
        "scene:car parent:World name:Car"
    ));
    CHECK(step_is(
        scenario.steps[1],
        1,
        "spawn",
        "wheel",
        "scene:wheel parent:car name:Wheel"
    ));
    CHECK(step_is(scenario.steps[2], 1, "host_delay", "c1", "car 1"));
    CHECK(step_is(scenario.steps[3], 3, "drop", "s>c2", "reliable 0"));
}

TEST_CASE(
    "[Networked][Scenario][Hosted] a cut scenario file is refused at the line "
    "it was cut on"
) {
    struct Row {
        const char *cut_at;
        int kept_chars;
        int refused_line;
    };
    const Row rows[] = {
        {"seed 7", 0, 4},
        {"1 spawn wheel", 7, 7},
    };
    const godot::String sample(SAMPLE);
    for (const Row &row : rows) {
        NETW_FORMAT_TEXT(cut_at, row.cut_at);
        CAPTURE(cut_at);
        const godot::String text
            = sample.substr(0, sample.find(row.cut_at) + row.kept_chars);
        const ScenarioText read = read_scenario(text);
        NETW_CHECK_EQ(read.refused_line, row.refused_line);
        CHECK_FALSE(read.refusal.is_empty());
    }
}

} // namespace TestScenarioText
