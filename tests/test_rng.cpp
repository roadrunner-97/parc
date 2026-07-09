// The RNG algorithm is pinned (see src/util/rng.h): splitmix64 state
// expansion + xoshiro256** 1.0. This file embeds the canonical public-domain
// reference (Blackman/Vigna) and requires exact output equality.
#include <cstdint>
#include <cstring>
#include <set>
#include <vector>

#include <gtest/gtest.h>

#include "util/rng.h"

namespace ref {

inline uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

inline uint64_t splitmix64(uint64_t *x) {
    uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

struct Xoshiro256ss {
    uint64_t s[4];
    explicit Xoshiro256ss(uint64_t seed) {
        uint64_t x = seed;
        for (auto &w : s) w = splitmix64(&x);
    }
    uint64_t next() {
        const uint64_t result = rotl(s[1] * 5, 7) * 9;
        const uint64_t t = s[1] << 17;
        s[2] ^= s[0];
        s[3] ^= s[1];
        s[1] ^= s[2];
        s[0] ^= s[3];
        s[2] ^= t;
        s[3] = rotl(s[3], 45);
        return result;
    }
};

}  // namespace ref

TEST(Rng, MatchesCanonicalReference) {
    for (uint64_t seed : {0ULL, 1ULL, 42ULL, 0xDEADBEEFULL,
                          0xFFFFFFFFFFFFFFFFULL}) {
        parc_rng r;
        parc_rng_seed(&r, seed);
        ref::Xoshiro256ss x(seed);
        for (int i = 0; i < 10000; ++i)
            ASSERT_EQ(parc_rng_next(&r), x.next())
                << "seed=" << seed << " i=" << i;
    }
}

TEST(Rng, FillIsLittleEndianNextStream) {
    parc_rng r;
    parc_rng_seed(&r, 99);
    uint8_t got[27];  // deliberately not a multiple of 8
    parc_rng_fill(&r, got, sizeof got);

    ref::Xoshiro256ss x(99);
    uint8_t want[27];
    for (size_t off = 0; off < sizeof want; off += 8) {
        uint64_t w = x.next();
        for (size_t j = 0; j < 8 && off + j < sizeof want; ++j)
            want[off + j] = static_cast<uint8_t>(w >> (8 * j));
    }
    EXPECT_EQ(std::memcmp(got, want, sizeof got), 0);

    parc_rng_fill(&r, nullptr, 0);  // must be legal
}

TEST(Rng, F64PinnedMapping) {
    parc_rng a, b;
    parc_rng_seed(&a, 7);
    parc_rng_seed(&b, 7);
    for (int i = 0; i < 1000; ++i) {
        double d = parc_rng_f64(&a);
        double want =
            static_cast<double>(parc_rng_next(&b) >> 11) * 0x1.0p-53;
        ASSERT_EQ(d, want);
        ASSERT_GE(d, 0.0);
        ASSERT_LT(d, 1.0);
    }
}

TEST(Rng, RangeBoundsAndCoverage) {
    parc_rng r;
    parc_rng_seed(&r, 12345);
    EXPECT_EQ(parc_rng_range(&r, 0), 0u);
    EXPECT_EQ(parc_rng_range(&r, 1), 0u);

    std::set<uint64_t> seen;
    for (int i = 0; i < 10000; ++i) {
        uint64_t v = parc_rng_range(&r, 17);
        ASSERT_LT(v, 17u);
        seen.insert(v);
    }
    EXPECT_EQ(seen.size(), 17u);  // 10k draws must hit all 17 values

    for (int i = 0; i < 1000; ++i)
        ASSERT_LT(parc_rng_range(&r, 0xFFFFFFFFFFFFFFFFULL),
                  0xFFFFFFFFFFFFFFFFULL);
}

TEST(Rng, SeedsProduceDistinctStreams) {
    parc_rng a, b;
    parc_rng_seed(&a, 1);
    parc_rng_seed(&b, 2);
    int diff = 0;
    for (int i = 0; i < 64; ++i)
        diff += parc_rng_next(&a) != parc_rng_next(&b);
    EXPECT_GE(diff, 60);
}
