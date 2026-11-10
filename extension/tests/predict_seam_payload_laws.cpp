#include "support/link.h"
#include "support/netw_test.h"

#include <cstdint>

#include "godot/variant.hpp"
#include "netw/api/prediction.hpp"
#include "netw/script/model.hpp"

#if defined(NETW_TIER_HOSTED)

#include "support/carrier.h"
#include "support/loopback_rig.h"
#include "support/minted_script.h"

using namespace godot;

namespace TestNetwPredictSeamPayload {

using netw_test::EntityDecl;
using netw_test::LoopbackRig;
using netw_test::WorldDecl;

constexpr const char *LISTENING_RULE = R"(extends RefCounted

var seen := []

func rule(value: Variant, ctx: NetwPredictCarryContext) -> Variant:
	seen.append([value, ctx.state.duplicate(), ctx.input.duplicate(),
		ctx.delta, ctx.label])
	var motion: Vector2 = ctx.input.get(&"motion", Vector2())
	return Vector2(value) + motion * 60.0 * ctx.delta
)";

struct Heard {
    int64_t calls = 0;
    int64_t digest = 0;
    String first;
};

Heard heard_of(const Array &p_calls) {
    Heard out;
    out.calls = p_calls.size();
    const String text = UtilityFunctions::var_to_str(p_calls);
    out.digest = int64_t(text.hash());
    if (!p_calls.is_empty()) {
        out.first = UtilityFunctions::var_to_str(p_calls[0]);
    }
    return out;
}

Heard carry_run() {
    LoopbackRig rig(1);
    rig.declare_world(
        WorldDecl().clocked(30, 3).lag_compensated().player(
            EntityDecl()
                .named("P")
                .on_schema("CarriedPose")
                .synced("position")
                .placed_at(Vector2())
                .predicted(0.01)
                .scheduled(netw::Schedule::FRAME)
                .carried(1.0, 200.0),
            0
        )
    );
    rig.conditions(0, netw_test::polls(4));
    rig.conditions(-1, netw_test::polls(4));
    const Ref<Script> rule_script = netw_test::minted_script(LISTENING_RULE);
    REQUIRE(rule_script.is_valid());
    const Variant rule = rule_script->call("new");
    Object *listener = rule;
    REQUIRE(listener != nullptr);
    rig.prediction_pool(0)->set_carry(
        rig.prediction_slot(StringName("P"), 0),
        StringName("position"),
        Callable(listener, StringName("rule"))
    );
    rig.node_of(rig.entity_of("P", 0), 0)->set("motion", Vector2(1.0, 0.0));
    for (int at = 0; at < 70; ++at) {
        if (at == 31) {
            Node2D *authority
                = Object::cast_to<Node2D>(rig.node_of(rig.entity_of("P")));
            authority->set_position(
                authority->get_position() + Vector2(60.0, -40.0)
            );
        }
        rig.step_frame(1);
    }
    return heard_of(listener->get("seen"));
}

void check_heard(
    const Heard &p_heard,
    int64_t p_calls,
    int64_t p_digest,
    const char *p_first
) {
    NETW_CHECK_EQ(p_heard.calls, p_calls);
    NETW_CHECK_EQ(p_heard.digest, p_digest);
    const bool same_first = p_heard.first == String(p_first);
    if (!same_first) {
        UtilityFunctions::print(p_heard.first);
    }
    CHECK(same_first);
}

constexpr const char *CARRY_FIRST = R"([Vector2(58, 0), {
&"position": Vector2(58, 0)
}, {
&"bombing": false,
&"motion": Vector2(1, 0)
}, 0.03333333333333333, 29])";

TEST_CASE(
    "[Networked][Predict] HS3 a carry rule hears the same state and input on "
    "every carried recovery"
) {
    check_heard(carry_run(), 15, 3245973770LL, CARRY_FIRST);
}

} // namespace TestNetwPredictSeamPayload

#endif
