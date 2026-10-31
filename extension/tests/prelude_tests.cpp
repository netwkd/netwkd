#include "support/netw_test.h"

#include "netw/log.hpp"

namespace TestNetwPrelude {

TEST_CASE("[Networked][Prelude][Hosted] the tier gate resolves to one tier") {
#if defined(NETW_TIER_HOSTED)
    const bool hosted = true;
#else
    const bool hosted = false;
#endif
#if defined(NETW_TIER_MODULE)
    const bool module_tier = true;
#else
    const bool module_tier = false;
#endif
    CHECK(hosted != module_tier);
}

TEST_FORCE_LINK(netw_prelude);

TEST_CASE(
    "[Networked][Prelude][Hosted] the module-only spellings are callable"
) {
    ERR_PRINT_OFF;
    ERR_PRINT_ON;
    CHECK(true);
}

TEST_CASE("[Networked][Prelude][Hosted] the integer macro compares exactly") {
    NETW_CHECK_EQ(2 + 2, 4);
    NETW_CHECK_EQ(-1, -1);
}

TEST_CASE(
    "[Networked][Prelude][Hosted] the float macro compares to tolerance"
) {
    NETW_CHECK_CLOSE(0.1 + 0.2, 0.3, 1e-9);
    NETW_CHECK_CLOSE(-2.5, -2.5, 0.0);
}

TEST_CASE("[Networked][Prelude][Hosted] log format substitutes arguments") {
    CHECK(netw::log::format("peer=%d", 7) == "peer=7");
}

} // namespace TestNetwPrelude
