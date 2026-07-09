#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "util/buf.h"

TEST(Buf, InitAndZeroStateAreEmpty) {
    parc_buf a;
    parc_buf_init(&a);
    EXPECT_EQ(a.data, nullptr);
    EXPECT_EQ(a.len, 0u);
    EXPECT_EQ(a.cap, 0u);

    parc_buf b;
    std::memset(&b, 0, sizeof b);  // zero state must be valid
    EXPECT_EQ(parc_buf_append(&b, "x", 1), PARC_OK);
    EXPECT_EQ(b.len, 1u);
    parc_buf_free(&b);
}

TEST(Buf, AppendAccumulatesContents) {
    parc_buf b;
    parc_buf_init(&b);
    std::vector<uint8_t> want;
    for (int i = 0; i < 1000; ++i) {
        uint8_t chunk[7];
        for (int j = 0; j < 7; ++j)
            chunk[j] = static_cast<uint8_t>((i * 7 + j) & 0xFF);
        ASSERT_EQ(parc_buf_append(&b, chunk, sizeof chunk), PARC_OK);
        want.insert(want.end(), chunk, chunk + sizeof chunk);
    }
    ASSERT_EQ(b.len, want.size());
    EXPECT_EQ(std::memcmp(b.data, want.data(), want.size()), 0);
    parc_buf_free(&b);
}

TEST(Buf, AppendEmptyIsNoOp) {
    parc_buf b;
    parc_buf_init(&b);
    EXPECT_EQ(parc_buf_append(&b, nullptr, 0), PARC_OK);
    EXPECT_EQ(b.len, 0u);
    parc_buf_free(&b);
}

TEST(Buf, ReservePreservesContentsAndLen) {
    parc_buf b;
    parc_buf_init(&b);
    ASSERT_EQ(parc_buf_append(&b, "hello", 5), PARC_OK);
    ASSERT_EQ(parc_buf_reserve(&b, 1 << 20), PARC_OK);
    EXPECT_GE(b.cap, size_t{1} << 20);
    EXPECT_EQ(b.len, 5u);
    EXPECT_EQ(std::memcmp(b.data, "hello", 5), 0);
    // Reserving less than current cap must not shrink.
    size_t cap = b.cap;
    ASSERT_EQ(parc_buf_reserve(&b, 1), PARC_OK);
    EXPECT_EQ(b.cap, cap);
    parc_buf_free(&b);
}

TEST(Buf, GrowthIsGeometric) {
    parc_buf b;
    parc_buf_init(&b);
    size_t reallocs = 0;
    size_t last_cap = 0;
    for (int i = 0; i < 100000; ++i) {
        ASSERT_EQ(parc_buf_append(&b, "z", 1), PARC_OK);
        if (b.cap != last_cap) {
            ++reallocs;
            last_cap = b.cap;
        }
    }
    EXPECT_LE(reallocs, 50u);  // linear growth would give thousands
    parc_buf_free(&b);
}

TEST(Buf, FreeIsIdempotentAndResets) {
    parc_buf b;
    parc_buf_init(&b);
    ASSERT_EQ(parc_buf_append(&b, "abc", 3), PARC_OK);
    parc_buf_free(&b);
    EXPECT_EQ(b.data, nullptr);
    EXPECT_EQ(b.len, 0u);
    EXPECT_EQ(b.cap, 0u);
    parc_buf_free(&b);  // second free must be safe
    // and the buffer must be reusable
    ASSERT_EQ(parc_buf_append(&b, "d", 1), PARC_OK);
    EXPECT_EQ(b.len, 1u);
    parc_buf_free(&b);
}
