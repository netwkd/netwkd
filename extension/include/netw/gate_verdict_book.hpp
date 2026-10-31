#pragma once

#include <cstdint>

#include "godot/variant.hpp"

namespace netw {

class GateVerdictBook {
public:
    static constexpr int ROWS = 6;

    enum Report {
        QUIET,
        WARN,
        DEFECT,
    };

private:
    int64_t totals[ROWS] = {0};

public:
    static int row_of(int64_t verdict);

    static bool counts(int64_t verdict);

    static Report report_of(int64_t verdict);

    bool count(int64_t verdict);
    int64_t total(int64_t verdict) const;

    void clear();
};

} // namespace netw
