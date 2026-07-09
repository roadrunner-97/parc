#include <cstring>
#include <set>
#include <string>

#include <gtest/gtest.h>

#include "parc/err.h"

TEST(Err, OkIsZero) {
    EXPECT_EQ(PARC_OK, 0);
}

TEST(Err, AllCodesHaveDistinctNonEmptyStrings) {
    std::set<std::string> seen;
    for (int e = 0; e < PARC_ERR_COUNT_; ++e) {
        const char *s = parc_err_str(static_cast<parc_err>(e));
        ASSERT_NE(s, nullptr) << "code " << e;
        EXPECT_GT(std::strlen(s), 0u) << "code " << e;
        EXPECT_TRUE(seen.insert(s).second) << "duplicate string for code " << e;
    }
}

TEST(Err, UnknownValues) {
    EXPECT_STREQ(parc_err_str(static_cast<parc_err>(PARC_ERR_COUNT_)),
                 "unknown error");
    EXPECT_STREQ(parc_err_str(static_cast<parc_err>(-1)), "unknown error");
    EXPECT_STREQ(parc_err_str(static_cast<parc_err>(9999)), "unknown error");
}
