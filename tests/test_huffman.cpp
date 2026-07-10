#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "codec/huffman.h"
#include "util/bitstream.h"
#include "util/rng.h"

namespace {

// Kraft sum in units of 2^-15; a complete code sums to 1 << 15.
uint32_t kraft(const uint8_t *lens, unsigned n) {
    uint32_t k = 0;
    for (unsigned s = 0; s < n; ++s)
        if (lens[s]) k += 1u << (15 - lens[s]);
    return k;
}

unsigned present(const uint8_t *lens, unsigned n) {
    unsigned c = 0;
    for (unsigned s = 0; s < n; ++s) c += lens[s] != 0;
    return c;
}

// Assert lens is valid per FORMAT.md §2.3 given the frequencies.
void expect_valid_lens(const std::vector<uint32_t> &freq,
                       const uint8_t *lens) {
    unsigned n = static_cast<unsigned>(freq.size());
    unsigned used = 0;
    for (unsigned s = 0; s < n; ++s) {
        EXPECT_LE(lens[s], 15) << "symbol " << s;
        EXPECT_EQ(lens[s] != 0, freq[s] != 0) << "symbol " << s;
        used += freq[s] != 0;
    }
    if (used == 0)
        EXPECT_EQ(kraft(lens, n), 0u);
    else if (used == 1)
        EXPECT_EQ(kraft(lens, n), 1u << 14);  // one symbol, length 1
    else
        EXPECT_EQ(kraft(lens, n), 1u << 15);
}

}  // namespace

TEST(HuffLens, RandomFrequenciesAlwaysValid) {
    parc_rng rng;
    parc_rng_seed(&rng, 0x4855FF);
    for (int iter = 0; iter < 300; ++iter) {
        unsigned n = static_cast<unsigned>(
            parc_rng_range(&rng, PARC_HUFF_MAX_SYMS) + 1);
        std::vector<uint32_t> freq(n);
        // Mix of zeros, small counts, and heavy skew.
        for (auto &f : freq) {
            uint64_t kind = parc_rng_range(&rng, 4);
            if (kind == 0)
                f = 0;
            else if (kind == 1)
                f = static_cast<uint32_t>(parc_rng_range(&rng, 5));
            else
                f = static_cast<uint32_t>(
                    parc_rng_range(&rng, 1u << 24) + 1);
        }
        uint8_t lens[PARC_HUFF_MAX_SYMS];
        parc_huff_lens(freq.data(), n, lens);
        expect_valid_lens(freq, lens);
    }
}

TEST(HuffLens, FibonacciFrequenciesHitDepthLimit) {
    // Fibonacci weights maximize Huffman depth: 40 symbols give an
    // unlimited-depth tree deeper than 15, forcing the limiter.
    std::vector<uint32_t> freq(40);
    uint32_t a = 1, b = 1;
    for (auto &f : freq) {
        f = a;
        uint32_t next = a + b;
        a = b;
        b = next;
    }
    uint8_t lens[64];
    parc_huff_lens(freq.data(), 40, lens);
    expect_valid_lens(freq, lens);
    unsigned maxlen = 0;
    for (int s = 0; s < 40; ++s) maxlen = std::max<unsigned>(maxlen, lens[s]);
    EXPECT_EQ(maxlen, 15u);
}

TEST(HuffLens, EmptyDegenerateAndPair) {
    uint32_t freq[4] = {0, 0, 0, 0};
    uint8_t lens[4];
    parc_huff_lens(freq, 4, lens);
    EXPECT_EQ(present(lens, 4), 0u);

    freq[2] = 7;
    parc_huff_lens(freq, 4, lens);
    EXPECT_EQ(lens[2], 1);  // degenerate: single symbol, length 1
    EXPECT_EQ(present(lens, 4), 1u);

    freq[0] = 1000000;  // wild skew still yields 1/1 for two symbols
    parc_huff_lens(freq, 4, lens);
    EXPECT_EQ(lens[0], 1);
    EXPECT_EQ(lens[2], 1);
}

TEST(HuffLens, OptimalForKnownDistribution) {
    // freqs 8,4,2,1,1 have unique optimal lengths 1,2,3,4,4.
    uint32_t freq[5] = {8, 4, 2, 1, 1};
    uint8_t lens[5];
    parc_huff_lens(freq, 5, lens);
    EXPECT_EQ(lens[0], 1);
    EXPECT_EQ(lens[1], 2);
    EXPECT_EQ(lens[2], 3);
    EXPECT_EQ(lens[3], 4);
    EXPECT_EQ(lens[4], 4);
}

TEST(HuffCodes, CanonicalAssignmentPinned) {
    // lens {2,1,3,3}: canonical codes sym1=0, sym0=10, sym2=110, sym3=111.
    // parc_henc stores them bit-reversed for the LSB-first writer.
    const uint8_t lens[4] = {2, 1, 3, 3};
    parc_henc e;
    parc_henc_init(&e, lens, 4);
    EXPECT_EQ(e.code[1], 0b0);
    EXPECT_EQ(e.code[0], 0b01);   // 10 reversed
    EXPECT_EQ(e.code[2], 0b011);  // 110 reversed
    EXPECT_EQ(e.code[3], 0b111);  // 111 reversed
}

TEST(HuffDec, RejectsInvalidTables) {
    parc_hdec d;
    {  // oversubscribed: three codes of length 1
        const uint8_t lens[3] = {1, 1, 1};
        EXPECT_EQ(parc_hdec_init(&d, lens, 3), PARC_ERR_CORRUPT);
    }
    {  // undersubscribed: 3 * 2^-2 < 1
        const uint8_t lens[3] = {2, 2, 2};
        EXPECT_EQ(parc_hdec_init(&d, lens, 3), PARC_ERR_CORRUPT);
    }
    {  // degenerate table must use length 1
        const uint8_t lens[3] = {0, 2, 0};
        EXPECT_EQ(parc_hdec_init(&d, lens, 3), PARC_ERR_CORRUPT);
    }
    {  // empty is fine (distance alphabet with no matches)
        const uint8_t lens[3] = {0, 0, 0};
        EXPECT_EQ(parc_hdec_init(&d, lens, 3), PARC_OK);
        EXPECT_EQ(d.nsyms, 0);
    }
    {  // complete pair is fine
        const uint8_t lens[2] = {1, 1};
        EXPECT_EQ(parc_hdec_init(&d, lens, 2), PARC_OK);
    }
}

TEST(HuffDec, DegenerateCodeRejectsOneBit) {
    const uint8_t lens[3] = {0, 1, 0};
    parc_hdec d;
    ASSERT_EQ(parc_hdec_init(&d, lens, 3), PARC_OK);

    uint8_t buf[1] = {0x02};  // bits: 0, 1
    parc_br r;
    parc_br_init(&r, buf, 1);
    EXPECT_EQ(parc_hdec_get(&d, &r), 1);   // 0 bit decodes the symbol
    EXPECT_EQ(parc_hdec_get(&d, &r), -1);  // 1 bit matches no code
}

TEST(Huff, EncodeDecodeRoundtrip) {
    parc_rng rng;
    parc_rng_seed(&rng, 0xC0DEC);
    for (int iter = 0; iter < 50; ++iter) {
        unsigned n = static_cast<unsigned>(
            parc_rng_range(&rng, PARC_HUFF_MAX_SYMS - 2) + 2);
        // Zipf-ish frequencies over a random subset.
        std::vector<uint32_t> freq(n, 0);
        for (int i = 0; i < 2000; ++i) {
            uint64_t s = parc_rng_range(&rng, n);
            freq[s / (1 + s % 3)]++;  // skew towards low symbols
        }
        std::vector<uint16_t> msg;
        for (unsigned s = 0; s < n; ++s)
            for (uint32_t c = 0; c < freq[s] % 17; ++c)
                msg.push_back(static_cast<uint16_t>(s));
        for (auto m : msg) freq[m]++;  // ensure every emitted sym has freq

        uint8_t lens[PARC_HUFF_MAX_SYMS];
        parc_huff_lens(freq.data(), n, lens);
        parc_henc e;
        parc_henc_init(&e, lens, n);
        parc_hdec d;
        ASSERT_EQ(parc_hdec_init(&d, lens, n), PARC_OK);

        std::vector<uint8_t> bits(2 * msg.size() + 16);
        parc_bw w;
        parc_bw_init(&w, bits.data(), bits.size());
        for (auto m : msg) parc_henc_put(&e, &w, m);
        size_t nbytes = 0;
        ASSERT_EQ(parc_bw_finish(&w, &nbytes), PARC_OK);

        parc_br r;
        parc_br_init(&r, bits.data(), nbytes);
        for (size_t i = 0; i < msg.size(); ++i)
            ASSERT_EQ(parc_hdec_get(&d, &r), msg[i]) << "token " << i;
        EXPECT_EQ(parc_br_err(&r), PARC_OK);
    }
}
