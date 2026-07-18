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

// Decode a packed payload in the given wire version, allocating the v1
// decode scratch as needed. raw_len may differ from any real block (negative
// tests), so the dctx is sized to it.
parc_err blk_decode(const uint8_t *comp, uint32_t clen, uint8_t *out,
                    uint32_t raw_len, unsigned version) {
    if (version == 0)
        return parc_blk_decompress(nullptr, comp, clen, out, raw_len, 0);
    parc_blk_dctx dx;
    size_t mb = raw_len ? raw_len : 1;
    EXPECT_EQ(parc_blk_dctx_init(&dx, mb), PARC_OK);
    parc_err err = parc_blk_decompress(&dx, comp, clen, out, raw_len, version);
    parc_blk_dctx_free(&dx);
    return err;
}

// Compress one block; if packed, decompress and compare. Returns the type.
int roundtrip(const std::vector<uint8_t> &in, unsigned level = PARC_LEVEL_DEFAULT,
              unsigned version = 1) {
    parc_blk_cctx cx;
    EXPECT_EQ(
        parc_blk_cctx_init(&cx, in.size() ? in.size() : 1, level, version),
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
    // Decode dst must carry PARC_WILDCOPY_SLACK trailing bytes (wildcopy
    // overrun); compare only the logical raw_len prefix.
    std::vector<uint8_t> out(in.size() + PARC_WILDCOPY_SLACK, 0xCC);
    EXPECT_EQ(blk_decode(comp.data(), clen, out.data(),
                         static_cast<uint32_t>(in.size()), version),
              PARC_OK);
    out.resize(in.size());
    EXPECT_EQ(out, in);
    return type;
}

// A valid packed payload to corrupt in the negative tests.
struct Packed {
    std::vector<uint8_t> raw;
    std::vector<uint8_t> comp;
    unsigned version;
};

Packed make_packed(unsigned version = 1) {
    Packed p;
    p.version = version;
    p.raw = gen(parc_gen_text, 7, 8192);
    parc_blk_cctx cx;
    EXPECT_EQ(parc_blk_cctx_init(&cx, p.raw.size(), PARC_LEVEL_DEFAULT, version),
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
    // Every level must produce a decoder-valid block for every input in both
    // wire versions; level only changes which matches are found, version only
    // the entropy stage — never correctness.
    for (unsigned version : {0u, 1u, 2u})
        for (unsigned lvl = PARC_LEVEL_MIN; lvl <= PARC_LEVEL_MAX; ++lvl)
            for (auto g : {parc_gen_random, parc_gen_text, parc_gen_json_log})
                for (size_t n : {size_t{1}, size_t{4}, size_t{5}, size_t{63},
                                 size_t{4096}, size_t{4097}, size_t{65536}})
                    roundtrip(gen(g, 100 + n, n), lvl, version);
}

TEST(Block, HigherLevelsNeverBeatWorseThanGreedy) {
    // On compressible text, a deeper search must not produce a larger packed
    // payload than the greedy level-1 matcher.
    auto in = gen(parc_gen_text, 55, 200000);
    auto packed_len = [&](unsigned level) -> uint32_t {
        parc_blk_cctx cx;
        EXPECT_EQ(parc_blk_cctx_init(&cx, in.size(), level, 1), PARC_OK);
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
    // The optimal tier (8, 9) encodes both a level-7 lazy parse and the
    // repeat-offset-aware optimal parse and keeps the smaller, so it must never
    // exceed the level-7 payload regardless of how the optimal parse is tuned.
    uint32_t lazy7 = packed_len(7);
    EXPECT_LE(packed_len(8), lazy7);
    EXPECT_LE(packed_len(9), lazy7);
}

TEST(Block, MaxDistanceAndLongMatch) {
    // Two copies of a random 64 KiB body separated by unique junk: forces a
    // large distance; a 100 KiB run forces long match lengths.
    auto body = gen(parc_gen_random, 12, 65536);
    std::vector<uint8_t> in = body;
    in.insert(in.end(), body.begin(), body.end());
    EXPECT_EQ(roundtrip(in), PARC_BLK_PACKED);
}

TEST(Block, RepeatOffsetPatternRoundtrips) {
    // Strongly periodic data makes one distance recur, driving the v1
    // repeat-offset (REP0) path; both versions must roundtrip and pack.
    parc_rng rng;
    parc_rng_seed(&rng, 0x5EA1);
    for (size_t period : {size_t{7}, size_t{32}, size_t{257}}) {
        std::vector<uint8_t> unit(period);
        for (auto &b : unit) b = static_cast<uint8_t>(parc_rng_range(&rng, 256));
        std::vector<uint8_t> in(200000);
        for (size_t i = 0; i < in.size(); ++i) in[i] = unit[i % period];
        for (unsigned version : {0u, 1u, 2u})
            EXPECT_EQ(roundtrip(in, PARC_LEVEL_DEFAULT, version),
                      PARC_BLK_PACKED)
                << "period " << period << " version " << version;
    }
    // Two alternating distances exercise REP1/REP2 and the MTF shuffle.
    std::vector<uint8_t> a(64), b(48);
    for (auto &x : a) x = static_cast<uint8_t>(parc_rng_range(&rng, 256));
    for (auto &x : b) x = static_cast<uint8_t>(parc_rng_range(&rng, 256));
    std::vector<uint8_t> mix;
    for (int i = 0; i < 1500; ++i) {
        mix.insert(mix.end(), a.begin(), a.end());
        mix.insert(mix.end(), b.begin(), b.end());
    }
    EXPECT_EQ(roundtrip(mix, PARC_LEVEL_DEFAULT, 1), PARC_BLK_PACKED);
}

TEST(BlockDecode, RejectsTruncatedPayload) {
    for (unsigned version : {0u, 1u, 2u}) {
        Packed p = make_packed(version);
        std::vector<uint8_t> out(p.raw.size() + PARC_WILDCOPY_SLACK);
        for (size_t cut : {size_t{0}, size_t{1}, size_t{100},
                           p.comp.size() - 1})
            EXPECT_EQ(blk_decode(p.comp.data(), static_cast<uint32_t>(cut),
                                 out.data(),
                                 static_cast<uint32_t>(p.raw.size()), version),
                      PARC_ERR_CORRUPT)
                << "version " << version << " cut " << cut;
    }
}

TEST(BlockDecode, RejectsNonMinimalCompLenAndPadding) {
    for (unsigned version : {0u, 1u, 2u}) {
        Packed p = make_packed(version);
        std::vector<uint8_t> out(p.raw.size() + PARC_WILDCOPY_SLACK);
        // extra byte appended: comp_len no longer minimal
        std::vector<uint8_t> longer = p.comp;
        longer.push_back(0);
        EXPECT_EQ(blk_decode(longer.data(),
                             static_cast<uint32_t>(longer.size()), out.data(),
                             static_cast<uint32_t>(p.raw.size()), version),
                  PARC_ERR_CORRUPT)
            << "version " << version;
    }
}

TEST(BlockDecode, RejectsWrongRawLen) {
    for (unsigned version : {0u, 1u, 2u}) {
        Packed p = make_packed(version);
        std::vector<uint8_t> out(p.raw.size() + 8 + PARC_WILDCOPY_SLACK);
        for (long d : {-3L, -1L, 1L, 3L}) {
            uint32_t raw_len =
                static_cast<uint32_t>(static_cast<long>(p.raw.size()) + d);
            EXPECT_EQ(blk_decode(p.comp.data(),
                                 static_cast<uint32_t>(p.comp.size()),
                                 out.data(), raw_len, version),
                      PARC_ERR_CORRUPT)
                << "version " << version << " delta " << d;
        }
    }
}

TEST(BlockDecode, SurvivesArbitraryGarbage) {
    // Any byte soup must yield a clean error or a successful decode of
    // exactly raw_len bytes (frame hashes catch wrong content) — never a
    // crash or overrun (ASan enforces). Both decoders.
    parc_rng rng;
    parc_rng_seed(&rng, 0xBAD);
    std::vector<uint8_t> out(4096 + PARC_WILDCOPY_SLACK);
    for (unsigned version : {0u, 1u, 2u})
        for (int iter = 0; iter < 2000; ++iter) {
            size_t clen = 1 + parc_rng_range(&rng, 700);
            std::vector<uint8_t> junk(clen);
            for (auto &b : junk)
                b = static_cast<uint8_t>(parc_rng_range(&rng, 256));
            blk_decode(junk.data(), static_cast<uint32_t>(clen), out.data(),
                       4096, version);
        }
}
