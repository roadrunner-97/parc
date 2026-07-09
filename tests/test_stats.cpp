#include <cmath>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "stat/stats.h"
#include "util/rng.h"

namespace {

struct StatsPtr {
    parc_stats *st = nullptr;
    StatsPtr() { EXPECT_EQ(parc_stats_create(&st), PARC_OK); }
    ~StatsPtr() { parc_stats_destroy(st); }
};

std::vector<uint8_t> random_bytes(size_t n, uint64_t seed) {
    std::vector<uint8_t> v(n);
    parc_rng r;
    parc_rng_seed(&r, seed);
    parc_rng_fill(&r, v.data(), n);
    return v;
}

}  // namespace

TEST(Stats, EmptyInput) {
    StatsPtr s;
    EXPECT_EQ(parc_stats_len(s.st), 0u);
    EXPECT_EQ(parc_stats_entropy_o0(s.st), 0.0);
    EXPECT_EQ(parc_stats_entropy_o1(s.st), 0.0);
    EXPECT_EQ(parc_stats_min_entropy(s.st), 0.0);
    EXPECT_EQ(parc_stats_mean(s.st), 0.0);
    double p = -1;
    EXPECT_EQ(parc_stats_chi2(s.st, &p), 0.0);
    EXPECT_EQ(p, 1.0);
    EXPECT_TRUE(std::isnan(parc_stats_serial_corr(s.st)));
    EXPECT_TRUE(std::isnan(parc_stats_montecarlo_pi(s.st)));
}

TEST(Stats, ClosedFormEntropies) {
    {   // constant input
        StatsPtr s;
        std::vector<uint8_t> z(4096, 0x42);
        parc_stats_update(s.st, z.data(), z.size());
        EXPECT_DOUBLE_EQ(parc_stats_entropy_o0(s.st), 0.0);
        EXPECT_DOUBLE_EQ(parc_stats_entropy_o1(s.st), 0.0);
        EXPECT_DOUBLE_EQ(parc_stats_min_entropy(s.st), 0.0);
        EXPECT_DOUBLE_EQ(parc_stats_mean(s.st), double(0x42));
        EXPECT_TRUE(std::isnan(parc_stats_serial_corr(s.st)));
    }
    {   // alternating "abab...": o0 = 1 exactly, o1 = 0 (fully determined)
        StatsPtr s;
        std::vector<uint8_t> v(4096);
        for (size_t i = 0; i < v.size(); ++i) v[i] = i % 2 ? 'b' : 'a';
        parc_stats_update(s.st, v.data(), v.size());
        EXPECT_DOUBLE_EQ(parc_stats_entropy_o0(s.st), 1.0);
        EXPECT_NEAR(parc_stats_entropy_o1(s.st), 0.0, 1e-9);
        // strongly anti-correlated neighbors
        EXPECT_LT(parc_stats_serial_corr(s.st), -0.99);
    }
    {   // exact 3:1 mixture: H = 2 - 0.75*log2(3), Hmin = -log2(0.75)
        StatsPtr s;
        std::vector<uint8_t> v;
        for (int i = 0; i < 1024; ++i) {
            v.push_back('A'); v.push_back('A'); v.push_back('A');
            v.push_back('B');
        }
        parc_stats_update(s.st, v.data(), v.size());
        EXPECT_NEAR(parc_stats_entropy_o0(s.st),
                    2.0 - 0.75 * std::log2(3.0), 1e-12);
        EXPECT_NEAR(parc_stats_min_entropy(s.st), -std::log2(0.75), 1e-12);
    }
}

TEST(Stats, UniformRandomLooksUniform) {
    StatsPtr s;
    auto v = random_bytes(1 << 20, 2026);
    parc_stats_update(s.st, v.data(), v.size());
    EXPECT_GT(parc_stats_entropy_o0(s.st), 7.99);
    EXPECT_GT(parc_stats_entropy_o1(s.st), 7.8);
    EXPECT_NEAR(parc_stats_mean(s.st), 127.5, 0.5);
    double p = -1;
    parc_stats_chi2(s.st, &p);
    EXPECT_GT(p, 0.001);
    EXPECT_LT(p, 0.999);
    EXPECT_NEAR(parc_stats_serial_corr(s.st), 0.0, 0.01);
}

TEST(Stats, MonteCarloPiOnUniform) {
    StatsPtr s;
    auto v = random_bytes(6u << 20, 7);  // 1M points
    parc_stats_update(s.st, v.data(), v.size());
    EXPECT_NEAR(parc_stats_montecarlo_pi(s.st), 3.14159265, 0.01);
}

TEST(Stats, SerialCorrOfRamp) {
    StatsPtr s;
    std::vector<uint8_t> v(1 << 20);
    for (size_t i = 0; i < v.size(); ++i) v[i] = uint8_t(i & 0xFF);
    parc_stats_update(s.st, v.data(), v.size());
    EXPECT_GT(parc_stats_serial_corr(s.st), 0.9);
}

TEST(Stats, ChunkingInvariance) {
    // Results must be identical regardless of update chunking — this pins
    // the carry of order-1 pairs and 6-byte Monte Carlo groups across
    // update boundaries.
    auto v = random_bytes((6 << 16) + 5, 99);  // not a multiple of 6
    StatsPtr one, many;
    parc_stats_update(one.st, v.data(), v.size());
    for (size_t off = 0; off < v.size(); off += 7) {
        size_t n = std::min<size_t>(7, v.size() - off);
        parc_stats_update(many.st, v.data() + off, n);
    }
    EXPECT_DOUBLE_EQ(parc_stats_entropy_o0(one.st),
                     parc_stats_entropy_o0(many.st));
    EXPECT_DOUBLE_EQ(parc_stats_entropy_o1(one.st),
                     parc_stats_entropy_o1(many.st));
    EXPECT_DOUBLE_EQ(parc_stats_serial_corr(one.st),
                     parc_stats_serial_corr(many.st));
    EXPECT_DOUBLE_EQ(parc_stats_montecarlo_pi(one.st),
                     parc_stats_montecarlo_pi(many.st));
}

TEST(Stats, ResetIsComplete) {
    StatsPtr s;
    auto v = random_bytes(1 << 16, 3);
    parc_stats_update(s.st, v.data(), v.size());
    parc_stats_reset(s.st);
    EXPECT_EQ(parc_stats_len(s.st), 0u);
    EXPECT_EQ(parc_stats_entropy_o0(s.st), 0.0);
    EXPECT_TRUE(std::isnan(parc_stats_montecarlo_pi(s.st)));
    // and it must behave like a fresh instance afterwards
    std::vector<uint8_t> z(100, 7);
    parc_stats_update(s.st, z.data(), z.size());
    EXPECT_DOUBLE_EQ(parc_stats_entropy_o0(s.st), 0.0);
    EXPECT_DOUBLE_EQ(parc_stats_mean(s.st), 7.0);
}

TEST(Stats, Chi2SurvivalFunctionMatchesScipy) {
    // Reference values from scipy.stats.chi2.sf (see git history for the
    // generating snippet); relative tolerance 1e-6.
    struct V { double x; unsigned df; double p; };
    const V vecs[] = {
        {200.0, 255, 9.954254445420e-01},
        {235.0, 255, 8.106151039939e-01},
        {255.0, 255, 4.882225217704e-01},
        {290.33, 255, 6.341561651029e-02},
        {310.46, 255, 9.997294630857e-03},
        {400.0, 255, 1.660002524412e-08},
        {3.84, 1, 5.004352124871e-02},
        {5.99, 2, 5.003662708659e-02},
        {16.92, 9, 4.998360638751e-02},
    };
    for (const auto &v : vecs)
        EXPECT_NEAR(parc_chi2_sf(v.x, v.df), v.p, v.p * 1e-6)
            << "x=" << v.x << " df=" << v.df;
    EXPECT_EQ(parc_chi2_sf(0.0, 255), 1.0);
    EXPECT_TRUE(std::isnan(parc_chi2_sf(-1.0, 255)));
    EXPECT_TRUE(std::isnan(parc_chi2_sf(1.0, 0)));
}

TEST(Stats, EntropyProfileLocatesStructure) {
    // 64 KiB of zeros then 64 KiB of random: profile must show it.
    std::vector<uint8_t> v(128 << 10, 0);
    auto rnd = random_bytes(64 << 10, 11);
    std::copy(rnd.begin(), rnd.end(), v.begin() + (64 << 10));

    double out[32];
    size_t n = 0;
    ASSERT_EQ(parc_entropy_profile(v.data(), v.size(), 4096, 4096, out, &n),
              PARC_OK);
    ASSERT_EQ(n, 32u);
    for (size_t i = 0; i < 16; ++i) EXPECT_LT(out[i], 0.01) << i;
    for (size_t i = 16; i < 32; ++i) EXPECT_GT(out[i], 7.5) << i;
}

TEST(Stats, EntropyProfileEdgeCases) {
    double out[4];
    size_t n = 99;
    uint8_t d[10] = {0};

    EXPECT_EQ(parc_entropy_profile(d, 0, 4, 4, out, &n), PARC_OK);
    EXPECT_EQ(n, 0u);
    // short input: one window covering everything
    EXPECT_EQ(parc_entropy_profile(d, 10, 4096, 4096, out, &n), PARC_OK);
    EXPECT_EQ(n, 1u);
    EXPECT_EQ(out[0], 0.0);
    // overlapping windows: 10 bytes, window 4, stride 2 -> starts 0,2,4,6
    EXPECT_EQ(parc_entropy_profile(d, 10, 4, 2, out, &n), PARC_OK);
    EXPECT_EQ(n, 4u);
    EXPECT_EQ(parc_entropy_profile(d, 10, 0, 4, out, &n), PARC_ERR_ARG);
    EXPECT_EQ(parc_entropy_profile(d, 10, 4, 0, out, &n), PARC_ERR_ARG);
}
