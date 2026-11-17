#include "support/netw_test.h"

#include "support/netw_cells.h"
#include "support/scenario_run.h"

#if defined(NETW_TIER_HOSTED)

namespace TestNetwDeclaredSceneLaws {

using namespace netw_test;

EntityDecl crate() {
    return EntityDecl().named("Crate").on_route(91).placed_at(godot::Vector2());
}

Scenario nested_scene() {
    Scenario scenario;
    scenario.label = "nested-scene";
    scenario.world.scene("Arena").scene("Vault").entity(crate(), "Vault");
    scenario.place(1, "Vault", "Arena");
    return scenario.until(2);
}

LawVerdict law_a_nested_scene_owns_its_own(const ScenarioRun &p_run) {
    const Membership arena = p_run.membership("Arena");
    const Membership vault = p_run.membership("Vault");
    if (!arena.taken() || !vault.taken()) {
        return law_broken("a declared scene answered nothing");
    }
    if (!vault.encloses("Crate")) {
        return law_broken("the inner scene does not enclose its own member");
    }
    if (!arena.encloses("Vault")) {
        return law_broken("the outer scene does not enclose the inner one");
    }
    if (arena.encloses("Crate")) {
        return law_broken(
            "the outer scene reaches past the inner one to its %d member(s)",
            vault.members()
        );
    }
    if (arena.members() != 1) {
        return law_broken(
            "the outer scene encloses %d entities where the inner one is all "
            "it owns",
            arena.members()
        );
    }
    return law_held();
}

const LawRow L_NEST = {
    "L-NEST",
    "a nested scene owns its own descendants, so the scene above it encloses "
    "the nested scene and nothing under it",
    law_a_nested_scene_owns_its_own,
};

TEST_CASE("[Netw][Scene][Declared][Law] a nested scene owns its own members") {
    const Scenario scenario = nested_scene();
    LoopbackRig rig(scenario.clients);
    const ScenarioRun run = ScenarioRun::scenes(rig, scenario);
    REQUIRE(run.regime_reached());
    NETW_CELL(L_NEST, scenario);
    NETW_LAW_HOLDS(L_NEST, run);
}

TEST_CASE(
    "[Netw][Scene][Declared][Law] a walk that ignores nesting claims the "
    "inner members"
) {
    const Scenario scenario = nested_scene();
    LoopbackRig rig(scenario.clients);
    const ScenarioRun run
        = ScenarioRun::scenes(rig, scenario, PLANT_TRANSPARENT_NESTING);
    REQUIRE(run.regime_reached());
    NETW_CELL(L_NEST, scenario);
    NETW_LAW_BREAKS(L_NEST, run);
}

} // namespace TestNetwDeclaredSceneLaws

#endif
