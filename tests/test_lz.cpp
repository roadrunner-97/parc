#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "codec/lz.h"
#include "gen.h"
#include "util/buf.h"
#include "util/rng.h"

namespace {

struct LzResult {
    std::vector<parc_tok> toks;
    std::vector<uint8_t> rebuilt;
};

// Check every token invariant and rebuild the input from a token stream.
void check_rebuild(LzResult &r, const std::vector<uint8_t> &in) {
    for (const auto &t : r.toks) {
        if (t.dist == 0) {
            EXPECT_LE(t.len_or_lit, 255u);
            r.rebuilt.push_back(static_cast<uint8_t>(t.len_or_lit));
        } else {
            EXPECT_GE(t.len_or_lit, unsigned{PARC_LZ_MIN_MATCH});
            EXPECT_LE(t.dist, r.rebuilt.size());
            if (t.dist > r.rebuilt.size()) {
                ADD_FAILURE() << "match reaches before block start";
                return;
            }
            for (uint32_t k = 0; k < t.len_or_lit; ++k)
                r.rebuilt.push_back(r.rebuilt[r.rebuilt.size() - t.dist]);
        }
    }
    EXPECT_EQ(r.rebuilt, in);
}

// Run the greedy matcher, check every token invariant, and rebuild.
LzResult lz(const std::vector<uint8_t> &in) {
    LzResult r;
    r.toks.resize(in.size() + 1);
    std::vector<uint32_t> htab(1u << PARC_LZ_HASH_BITS);
    size_t nt = parc_lz_greedy(in.data(), in.size(), r.toks.data(),
                               htab.data());
    EXPECT_LE(nt, in.size());
    r.toks.resize(nt);
    for (const auto &t : r.toks) {
        if (t.dist == 0) {
            EXPECT_LE(t.len_or_lit, 255u);
            r.rebuilt.push_back(static_cast<uint8_t>(t.len_or_lit));
        } else {
            EXPECT_GE(t.len_or_lit, unsigned{PARC_LZ_MIN_MATCH});
            EXPECT_LE(t.dist, r.rebuilt.size());
            if (t.dist > r.rebuilt.size()) {
                ADD_FAILURE() << "match reaches before block start";
                return r;
            }
            for (uint32_t k = 0; k < t.len_or_lit; ++k)
                r.rebuilt.push_back(r.rebuilt[r.rebuilt.size() - t.dist]);
        }
    }
    return r;
}

// Run the chain matcher at one level, check invariants, and rebuild.
LzResult lz_chain(const std::vector<uint8_t> &in, unsigned level) {
    LzResult r;
    r.toks.resize(in.size() + 1);
    std::vector<uint32_t> head(1u << PARC_LZ_HASH_BITS);
    std::vector<uint32_t> prev(in.size() ? in.size() : 1);
    parc_lz_cfg cfg = parc_lz_cfg_for_level(level);
    size_t nt = parc_lz_chain(in.data(), in.size(), r.toks.data(),
                              head.data(), prev.data(), cfg);
    EXPECT_LE(nt, in.size());
    r.toks.resize(nt);
    check_rebuild(r, in);
    return r;
}

std::vector<uint8_t> gen(parc_err (*g)(parc_rng *, size_t, parc_buf *),
                         uint64_t seed, size_t n) {
    parc_rng rng;
    parc_rng_seed(&rng, seed);
    parc_buf b;
    parc_buf_init(&b);
    EXPECT_EQ(g(&rng, n, &b), PARC_OK);
    std::vector<uint8_t> v(b.data, b.data + b.len);
    parc_buf_free(&b);
    return v;
}

}  // namespace

TEST(Lz, TokensRebuildInputAcrossGenerators) {
    for (auto g : {parc_gen_random, parc_gen_text, parc_gen_json_log}) {
        for (size_t n : {size_t{0}, size_t{1}, size_t{3}, size_t{4},
                         size_t{5}, size_t{4096}, size_t{100000}}) {
            auto in = gen(g, 42 + n, n);
            auto r = lz(in);
            EXPECT_EQ(r.rebuilt, in);
        }
    }
}

TEST(Lz, ShortInputsAreAllLiterals) {
    for (size_t n = 0; n < PARC_LZ_MIN_MATCH; ++n) {
        std::vector<uint8_t> in(n, 0xAB);
        auto r = lz(in);
        EXPECT_EQ(r.rebuilt, in);
        EXPECT_EQ(r.toks.size(), n);
        for (const auto &t : r.toks) EXPECT_EQ(t.dist, 0u);
    }
}

TEST(Lz, LongRunCollapsesToOverlappedMatch) {
    std::vector<uint8_t> in(100000, 'a');
    auto r = lz(in);
    EXPECT_EQ(r.rebuilt, in);
    // literal 'a', then one (or few) overlapped dist-1 matches
    EXPECT_LE(r.toks.size(), 8u);
    ASSERT_GE(r.toks.size(), 2u);
    EXPECT_EQ(r.toks[1].dist, 1u);
}

TEST(Lz, RepeatedPhraseIsMatched) {
    std::string s;
    for (int i = 0; i < 50; ++i) s += "the quick brown fox #" + std::to_string(i % 7) + " ";
    std::vector<uint8_t> in(s.begin(), s.end());
    auto r = lz(in);
    EXPECT_EQ(r.rebuilt, in);
    EXPECT_LT(r.toks.size(), in.size() / 3);  // must find real matches
}

TEST(LzChain, TokensRebuildInputAcrossLevelsAndGenerators) {
    for (unsigned lvl = 2; lvl <= unsigned{PARC_LZ_LEVEL_MAX}; ++lvl)
        for (auto g : {parc_gen_random, parc_gen_text, parc_gen_json_log})
            for (size_t n : {size_t{0}, size_t{1}, size_t{3}, size_t{4},
                             size_t{5}, size_t{63}, size_t{4096},
                             size_t{100000}}) {
                auto in = gen(g, 42 + n + lvl, n);
                lz_chain(in, lvl);  // asserts rebuild == in
            }
}

TEST(LzChain, ShortInputsAreAllLiterals) {
    for (size_t n = 0; n < PARC_LZ_MIN_MATCH; ++n) {
        std::vector<uint8_t> in(n, 0xAB);
        auto r = lz_chain(in, 9);
        EXPECT_EQ(r.toks.size(), n);
        for (const auto &t : r.toks) EXPECT_EQ(t.dist, 0u);
    }
}

TEST(LzChain, DeeperSearchIsNeverWorse) {
    // On repetitive text the chain matcher must not emit more tokens than the
    // greedy matcher, and deeper levels must not regress against shallower.
    std::string s;
    for (int i = 0; i < 400; ++i)
        s += "the quick brown fox #" + std::to_string(i % 13) + " ";
    std::vector<uint8_t> in(s.begin(), s.end());
    size_t greedy = lz(in).toks.size();
    for (unsigned lvl = 2; lvl <= unsigned{PARC_LZ_LEVEL_MAX}; ++lvl)
        EXPECT_LE(lz_chain(in, lvl).toks.size(), greedy) << "level " << lvl;
}

TEST(LzChain, LongRunCollapsesToOverlappedMatch) {
    std::vector<uint8_t> in(100000, 'a');
    auto r = lz_chain(in, 9);
    EXPECT_LE(r.toks.size(), 8u);
    ASSERT_GE(r.toks.size(), 2u);
    EXPECT_EQ(r.toks[1].dist, 1u);
}
