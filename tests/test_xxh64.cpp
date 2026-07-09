// Reference vectors generated with the official xxHash library
// (py-xxhash 3.x / xxhsum) — regenerate with tools/gen_xxh64_vectors.py.
#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "util/xxh64.h"

namespace {

// pat[i] = (i * 167 + 13) % 251
std::vector<uint8_t> pattern(size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i)
        v[i] = static_cast<uint8_t>((i * 167 + 13) % 251);
    return v;
}

struct Vec {
    const void *data;
    size_t len;
    uint64_t seed;
    uint64_t digest;
};

std::vector<Vec> vectors() {
    static const std::vector<uint8_t> pat = pattern(1024);
    return {
        {nullptr, 0, 0x0ULL, 0xEF46DB3751D8E999ULL},
        {nullptr, 0, 0x9E3779B97F4A7C15ULL, 0xC4349FC93C010000ULL},
        {"a", 1, 0x0ULL, 0xD24EC4F1A98C6E5BULL},
        {"abc", 3, 0x0ULL, 0x44BC2CF5AD770999ULL},
        {"abc", 3, 0x2AULL, 0x13C1D910702770E6ULL},
        {"Hello, world!", 13, 0x0ULL, 0xF58336A78B6F9476ULL},
        {pat.data(), 31, 0x0ULL, 0x87062C6BAB5BC575ULL},
        {pat.data(), 32, 0x0ULL, 0xFC141BBF42826CEEULL},
        {pat.data(), 33, 0x0ULL, 0xF458E506ADC82633ULL},
        {pat.data(), 63, 0x7ULL, 0x94F6214A831C9834ULL},
        {pat.data(), 64, 0x7ULL, 0x2573422B0B03A0C4ULL},
        {pat.data(), 1024, 0x0ULL, 0x7D5955AFD9B04B29ULL},
        {pat.data(), 1024, 0xDEADBEEFULL, 0x018AB1E512E3EBA4ULL},
    };
}

}  // namespace

TEST(Xxh64, OneShotMatchesReferenceVectors) {
    for (const auto &v : vectors()) {
        EXPECT_EQ(parc_xxh64(v.data, v.len, v.seed), v.digest)
            << "len=" << v.len << " seed=" << v.seed;
    }
}

TEST(Xxh64, StreamingMatchesOneShotForAllChunkings) {
    const auto pat = pattern(1024);
    const uint64_t expect = parc_xxh64(pat.data(), pat.size(), 42);
    for (size_t chunk : {size_t{1}, size_t{3}, size_t{7}, size_t{31},
                         size_t{32}, size_t{33}, size_t{64}, size_t{257},
                         size_t{1024}}) {
        parc_xxh64_state st;
        parc_xxh64_init(&st, 42);
        for (size_t off = 0; off < pat.size(); off += chunk) {
            size_t n = std::min(chunk, pat.size() - off);
            parc_xxh64_update(&st, pat.data() + off, n);
        }
        EXPECT_EQ(parc_xxh64_digest(&st), expect) << "chunk=" << chunk;
    }
}

TEST(Xxh64, StreamingMatchesReferenceVectors) {
    for (const auto &v : vectors()) {
        parc_xxh64_state st;
        parc_xxh64_init(&st, v.seed);
        parc_xxh64_update(&st, v.data, v.len);
        EXPECT_EQ(parc_xxh64_digest(&st), v.digest) << "len=" << v.len;
    }
}

TEST(Xxh64, DigestIsNonDestructive) {
    parc_xxh64_state st;
    parc_xxh64_init(&st, 0);
    parc_xxh64_update(&st, "Hello, ", 7);
    (void)parc_xxh64_digest(&st);  // mid-stream peek must not disturb state
    parc_xxh64_update(&st, "world!", 6);
    EXPECT_EQ(parc_xxh64_digest(&st), parc_xxh64("Hello, world!", 13, 0));
}

TEST(Xxh64, EmptyUpdatesAreNoOps) {
    parc_xxh64_state st;
    parc_xxh64_init(&st, 7);
    parc_xxh64_update(&st, nullptr, 0);
    parc_xxh64_update(&st, "abc", 3);
    parc_xxh64_update(&st, nullptr, 0);
    EXPECT_EQ(parc_xxh64_digest(&st), parc_xxh64("abc", 3, 7));
}
