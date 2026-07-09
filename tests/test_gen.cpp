#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "gen.h"
#include "stat/stats.h"

namespace {

struct Measured {
    double o0, o1;
};

Measured measure(const parc_buf &b) {
    parc_stats *st = nullptr;
    EXPECT_EQ(parc_stats_create(&st), PARC_OK);
    parc_stats_update(st, b.data, b.len);
    Measured m{parc_stats_entropy_o0(st), parc_stats_entropy_o1(st)};
    parc_stats_destroy(st);
    return m;
}

using GenFn = parc_err (*)(parc_rng *, size_t, parc_buf *);

// Determinism + exact length + seed sensitivity, shared by all generators.
void check_generator_basics(GenFn fn, size_t len) {
    parc_rng r1, r2, r3;
    parc_rng_seed(&r1, 555);
    parc_rng_seed(&r2, 555);
    parc_rng_seed(&r3, 556);
    parc_buf a, b, c;
    parc_buf_init(&a); parc_buf_init(&b); parc_buf_init(&c);

    ASSERT_EQ(fn(&r1, len, &a), PARC_OK);
    ASSERT_EQ(fn(&r2, len, &b), PARC_OK);
    ASSERT_EQ(fn(&r3, len, &c), PARC_OK);
    ASSERT_EQ(a.len, len);
    ASSERT_EQ(b.len, len);
    EXPECT_EQ(std::memcmp(a.data, b.data, len), 0) << "same seed differs";
    if (len >= 64) {
        EXPECT_NE(std::memcmp(a.data, c.data, len), 0) << "seed ignored";
    }

    // len == 0 appends nothing
    ASSERT_EQ(fn(&r1, 0, &a), PARC_OK);
    EXPECT_EQ(a.len, len);

    parc_buf_free(&a); parc_buf_free(&b); parc_buf_free(&c);
}

}  // namespace

TEST(Gen, RandomBasics) {
    check_generator_basics(parc_gen_random, 1 << 20);
    parc_rng r; parc_rng_seed(&r, 1);
    parc_buf b; parc_buf_init(&b);
    ASSERT_EQ(parc_gen_random(&r, 1 << 20, &b), PARC_OK);
    EXPECT_GT(measure(b).o0, 7.99);
    parc_buf_free(&b);
}

TEST(Gen, EntropyHitsTargets) {
    for (double target : {0.5, 1.0, 2.0, 4.0, 6.0, 7.5}) {
        parc_rng r; parc_rng_seed(&r, 42);
        parc_buf b; parc_buf_init(&b);
        ASSERT_EQ(parc_gen_entropy(&r, target, 1 << 20, &b), PARC_OK);
        EXPECT_NEAR(measure(b).o0, target, 0.05) << "target " << target;
        parc_buf_free(&b);
    }
}

TEST(Gen, EntropyExtremesAndArgs) {
    parc_rng r; parc_rng_seed(&r, 42);
    parc_buf b; parc_buf_init(&b);
    ASSERT_EQ(parc_gen_entropy(&r, 0.0, 4096, &b), PARC_OK);
    for (size_t i = 0; i < b.len; ++i) ASSERT_EQ(b.data[i], b.data[0]);
    b.len = 0;
    ASSERT_EQ(parc_gen_entropy(&r, 8.0, 1 << 20, &b), PARC_OK);
    EXPECT_GT(measure(b).o0, 7.99);
    EXPECT_EQ(parc_gen_entropy(&r, -0.1, 16, &b), PARC_ERR_ARG);
    EXPECT_EQ(parc_gen_entropy(&r, 8.1, 16, &b), PARC_ERR_ARG);
    parc_buf_free(&b);
}

TEST(Gen, RunsHaveRequestedMeanLength) {
    parc_rng r; parc_rng_seed(&r, 9);
    parc_buf b; parc_buf_init(&b);
    ASSERT_EQ(parc_gen_runs(&r, 64.0, 1 << 20, &b), PARC_OK);

    size_t transitions = 0;
    for (size_t i = 1; i < b.len; ++i) transitions += b.data[i] != b.data[i - 1];
    // ~256/255 of run boundaries are visible transitions; 20% tolerance.
    double mean_run = double(b.len) / double(transitions + 1);
    EXPECT_NEAR(mean_run, 64.0, 64.0 * 0.2);

    auto m = measure(b);
    EXPECT_GT(m.o0, 7.0);   // run values are uniform bytes
    EXPECT_LT(m.o1, 1.0);   // but the next byte is usually the same
    EXPECT_EQ(parc_gen_runs(&r, 0.5, 16, &b), PARC_ERR_ARG);
    parc_buf_free(&b);
}

TEST(Gen, TextBasics) {
    check_generator_basics(parc_gen_text, 512 << 10);
    parc_rng r; parc_rng_seed(&r, 5);
    parc_buf b; parc_buf_init(&b);
    ASSERT_EQ(parc_gen_text(&r, 512 << 10, &b), PARC_OK);

    size_t line_len = 0, spaces = 0;
    for (size_t i = 0; i < b.len; ++i) {
        uint8_t ch = b.data[i];
        ASSERT_TRUE(ch == '\n' || (ch >= 0x20 && ch <= 0x7E))
            << "byte " << int(ch) << " at " << i;
        if (ch == '\n') line_len = 0;
        else ASSERT_LT(++line_len, 100u) << "unwrapped line at " << i;
        spaces += ch == ' ';
    }
    EXPECT_GT(spaces, b.len / 20);  // it should look like words

    auto m = measure(b);
    EXPECT_GT(m.o0, 3.0);
    EXPECT_LT(m.o0, 5.5);
    EXPECT_LE(m.o1, m.o0 - 0.5) << "text lacks sequential structure";
    parc_buf_free(&b);
}

TEST(Gen, JsonLogBasics) {
    check_generator_basics(parc_gen_json_log, 512 << 10);
    parc_rng r; parc_rng_seed(&r, 6);
    parc_buf b; parc_buf_init(&b);
    ASSERT_EQ(parc_gen_json_log(&r, 512 << 10, &b), PARC_OK);

    // Every complete line is {...} and mentions the fixed keys.
    std::string data(reinterpret_cast<const char *>(b.data), b.len);
    size_t start = 0, lines = 0;
    for (size_t nl = data.find('\n'); nl != std::string::npos;
         start = nl + 1, nl = data.find('\n', start), ++lines) {
        ASSERT_GT(nl, start) << "empty line " << lines;
        ASSERT_EQ(data[start], '{') << "line " << lines;
        ASSERT_EQ(data[nl - 1], '}') << "line " << lines;
        ASSERT_NE(data.find("\"ts\":", start), std::string::npos);
    }
    EXPECT_GT(lines, 100u);

    auto m = measure(b);
    EXPECT_LE(m.o1, m.o0 - 0.5) << "log lacks repeated structure";
    parc_buf_free(&b);
}

TEST(Gen, RecordsBasics) {
    parc_rng r; parc_rng_seed(&r, 8);
    parc_buf b; parc_buf_init(&b);
    ASSERT_EQ(parc_gen_records(&r, 64, 1 << 20, &b), PARC_OK);
    ASSERT_EQ(b.len, size_t{1} << 20);

    // Counter field: little-endian u32 at offset 0 of each record increments.
    for (uint32_t i = 0; i < 1000; ++i) {
        uint32_t v;
        std::memcpy(&v, b.data + size_t{i} * 64, 4);
        ASSERT_EQ(v, i) << "record " << i;
    }
    EXPECT_LT(measure(b).o0, 7.0);

    // determinism
    parc_rng r2; parc_rng_seed(&r2, 8);
    parc_buf b2; parc_buf_init(&b2);
    ASSERT_EQ(parc_gen_records(&r2, 64, 1 << 20, &b2), PARC_OK);
    EXPECT_EQ(std::memcmp(b.data, b2.data, b.len), 0);

    EXPECT_EQ(parc_gen_records(&r, 7, 64, &b), PARC_ERR_ARG);
    EXPECT_EQ(parc_gen_records(&r, 4097, 64, &b), PARC_ERR_ARG);
    parc_buf_free(&b);
    parc_buf_free(&b2);
}
