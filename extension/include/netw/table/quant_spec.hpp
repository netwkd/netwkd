#pragma once

#include <cstdint>

#include "godot/variant.hpp"

namespace netw::table {

enum class Quant : uint8_t { RAW, SCALAR, ANGLE, TURN, CUSTOM };

constexpr double ANGLE_PI = 3.14159265358979323846;
constexpr double ANGLE_TAU = ANGLE_PI * 2.0;

struct QuantSpec {
    Quant kind = Quant::RAW;
    int bits = 0;
    double low = 0.0;
    double high = 0.0;
    int width = 0;
    int parts = 0;
    godot::StringName label;
    uint64_t object = 0;

    bool quantized() const {
        return kind != Quant::RAW;
    }
};

int64_t scalar_top(int p_bits);
int64_t scalar_code(int p_bits, double p_low, double p_high, double p_value);
double scalar_value(int p_bits, double p_low, double p_high, int64_t p_code);

int64_t angle_code(int p_bits, double p_value);
double angle_value(int p_bits, bool p_centered, int64_t p_code);

int turn_width(int p_bits);
int64_t turn_code(int p_bits, const godot::Quaternion &p_value);
godot::Quaternion turn_value(int p_bits, int64_t p_packed);

} // namespace netw::table
