#pragma once

#include "scenario.h"

#include "godot/templates.hpp"
#include "godot/variant.hpp"

namespace netw_test {

struct ScenarioText {
    Scenario scenario;
    int refused_line = 0;
    godot::String refusal;
};

namespace scenario_text {

constexpr int HEADER_LINES = 5;
constexpr int COUNT_DIGITS = 18;

inline int token_end(const godot::String &p_line, int p_from) {
    int at = p_from;
    while (at < p_line.length() && p_line[at] != ' ') {
        ++at;
    }
    return at;
}

inline godot::String after_token(const godot::String &p_line, int p_end) {
    return p_end < p_line.length() ? p_line.substr(p_end + 1)
                                   : godot::String();
}

inline bool is_count(const godot::String &p_token) {
    if (p_token.is_empty() || p_token.length() > COUNT_DIGITS) {
        return false;
    }
    for (int at = 0; at < p_token.length(); ++at) {
        if (p_token[at] < '0' || p_token[at] > '9') {
            return false;
        }
    }
    return true;
}

inline godot::String read_header(
    Scenario &r_scenario,
    int p_number,
    const godot::String &p_key,
    const godot::String &p_rest
) {
    static const char *const KEYS[HEADER_LINES]
        = {"label", "clients", "ticks", "seed", "laws"};
    static const char *const SHAPES[HEADER_LINES] = {
        "This line must read 'label <name>'.",
        "This line must read 'clients <count>'.",
        "This line must read 'ticks <count>' with a count above zero.",
        "This line must read 'seed <count>'.",
        "This line must read 'laws' followed by law names, one space apart.",
    };
    const godot::String shape(SHAPES[p_number - 1]);
    if (p_key != KEYS[p_number - 1]) {
        return shape;
    }
    if (p_number == 1) {
        if (p_rest.is_empty() || token_end(p_rest, 0) != p_rest.length()) {
            return shape;
        }
        r_scenario.label = p_rest;
    } else if (p_number == HEADER_LINES) {
        int from = 0;
        while (from < p_rest.length()) {
            const int end = token_end(p_rest, from);
            if (end == from || end == p_rest.length() - 1) {
                return shape;
            }
            r_scenario.laws.push_back(
                godot::StringName(p_rest.substr(from, end - from))
            );
            from = end + 1;
        }
    } else {
        if (!is_count(p_rest)) {
            return shape;
        }
        const int64_t count = p_rest.to_int();
        if (p_number == 2) {
            r_scenario.clients = int(count);
        } else if (p_number == 3) {
            if (count == 0) {
                return shape;
            }
            r_scenario.run_ticks = int(count);
        } else {
            r_scenario.seed = count;
        }
    }
    return godot::String();
}

inline godot::String read_step(
    Scenario &r_scenario,
    const godot::String &p_tick,
    const godot::String &p_rest
) {
    if (!is_count(p_tick) || p_tick.to_int() < 1
        || p_tick.to_int() > r_scenario.run_ticks) {
        return "A step must start with a tick from 1 to the ticks line.";
    }
    const int verb_end = token_end(p_rest, 0);
    const godot::String subject_line = after_token(p_rest, verb_end);
    const int subject_end = token_end(subject_line, 0);
    if (verb_end == 0 || subject_end == 0) {
        return "A step needs a tick, a verb and a subject, one space apart.";
    }
    Scenario::Step step;
    step.tick = int(p_tick.to_int());
    step.verb = godot::StringName(p_rest.substr(0, verb_end));
    step.subject = godot::StringName(subject_line.substr(0, subject_end));
    step.value = after_token(subject_line, subject_end);
    r_scenario.steps.push_back(step);
    return godot::String();
}

inline godot::String read_line(
    Scenario &r_scenario,
    int p_number,
    const godot::String &p_line
) {
    for (int at = 0; at < p_line.length(); ++at) {
        if (p_line[at] < 0x20 || p_line[at] > 0x7e) {
            return "A scenario line holds only printable ASCII.";
        }
    }
    const int key_end = token_end(p_line, 0);
    const godot::String key = p_line.substr(0, key_end);
    const godot::String rest = after_token(p_line, key_end);
    if (p_number <= HEADER_LINES) {
        return read_header(r_scenario, p_number, key, rest);
    }
    return read_step(r_scenario, key, rest);
}

} // namespace scenario_text

inline ScenarioText read_scenario(const godot::String &p_text) {
    ScenarioText out;
    int number = 0;
    int from = 0;
    while (from < p_text.length() || number < scenario_text::HEADER_LINES) {
        ++number;
        int end = from < p_text.length() ? p_text.find("\n", from) : -1;
        if (end < 0) {
            end = p_text.length();
        }
        const godot::String line = from < p_text.length()
            ? p_text.substr(from, end - from)
            : godot::String();
        from = end + 1;
        const godot::String refusal
            = scenario_text::read_line(out.scenario, number, line);
        if (!refusal.is_empty()) {
            out.refused_line = number;
            out.refusal = refusal;
            return out;
        }
    }
    return out;
}

} // namespace netw_test
