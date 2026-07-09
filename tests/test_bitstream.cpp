#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "util/bitstream.h"
#include "util/rng.h"

TEST(Bitstream, PinnedByteLayout) {
    // 1 (1 bit), 2 (2 bits), 0x13 (5 bits) => 0x9D. This byte layout is
    // part of the parc format; if this test breaks, the format broke.
    uint8_t out[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    parc_bw w;
    parc_bw_init(&w, out, sizeof out);
    parc_bw_put(&w, 1, 1);
    parc_bw_put(&w, 2, 2);
    parc_bw_put(&w, 0x13, 5);
    size_t n = 0;
    ASSERT_EQ(parc_bw_finish(&w, &n), PARC_OK);
    ASSERT_EQ(n, 1u);
    EXPECT_EQ(out[0], 0x9D);
}

TEST(Bitstream, FinalByteZeroPadded) {
    uint8_t out[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    parc_bw w;
    parc_bw_init(&w, out, sizeof out);
    parc_bw_put(&w, 0x3, 2);
    size_t n = 0;
    ASSERT_EQ(parc_bw_finish(&w, &n), PARC_OK);
    ASSERT_EQ(n, 1u);
    EXPECT_EQ(out[0], 0x03);  // high 6 bits zero-padded
}

TEST(Bitstream, EmptyStream) {
    parc_bw w;
    parc_bw_init(&w, nullptr, 0);
    size_t n = 123;
    EXPECT_EQ(parc_bw_finish(&w, &n), PARC_OK);
    EXPECT_EQ(n, 0u);

    parc_br r;
    parc_br_init(&r, nullptr, 0);
    EXPECT_EQ(parc_br_get(&r, 0), 0u);
    EXPECT_EQ(parc_br_err(&r), PARC_OK);
    EXPECT_EQ(parc_br_get(&r, 1), 0u);
    EXPECT_EQ(parc_br_err(&r), PARC_ERR_TRUNCATED);
}

TEST(Bitstream, RoundtripRandomFields) {
    parc_rng rng;
    parc_rng_seed(&rng, 0xB175);

    struct Field { uint64_t v; unsigned n; };
    std::vector<Field> fields;
    uint64_t total_bits = 0;
    for (int i = 0; i < 20000; ++i) {
        unsigned n = static_cast<unsigned>(
            parc_rng_range(&rng, PARC_BITSTREAM_MAX_BITS) + 1);  // [1,57]
        uint64_t v = parc_rng_next(&rng) & ((1ULL << n) - 1);
        fields.push_back({v, n});
        total_bits += n;
    }

    std::vector<uint8_t> buf((total_bits + 7) / 8);
    parc_bw w;
    parc_bw_init(&w, buf.data(), buf.size());
    for (const auto &f : fields) parc_bw_put(&w, f.v, f.n);
    size_t written = 0;
    ASSERT_EQ(parc_bw_finish(&w, &written), PARC_OK);
    EXPECT_EQ(written, buf.size());

    parc_br r;
    parc_br_init(&r, buf.data(), written);
    for (size_t i = 0; i < fields.size(); ++i)
        ASSERT_EQ(parc_br_get(&r, fields[i].n), fields[i].v) << "field " << i;
    EXPECT_EQ(parc_br_err(&r), PARC_OK);
    EXPECT_EQ(parc_br_bits_consumed(&r), total_bits);
}

TEST(Bitstream, MaxWidthFields) {
    const uint64_t v = (1ULL << PARC_BITSTREAM_MAX_BITS) - 1;
    uint8_t buf[64];
    parc_bw w;
    parc_bw_init(&w, buf, sizeof buf);
    for (int i = 0; i < 5; ++i)
        parc_bw_put(&w, i % 2 ? v : 0, PARC_BITSTREAM_MAX_BITS);
    size_t written = 0;
    ASSERT_EQ(parc_bw_finish(&w, &written), PARC_OK);

    parc_br r;
    parc_br_init(&r, buf, written);
    for (int i = 0; i < 5; ++i)
        ASSERT_EQ(parc_br_get(&r, PARC_BITSTREAM_MAX_BITS), i % 2 ? v : 0);
    EXPECT_EQ(parc_br_err(&r), PARC_OK);
}

TEST(Bitstream, WriterOverflowIsStickyAndReported) {
    uint8_t buf[1];
    parc_bw w;
    parc_bw_init(&w, buf, sizeof buf);
    for (int i = 0; i < 4; ++i) parc_bw_put(&w, 0x2A, 6);  // 24 bits > 8
    size_t n = 123;
    EXPECT_EQ(parc_bw_finish(&w, &n), PARC_ERR_LIMIT);
    EXPECT_EQ(n, 0u);
}

TEST(Bitstream, ReaderOverrunIsStickyReturnsZero) {
    const uint8_t buf[1] = {0xFF};
    parc_br r;
    parc_br_init(&r, buf, sizeof buf);
    EXPECT_EQ(parc_br_get(&r, 8), 0xFFu);
    EXPECT_EQ(parc_br_err(&r), PARC_OK);
    EXPECT_EQ(parc_br_get(&r, 1), 0u);  // past the end
    EXPECT_EQ(parc_br_err(&r), PARC_ERR_TRUNCATED);
    EXPECT_EQ(parc_br_get(&r, 8), 0u);  // still failed, still zero
    EXPECT_EQ(parc_br_err(&r), PARC_ERR_TRUNCATED);
}

TEST(Bitstream, ZeroWidthPutGetAreNoOps) {
    uint8_t buf[2];
    parc_bw w;
    parc_bw_init(&w, buf, sizeof buf);
    parc_bw_put(&w, 0, 0);
    parc_bw_put(&w, 0x5, 3);
    parc_bw_put(&w, 0, 0);
    size_t n = 0;
    ASSERT_EQ(parc_bw_finish(&w, &n), PARC_OK);
    ASSERT_EQ(n, 1u);

    parc_br r;
    parc_br_init(&r, buf, n);
    EXPECT_EQ(parc_br_get(&r, 0), 0u);
    EXPECT_EQ(parc_br_get(&r, 3), 0x5u);
    EXPECT_EQ(parc_br_err(&r), PARC_OK);
    EXPECT_EQ(parc_br_bits_consumed(&r), 3u);
}
