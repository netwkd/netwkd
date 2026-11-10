#include "support/netw_test.h"
#include "support/predict_rows.h"

#include "netw/predict/engine.hpp"
#include "netw/predict/journal.hpp"
#include "netw/prediction_core.hpp"

namespace TestNetwPredictMeterLaws {

using namespace godot;
using namespace netw;
using netw::predict::Domain;
using netw::predict::ROW_ACKED;
using netw::predict::ROW_MATCHED;

LocalVector<netw::predict::FieldDecl> declaration(double p_epsilon) {
    LocalVector<netw::predict::FieldDecl> out;
    netw::predict::FieldDecl value = netw::field_decl(
        StringName("value"),
        0,
        StringName(),
        0.0,
        false,
        false,
        p_epsilon
    );
    value.type = int(Variant::FLOAT);
    out.push_back(value);
    return out;
}

int64_t seated(NetwPredictionEngine &r_pool, double p_epsilon) {
    const int64_t slot = r_pool.open(declaration(p_epsilon));
    REQUIRE(slot >= 0);
    return slot;
}

predict::StateRow valued(
    const NetwPredictionEngine &p_pool,
    int64_t p_slot,
    double p_value
) {
    Dictionary values;
    values[StringName("value")] = p_value;
    return netw_test::state_row(&p_pool, p_slot, values);
}

TEST_CASE(
    "[Networked][Predict][Hosted][Meter] M1 a correction is never worth "
    "zero, so a budget that spends on one can always tell it happened"
) {
    NetwPredictionEngine pool;
    const int64_t slot = seated(pool, 0.5);

    const predict::Judged judged = pool.judge_state(
        slot,
        4,
        int64_t(Domain::IN_DOMAIN),
        int64_t(ROW_ACKED),
        valued(pool, slot, 1.0),
        valued(pool, slot, 1.0)
    );
    CHECK(judged.corrected);
    NETW_CHECK_GE(judged.meter, 1);
}

TEST_CASE(
    "[Networked][Predict][Hosted][Meter] M2 a verdict that corrected nothing "
    "reports a meter of zero however far apart the two states measured"
) {
    NetwPredictionEngine pool;
    const int64_t slot = seated(pool, 0.5);

    const predict::Judged judged = pool.judge_state(
        slot,
        4,
        int64_t(Domain::IN_DOMAIN),
        int64_t(ROW_ACKED | ROW_MATCHED),
        valued(pool, slot, 1.0),
        valued(pool, slot, 13.0)
    );
    CHECK_FALSE(judged.corrected);
    NETW_CHECK_EQ(judged.meter, 0);
    NETW_CHECK_CLOSE(judged.divergence, 12.0, 0.0001);
}

} // namespace TestNetwPredictMeterLaws
