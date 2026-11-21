#pragma once

#include <cmath>

#if defined(NETW_MODULE)
#include "core/math/math_funcs.h"

namespace godot {
namespace Math = ::Math;
} // namespace godot
#elif defined(NETW_GDEXTENSION)
#include <godot_cpp/core/math.hpp>
#else
#error "Define NETW_MODULE or NETW_GDEXTENSION."
#endif

namespace netw::gd {

inline constexpr double TAU = 6.2831853071795864769252867666;

inline double angle_difference(double p_from, double p_to) {
    const double difference = std::fmod(p_to - p_from, TAU);
    return std::fmod(2.0 * difference, TAU) - difference;
}

} // namespace netw::gd
