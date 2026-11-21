#include "netw/sealed.hpp"

#include "netw/table/quant_spec.hpp"

#include <algorithm>
#include <array>
#include <cmath>

using namespace godot;

namespace netw::table {

namespace {

constexpr double TURN_COMPONENT_LIMIT = 0.7071067811865476;

int64_t turn_component_code(int p_bits, double p_value) {
    const int64_t top = (int64_t(1) << p_bits) - 1;
    const double span = TURN_COMPONENT_LIMIT * 2.0;
    const double unit
        = std::clamp((p_value + TURN_COMPONENT_LIMIT) / span, 0.0, 1.0);
    return int64_t(std::round(unit * double(top)));
}

double turn_component_value(int p_bits, int64_t p_code) {
    const int64_t top = (int64_t(1) << p_bits) - 1;
    const double unit = double(p_code) / double(top);
    return unit * TURN_COMPONENT_LIMIT * 2.0 - TURN_COMPONENT_LIMIT;
}

} // namespace

int64_t scalar_top(int p_bits) {
    return std::max<int64_t>(2, (int64_t(1) << p_bits) - 1) - 1;
}

int64_t scalar_code(int p_bits, double p_low, double p_high, double p_value) {
    const double span = p_high - p_low;
    const int64_t top = scalar_top(p_bits);
    const double unit
        = span == 0.0 ? 0.0 : std::clamp((p_value - p_low) / span, 0.0, 1.0);
    return std::clamp<int64_t>(int64_t(std::round(unit * double(top))), 0, top);
}

double scalar_value(int p_bits, double p_low, double p_high, int64_t p_code) {
    const int64_t top = scalar_top(p_bits);
    const int64_t held = std::clamp<int64_t>(p_code, 0, top);
    return p_low + double(held) / double(top) * (p_high - p_low);
}

int64_t angle_code(int p_bits, double p_value) {
    const uint64_t level_count = uint64_t(1) << p_bits;
    double wrapped = std::fmod(p_value, ANGLE_TAU);
    if (wrapped < 0.0) {
        wrapped += ANGLE_TAU;
    }
    return int64_t(
        uint64_t(std::llround(wrapped / ANGLE_TAU * double(level_count)))
        % level_count
    );
}

double angle_value(int p_bits, bool p_centered, int64_t p_code) {
    const uint64_t level_count = uint64_t(1) << p_bits;
    double angle = double(p_code) / double(level_count) * ANGLE_TAU;
    if (p_centered && angle > ANGLE_PI) {
        angle -= ANGLE_TAU;
    }
    return angle;
}

int turn_width(int p_bits) {
    return 2 + p_bits * 3;
}

int64_t turn_code(int p_bits, const Quaternion &p_value) {
    Quaternion quaternion = p_value.normalized();
    std::array<double, 4> components = {
        quaternion.x,
        quaternion.y,
        quaternion.z,
        quaternion.w,
    };
    int largest = 0;
    for (int index = 1; index < 4; ++index) {
        if (std::abs(components[index]) > std::abs(components[largest])) {
            largest = index;
        }
    }
    if (components[largest] < 0.0) {
        for (double &component : components) {
            component = -component;
        }
    }
    int64_t packed = int64_t(largest);
    int at = 2;
    for (int index = 0; index < 4; ++index) {
        if (index == largest) {
            continue;
        }
        packed |= turn_component_code(p_bits, components[index]) << at;
        at += p_bits;
    }
    return packed;
}

Quaternion turn_value(int p_bits, int64_t p_packed) {
    const int largest = int(p_packed & 0x3);
    const int64_t mask = (int64_t(1) << p_bits) - 1;
    std::array<double, 4> components = {0.0, 0.0, 0.0, 0.0};
    double length_squared = 0.0;
    int at = 2;
    for (int index = 0; index < 4; ++index) {
        if (index == largest) {
            continue;
        }
        components[index]
            = turn_component_value(p_bits, (p_packed >> at) & mask);
        length_squared += components[index] * components[index];
        at += p_bits;
    }
    components[largest] = std::sqrt(std::max(0.0, 1.0 - length_squared));
    return Quaternion(
               components[0],
               components[1],
               components[2],
               components[3]
    )
        .normalized();
}

} // namespace netw::table
