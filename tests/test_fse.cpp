#include <cstdint>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "codec/fse.h"
#include "util/bitstream.h"
#include "util/rng.h"

namespace {

// Build a frequency table from a symbol message.
std::vector<uint32_t> histogram(const std::vector<uint8_t> &msg,
                                unsigned max_symbol) {
    std::vector<uint32_t> freq(max_symbol + 1, 0);
    for (auto s : msg) freq[s]++;
    return freq;
}

// Round-trip a symbol message through table build + encode + decode and
// assert exact recovery. Uses freq to size/normalize the table.
void expect_roundtrip(const std::vector<uint8_t> &msg,
                      const std::vector<uint32_t> &freq, unsigned nominal_max) {
    // max_symbol is by contract the highest *present* symbol.
    unsigned max_symbol = 0;
    for (unsigned s = 0; s <= nominal_max; ++s)
        if (freq[s]) max_symbol = s;
    unsigned tl = parc_fse_tablelog(std::max<size_t>(msg.size(), 1), max_symbol);
    std::vector<int16_t> norm(max_symbol + 1, 0);
    ASSERT_EQ(parc_fse_normalize(freq.data(), max_symbol, tl, norm.data()),
              PARC_OK);

    // normalization invariants
    uint32_t sum = 0;
    for (unsigned s = 0; s <= max_symbol; ++s) {
        EXPECT_EQ(norm[s] != 0, freq[s] != 0) << "symbol " << s;
        EXPECT_GE(norm[s], 0);
        sum += static_cast<uint32_t>(norm[s]);
    }
    EXPECT_EQ(sum, 1u << tl);

    auto enc = std::make_unique<parc_fenc>();
    parc_fenc_build(enc.get(), norm.data(), max_symbol, tl);

    std::vector<uint8_t> buf(msg.size() * 3 + 64);
    std::vector<uint32_t> grp(std::max<size_t>(msg.size(), 1));
    parc_bw w;
    parc_bw_init(&w, buf.data(), buf.size());
    parc_fse_write_table(&w, norm.data(), max_symbol, tl);
    ASSERT_EQ(parc_fse_encode(&w, enc.get(), msg.data(), msg.size(), grp.data()),
              PARC_OK);
    size_t nbytes = 0;
    ASSERT_EQ(parc_bw_finish(&w, &nbytes), PARC_OK);

    parc_br r;
    parc_br_init(&r, buf.data(), nbytes);
    int16_t rnorm[PARC_FSE_MAX_SYMS];
    unsigned rms = 0, rtl = 0;
    ASSERT_EQ(parc_fse_read_table(&r, rnorm, &rms, &rtl, PARC_FSE_MAX_SYMS - 1),
              PARC_OK);
    EXPECT_EQ(rtl, tl);
    // read-back table matches (up to the written max_symbol)
    for (unsigned s = 0; s <= std::max(rms, max_symbol); ++s)
        EXPECT_EQ(rnorm[s], norm[s]) << "symbol " << s;

    auto dec = std::make_unique<parc_fdec>();
    ASSERT_EQ(parc_fdec_build(dec.get(), rnorm, rms, rtl), PARC_OK);
    std::vector<uint8_t> out(msg.size());
    ASSERT_EQ(parc_fse_decode(&r, dec.get(), out.data(), out.size()), PARC_OK);
    EXPECT_EQ(out, msg);
    EXPECT_EQ(parc_br_err(&r), PARC_OK);
}

}  // namespace

TEST(FseNormalize, SumsToTableSizeAndKeepsSupport) {
    parc_rng rng;
    parc_rng_seed(&rng, 0xF5E01);
    for (int iter = 0; iter < 300; ++iter) {
        unsigned max_symbol =
            static_cast<unsigned>(parc_rng_range(&rng, 256));
        std::vector<uint32_t> freq(max_symbol + 1, 0);
        bool any = false;
        for (auto &f : freq) {
            uint64_t kind = parc_rng_range(&rng, 4);
            if (kind == 0)
                f = 0;
            else
                f = static_cast<uint32_t>(parc_rng_range(&rng, 1u << 20) + 1);
            any = any || f != 0;
        }
        if (!any) freq[0] = 1;
        unsigned tl = parc_fse_tablelog(1u << 16, max_symbol);
        std::vector<int16_t> norm(max_symbol + 1, 0);
        ASSERT_EQ(parc_fse_normalize(freq.data(), max_symbol, tl, norm.data()),
                  PARC_OK);
        uint32_t sum = 0;
        for (unsigned s = 0; s <= max_symbol; ++s) {
            EXPECT_EQ(norm[s] != 0, freq[s] != 0) << "iter " << iter << " s " << s;
            sum += static_cast<uint32_t>(norm[s]);
        }
        EXPECT_EQ(sum, 1u << tl) << "iter " << iter;
    }
}

TEST(FseTable, WriteReadRoundtrip) {
    const uint32_t freq[6] = {100, 50, 25, 12, 6, 3};
    unsigned max_symbol = 5;
    unsigned tl = parc_fse_tablelog(196, max_symbol);
    int16_t norm[6];
    ASSERT_EQ(parc_fse_normalize(freq, max_symbol, tl, norm), PARC_OK);

    uint8_t buf[64];
    parc_bw w;
    parc_bw_init(&w, buf, sizeof buf);
    parc_fse_write_table(&w, norm, max_symbol, tl);
    size_t nbytes = 0;
    ASSERT_EQ(parc_bw_finish(&w, &nbytes), PARC_OK);

    parc_br r;
    parc_br_init(&r, buf, nbytes);
    int16_t rnorm[PARC_FSE_MAX_SYMS];
    unsigned rms = 0, rtl = 0;
    ASSERT_EQ(parc_fse_read_table(&r, rnorm, &rms, &rtl, PARC_FSE_MAX_SYMS - 1),
              PARC_OK);
    EXPECT_EQ(rms, max_symbol);
    EXPECT_EQ(rtl, tl);
    for (unsigned s = 0; s <= max_symbol; ++s) EXPECT_EQ(rnorm[s], norm[s]);
}

TEST(FseDec, RejectsMalformedTable) {
    // counts that do not sum to 1 << table_log
    int16_t norm[3] = {1, 1, 1};  // sums to 3, table_size is 32
    auto dec = std::make_unique<parc_fdec>();
    EXPECT_EQ(parc_fdec_build(dec.get(), norm, 2, 5), PARC_ERR_CORRUPT);

    // a well-formed table for the same alphabet builds fine
    int16_t good[3] = {16, 8, 8};  // sums to 32 = 1 << 5
    EXPECT_EQ(parc_fdec_build(dec.get(), good, 2, 5), PARC_OK);
}

TEST(FseReadTable, RejectsBadSumFromStream) {
    // Hand-craft a table stream with a deliberately wrong count sum.
    uint8_t buf[64];
    parc_bw w;
    parc_bw_init(&w, buf, sizeof buf);
    unsigned tl = 5, max_symbol = 1;
    parc_bw_put(&w, tl, 4);
    parc_bw_put(&w, max_symbol, 8);
    // bucket-coded counts of 3 (bucket 2 in 4 bits, then 1 mantissa bit);
    // sum 6 != 32
    parc_bw_put(&w, 2, 4);
    parc_bw_put(&w, 1, 1);
    parc_bw_put(&w, 2, 4);
    parc_bw_put(&w, 1, 1);
    size_t nbytes = 0;
    ASSERT_EQ(parc_bw_finish(&w, &nbytes), PARC_OK);

    parc_br r;
    parc_br_init(&r, buf, nbytes);
    int16_t norm[PARC_FSE_MAX_SYMS];
    unsigned rms = 0, rtl = 0;
    EXPECT_EQ(parc_fse_read_table(&r, norm, &rms, &rtl, PARC_FSE_MAX_SYMS - 1),
              PARC_ERR_CORRUPT);
}

TEST(Fse, EncodeDecodeRoundtripRandom) {
    parc_rng rng;
    parc_rng_seed(&rng, 0x0A5C0DE);
    for (int iter = 0; iter < 80; ++iter) {
        unsigned max_symbol =
            static_cast<unsigned>(parc_rng_range(&rng, 255) + 1);
        size_t count = static_cast<size_t>(parc_rng_range(&rng, 5000) + 1);
        std::vector<uint8_t> msg(count);
        // skew towards low symbols so distributions are non-uniform
        for (auto &m : msg) {
            uint64_t s = parc_rng_range(&rng, max_symbol + 1);
            s = s / (1 + s % 3);
            m = static_cast<uint8_t>(s);
        }
        auto freq = histogram(msg, max_symbol);
        expect_roundtrip(msg, freq, max_symbol);
    }
}

TEST(Fse, SkewedStreamCompresses) {
    // A stream dominated by one symbol must code well below 8 bits/symbol:
    // FSE gives fractional bits, so a ~99% symbol costs far less than 1 bit.
    const unsigned max_symbol = 3;
    std::vector<uint8_t> msg(20000);
    parc_rng rng;
    parc_rng_seed(&rng, 0x5EED);
    for (auto &m : msg)
        m = static_cast<uint8_t>(parc_rng_range(&rng, 100) < 99
                                      ? 0
                                      : 1 + parc_rng_range(&rng, 3));
    auto freq = histogram(msg, max_symbol);
    unsigned tl = parc_fse_tablelog(msg.size(), max_symbol);
    int16_t norm[max_symbol + 1];
    ASSERT_EQ(parc_fse_normalize(freq.data(), max_symbol, tl, norm), PARC_OK);
    auto enc = std::make_unique<parc_fenc>();
    parc_fenc_build(enc.get(), norm, max_symbol, tl);

    std::vector<uint8_t> buf(msg.size() + 256);
    std::vector<uint32_t> grp(msg.size());
    parc_bw w;
    parc_bw_init(&w, buf.data(), buf.size());
    ASSERT_EQ(parc_fse_encode(&w, enc.get(), msg.data(), msg.size(), grp.data()),
              PARC_OK);
    size_t nbytes = 0;
    ASSERT_EQ(parc_bw_finish(&w, &nbytes), PARC_OK);
    // 2 bits/symbol naive; a 99%-skewed source is well under 0.25 bytes/sym.
    EXPECT_LT(nbytes, msg.size() / 4) << "FSE failed to exploit the skew";
}

TEST(Fse, EdgeCounts) {
    // single distinct symbol (norm gets the whole table)
    {
        std::vector<uint8_t> msg(1000, 7);
        expect_roundtrip(msg, histogram(msg, 7), 7);
    }
    // one symbol total
    {
        std::vector<uint8_t> msg(1, 3);
        expect_roundtrip(msg, histogram(msg, 3), 3);
    }
    // empty stream: encode/decode are no-ops
    {
        std::vector<uint8_t> msg;
        uint32_t freq[1] = {1};
        unsigned tl = parc_fse_tablelog(1, 0);
        int16_t norm[1];
        ASSERT_EQ(parc_fse_normalize(freq, 0, tl, norm), PARC_OK);
        auto enc = std::make_unique<parc_fenc>();
        parc_fenc_build(enc.get(), norm, 0, tl);
        uint8_t buf[16];
        parc_bw w;
        parc_bw_init(&w, buf, sizeof buf);
        EXPECT_EQ(parc_fse_encode(&w, enc.get(), nullptr, 0, nullptr), PARC_OK);
    }
    // uniform over the full byte alphabet
    {
        std::vector<uint8_t> msg(4096);
        for (size_t i = 0; i < msg.size(); ++i)
            msg[i] = static_cast<uint8_t>(i & 0xFF);
        expect_roundtrip(msg, histogram(msg, 255), 255);
    }
}
