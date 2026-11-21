#pragma once

#include "godot/callable.hpp"
#include "godot/variant.hpp"
#include <cstdint>

namespace netw {

struct GameCall {
    const char *what;
    const char *signature;
    int64_t argument_count;
    const char *member = "";
};

constexpr int64_t game_call_argument_count(const char *p_signature) {
    const char *at = p_signature;
    while (*at != '\0' && *at != '(') {
        ++at;
    }
    if (*at == '\0') {
        return 0;
    }
    ++at;
    int64_t separators = 0;
    int64_t depth = 0;
    bool has_parameter = false;
    for (; *at != '\0' && *at != ')'; ++at) {
        if (at[0] == '.' && at[1] == '.' && at[2] == '.') {
            return -1;
        }
        if (*at == '[') {
            ++depth;
        } else if (*at == ']') {
            --depth;
        } else if (*at == ',' && depth == 0) {
            ++separators;
        } else if (*at != ' ') {
            has_parameter = true;
        }
    }
    return has_parameter ? separators + 1 : 0;
}

namespace game_calls {

#define NETW_GAME_CALL(m_id, m_member, m_what, m_signature) \
    inline constexpr GameCall m_id \
        = {m_what, \
           m_signature, \
           game_call_argument_count(m_signature), \
           m_member};
#include "netw/game_calls.def"
#undef NETW_GAME_CALL

static_assert(PROMISE_SETTLED.argument_count == 0);
static_assert(PROMISE_CATCH.argument_count == 2);
static_assert(SPAWN_FUNCTION.argument_count == -1);

} // namespace game_calls

bool game_call_accepts(const godot::Callable &p_fn, int64_t p_count);

bool game_call_declared(const godot::Callable &p_fn, const GameCall &p_site);

godot::Variant game_call(
    const godot::Callable &p_fn,
    const godot::Array &p_args,
    const GameCall &p_site,
    bool *r_called = nullptr
);

} // namespace netw
