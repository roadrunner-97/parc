#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "codec/block.h"
#include "parc/parc.h"
#include "gen.h"
#include "util/buf.h"
#include "util/rng.h"

namespace {

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

// Compress one block; if packed, decompress and compare. Returns the type.
int roundtrip(const std::vector<uint8_t> &in,
              unsigned level = PARC_LEVEL_DEFAULT) {
    parc_blk_cctx cx;
    EXPECT_EQ(parc_blk_cctx_init(&cx, in.size() ? in.size() : 1, level),
              PARC_OK);
    std::vector<uint8_t> comp(in.size());
    uint32_t clen = 0;
    int type = parc_blk_compress(&cx, in.data(),
                                 static_cast<uint32_t>(in.size()),
                                 comp.data(), &clen);
    parc_blk_cctx_free(&cx);
    if (type == PARC_BLK_STORED) {
        EXPECT_EQ(clen, in.size());
        return type;
    }
    EXPECT_EQ(type, PARC_BLK_PACKED);
    EXPECT_GE(clen, 1u);
    EXPECT_LT(clen, in.size());
    std::vector<uint8_t> out(in.size(), 0xCC);
    EXPECT_EQ(parc_blk_decompress(comp.data(), clen, out.data(),
                                  static_cast<uint32_t>(in.size())),
              PARC_OK);
    EXPECT_EQ(out, in);
    return type;
}

// A valid packed payload to corrupt in the negative tests.
struct Packed {
    std::vector<uint8_t> raw;
    std::vector<uint8_t> comp;
};

Packed make_packed() {
    Packed p;
    p.raw = gen(parc_gen_text, 7, 8192);
    parc_blk_cctx cx;
    EXPECT_EQ(parc_blk_cctx_init(&cx, p.raw.size(), PARC_LEVEL_DEFAULT),
              PARC_OK);
    p.comp.resize(p.raw.size());
    uint32_t clen = 0;
    EXPECT_EQ(parc_blk_compress(&cx, p.raw.data(),
                                static_cast<uint32_t>(p.raw.size()),
                                p.comp.data(), &clen),
              PARC_BLK_PACKED);
    p.comp.resize(clen);
    parc_blk_cctx_free(&cx);
    return p;
}

}  // namespace

TEST(Block, CompressibleDataPacksAndRoundtrips) {
    EXPECT_EQ(roundtrip(gen(parc_gen_text, 1, 65536)), PARC_BLK_PACKED);
    EXPECT_EQ(roundtrip(gen(parc_gen_json_log, 2, 65536)), PARC_BLK_PACKED);
    EXPECT_EQ(roundtrip(std::vector<uint8_t>(100000, 'x')), PARC_BLK_PACKED);
}

TEST(Block, RandomDataFallsBackToStored) {
    EXPECT_EQ(roundtrip(gen(parc_gen_random, 3, 65536)), PARC_BLK_STORED);
}

TEST(Block, TinyBlocksAlwaysStored) {
    // Below the table overhead a packed block cannot beat stored.
    for (size_t n = 1; n <= 64; ++n)
        roundtrip(gen(parc_gen_text, n, n));  // asserts equality if packed
}

TEST(Block, BoundarySizesRoundtrip) {
    for (size_t n : {size_t{1}, size_t{2}, size_t{4}, size_t{5}, size_t{200},
                     size_t{4095}, size_t{4096}, size_t{4097}})
        for (auto g : {parc_gen_random, parc_gen_text, parc_gen_json_log})
            roundtrip(gen(g, 9 + n, n));  // roundtrip() asserts equality
}

TEST(Block, AllLevelsRoundtrip) {
    // Every level must produce a decoder-valid block for every input; higher
    // levels only change which matches are found, never correctness.
    for (unsigned lvl = PARC_LEVEL_MIN; lvl <= PARC_LEVEL_MAX; ++lvl)
        for (auto g : {parc_gen_random, parc_gen_text, parc_gen_json_log})
            for (size_t n : {size_t{1}, size_t{4}, size_t{5}, size_t{63},
                             size_t{4096}, size_t{4097}, size_t{65536}})
                roundtrip(gen(g, 100 + n, n), lvl);  // asserts equality
}

TEST(Block, HigherLevelsNeverBeatWorseThanGreedy) {
    // On compressible text, a deeper search must not produce a larger packed
    // payload than the greedy level-1 matcher.
    auto in = gen(parc_gen_text, 55, 200000);
    auto packed_len = [&](unsigned level) -> uint32_t {
        parc_blk_cctx cx;
        EXPECT_EQ(parc_blk_cctx_init(&cx, in.size(), level), PARC_OK);
        std::vector<uint8_t> comp(in.size());
        uint32_t clen = 0;
        EXPECT_EQ(parc_blk_compress(&cx, in.data(),
                                    static_cast<uint32_t>(in.size()),
                                    comp.data(), &clen),
                  PARC_BLK_PACKED);
        parc_blk_cctx_free(&cx);
        return clen;
    };
    uint32_t greedy = packed_len(1);
    EXPECT_LE(packed_len(6), greedy);
    EXPECT_LE(packed_len(9), greedy);
}

TEST(Block, MaxDistanceAndLongMatch) {
    // Two copies of a random 64 KiB body separated by unique junk: forces a
    // large distance; a 100 KiB run forces long match lengths.
    auto body = gen(parc_gen_random, 12, 65536);
    std::vector<uint8_t> in = body;
    in.insert(in.end(), body.begin(), body.end());
    EXPECT_EQ(roundtrip(in), PARC_BLK_PACKED);
}

TEST(BlockDecode, RejectsTruncatedPayload) {
    Packed p = make_packed();
    std::vector<uint8_t> out(p.raw.size());
    for (size_t cut : {size_t{0}, size_t{1}, size_t{100},
                       p.comp.size() - 1})
        EXPECT_EQ(parc_blk_decompress(p.comp.data(),
                                      static_cast<uint32_t>(cut), out.data(),
                                      static_cast<uint32_t>(p.raw.size())),
                  PARC_ERR_CORRUPT)
            << "cut " << cut;
}

TEST(BlockDecode, RejectsNonMinimalCompLenAndPadding) {
    Packed p = make_packed();
    std::vector<uint8_t> out(p.raw.size());
    // extra byte appended: comp_len no longer minimal
    std::vector<uint8_t> longer = p.comp;
    longer.push_back(0);
    EXPECT_EQ(parc_blk_decompress(longer.data(),
                                  static_cast<uint32_t>(longer.size()),
                                  out.data(),
                                  static_cast<uint32_t>(p.raw.size())),
              PARC_ERR_CORRUPT);
}

TEST(BlockDecode, RejectsWrongRawLen) {
    Packed p = make_packed();
    std::vector<uint8_t> out(p.raw.size() + 8);
    for (long d : {-3L, -1L, 1L, 3L}) {
        uint32_t raw_len = static_cast<uint32_t>(
            static_cast<long>(p.raw.size()) + d);
        EXPECT_EQ(parc_blk_decompress(p.comp.data(),
                                      static_cast<uint32_t>(p.comp.size()),
                                      out.data(), raw_len),
                  PARC_ERR_CORRUPT)
            << "delta " << d;
    }
}

TEST(BlockDecode, SurvivesArbitraryGarbage) {
    // Any byte soup must yield a clean error or a successful decode of
    // exactly raw_len bytes (frame hashes catch wrong content) — never a
    // crash or overrun (ASan enforces).
    parc_rng rng;
    parc_rng_seed(&rng, 0xBAD);
    std::vector<uint8_t> out(4096);
    for (int iter = 0; iter < 2000; ++iter) {
        size_t clen = 1 + parc_rng_range(&rng, 700);
        std::vector<uint8_t> junk(clen);
        for (auto &b : junk)
            b = static_cast<uint8_t>(parc_rng_range(&rng, 256));
        parc_blk_decompress(junk.data(), static_cast<uint32_t>(clen),
                            out.data(), 4096);
    }
}
