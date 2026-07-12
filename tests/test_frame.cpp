#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "parc/parc.h"
#include "gen.h"
#include "util/buf.h"
#include "util/rng.h"

namespace {

FILE *file_of(const std::vector<uint8_t> &data) {
    FILE *f = tmpfile();
    EXPECT_NE(f, nullptr);
    if (!data.empty()) {
        EXPECT_EQ(fwrite(data.data(), 1, data.size(), f), data.size());
    }
    rewind(f);
    return f;
}

std::vector<uint8_t> slurp(FILE *f) {
    std::vector<uint8_t> v;
    rewind(f);
    uint8_t buf[4096];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0)
        v.insert(v.end(), buf, buf + got);
    return v;
}

std::vector<uint8_t> compress(const std::vector<uint8_t> &in,
                              unsigned block_log, parc_info *fi = nullptr,
                              unsigned format = PARC_FORMAT_DEFAULT) {
    FILE *fin = file_of(in);
    FILE *fout = tmpfile();
    parc_copts opts = {block_log, 0, 0, format};
    EXPECT_EQ(parc_compress_stream(fin, fout, &opts, fi), PARC_OK);
    auto frame = slurp(fout);
    fclose(fin);
    fclose(fout);
    return frame;
}

// out == nullptr verifies without capturing output.
parc_err decompress(const std::vector<uint8_t> &frame,
                    std::vector<uint8_t> *out, parc_info *fi = nullptr) {
    FILE *fin = file_of(frame);
    FILE *fout = out ? tmpfile() : nullptr;
    parc_err err = parc_decompress_stream(fin, fout, nullptr, fi);
    if (out)
        *out = slurp(fout);
    fclose(fin);
    if (fout)
        fclose(fout);
    return err;
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

// A small frame mixing packed and stored blocks, for the corruption suite:
// text (packs) followed by random (stored) at 4 KiB blocks.
std::vector<uint8_t> mixed_frame(std::vector<uint8_t> *content,
                                 unsigned format = PARC_FORMAT_V1) {
    auto text = gen(parc_gen_text, 21, 6000);
    auto rand = gen(parc_gen_random, 22, 3000);
    content->assign(text.begin(), text.end());
    content->insert(content->end(), rand.begin(), rand.end());
    parc_info fi;
    auto frame = compress(*content, 12, &fi, format);
    EXPECT_EQ(fi.blocks, 3u);
    EXPECT_GE(fi.stored_blocks, 1u);
    EXPECT_LE(fi.stored_blocks, 2u);
    return frame;
}

}  // namespace

TEST(Frame, PropertyRoundtripAllClassesAndBoundarySizes) {
    struct Gen {
        const char *name;
        parc_err (*fn)(parc_rng *, size_t, parc_buf *);
    };
    const Gen gens[] = {
        {"random", parc_gen_random},  {"text", parc_gen_text},
        {"json-log", parc_gen_json_log},
    };
    // block_log 12 → 4096-byte blocks; sizes straddle every boundary
    const size_t sizes[] = {0,    1,    2,    3,    4,    5,     7,
                            4095, 4096, 4097, 8191, 8192, 8193,  100000};
    for (const auto &g : gens) {
        for (size_t n : sizes) {
            auto in = gen(g.fn, 0xF00D + n, n);
            parc_info ci, di;
            auto frame = compress(in, 12, &ci);
            EXPECT_EQ(ci.raw_bytes, n) << g.name << " n=" << n;
            EXPECT_EQ(ci.blocks, (n + 4095) / 4096) << g.name << " n=" << n;
            EXPECT_EQ(ci.frame_bytes, frame.size()) << g.name << " n=" << n;

            std::vector<uint8_t> out;
            ASSERT_EQ(decompress(frame, &out, &di), PARC_OK)
                << g.name << " n=" << n;
            EXPECT_EQ(out, in) << g.name << " n=" << n;
            EXPECT_EQ(di.raw_bytes, ci.raw_bytes);
            EXPECT_EQ(di.frame_bytes, ci.frame_bytes);
            EXPECT_EQ(di.blocks, ci.blocks);
            EXPECT_EQ(di.stored_blocks, ci.stored_blocks);

            // verify-only path
            EXPECT_EQ(decompress(frame, nullptr), PARC_OK)
                << g.name << " n=" << n;
        }
    }
}

TEST(Frame, MultiBlockAtDefaultBlockSize) {
    auto in = gen(parc_gen_json_log, 33, (3u << 20) + 12345);
    parc_info fi;
    auto frame = compress(in, 0, &fi);  // 0 → default block_log 20
    EXPECT_EQ(fi.blocks, 4u);
    std::vector<uint8_t> out;
    ASSERT_EQ(decompress(frame, &out), PARC_OK);
    EXPECT_EQ(out, in);
}

// The default block size (the match window) scales with level: levels 1-3
// keep block_log 20 (1 MiB), levels 4-9 widen to 22 (4 MiB). The same ~3.14
// MiB input that spans 4 blocks at the low default fits in 1 at the high one.
TEST(Frame, DefaultBlockSizeWidensWithLevel) {
    auto in = gen(parc_gen_json_log, 33, (3u << 20) + 12345);
    // Exercise the exact policy boundary (last low level, first high level).
    for (auto [level, want_blocks] : {std::pair{3u, 4u}, {4u, 1u}}) {
        FILE *fin = file_of(in);
        FILE *fout = tmpfile();
        parc_copts opts = {0, 0, level, PARC_FORMAT_DEFAULT};  // block_log 0 = default
        parc_info fi;
        ASSERT_EQ(parc_compress_stream(fin, fout, &opts, &fi), PARC_OK)
            << "level " << level;
        EXPECT_EQ(fi.blocks, want_blocks) << "level " << level;
        auto frame = slurp(fout);
        fclose(fin);
        fclose(fout);
        std::vector<uint8_t> out;
        ASSERT_EQ(decompress(frame, &out), PARC_OK) << "level " << level;
        EXPECT_EQ(out, in) << "level " << level;
    }
}

TEST(Frame, EmptyInput) {
    parc_info ci, di;
    auto frame = compress({}, 12, &ci);
    EXPECT_EQ(ci.raw_bytes, 0u);
    EXPECT_EQ(ci.blocks, 0u);
    EXPECT_EQ(frame.size(), 37u);  // 8 header + 29 trailer, zero blocks

    std::vector<uint8_t> out{1, 2, 3};
    ASSERT_EQ(decompress(frame, &out, &di), PARC_OK);
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(di.raw_bytes, 0u);
}

TEST(Frame, BadOptionsRejected) {
    FILE *fin = file_of({});
    FILE *fout = tmpfile();
    for (unsigned bl : {11u, 25u, 99u}) {
        parc_copts opts = {bl, 0, 0, PARC_FORMAT_DEFAULT};
        EXPECT_EQ(parc_compress_stream(fin, fout, &opts, nullptr),
                  PARC_ERR_ARG)
            << bl;
    }
    fclose(fin);
    fclose(fout);
}

TEST(Frame, HeaderFieldValidation) {
    std::vector<uint8_t> content;
    const auto frame = mixed_frame(&content);

    auto expect_hdr_err = [&](size_t pos, uint8_t val, parc_err want) {
        auto bad = frame;
        bad[pos] = val;
        EXPECT_EQ(decompress(bad, nullptr), want)
            << "byte " << pos << " = " << int{val};
    };
    expect_hdr_err(0, 'q', PARC_ERR_CORRUPT);   // magic
    expect_hdr_err(4, 2, PARC_ERR_VERSION);     // future version (0 and 1 valid)
    expect_hdr_err(5, 1, PARC_ERR_VERSION);     // unknown flag
    expect_hdr_err(6, 11, PARC_ERR_CORRUPT);    // block_log too small
    expect_hdr_err(6, 25, PARC_ERR_CORRUPT);    // block_log too large
    expect_hdr_err(7, 1, PARC_ERR_CORRUPT);     // reserved
}

TEST(Frame, ErrorTaxonomyExamples) {
    std::vector<uint8_t> content;
    const auto frame = mixed_frame(&content);

    // flip a bit inside the stream hash (frame end: total 8, hash 8,
    // trailer_len 4, magic 4) → everything is structurally fine but the
    // content hash mismatches
    auto bad = frame;
    bad[frame.size() - 13] ^= 0x10;
    EXPECT_EQ(decompress(bad, nullptr), PARC_ERR_CHECKSUM);

    // flip a byte of a stored block's payload → per-block hash catches it
    // (block 2 is random data ⇒ stored; but flipping any payload byte of
    // any block type must yield CHECKSUM or CORRUPT, so flip in block 1)
    bad = frame;
    bad[8 + 17 + 5] ^= 0xFF;
    parc_err e = decompress(bad, nullptr);
    EXPECT_TRUE(e == PARC_ERR_CHECKSUM || e == PARC_ERR_CORRUPT)
        << parc_err_str(e);

    // cut mid-trailer → truncated
    bad = frame;
    bad.resize(frame.size() - 10);
    EXPECT_EQ(decompress(bad, nullptr), PARC_ERR_TRUNCATED);

    // trailing garbage after the footer
    bad = frame;
    bad.push_back(0);
    EXPECT_EQ(decompress(bad, nullptr), PARC_ERR_CORRUPT);

    // two concatenated frames: v0 rejects concatenation
    bad = frame;
    bad.insert(bad.end(), frame.begin(), frame.end());
    EXPECT_EQ(decompress(bad, nullptr), PARC_ERR_CORRUPT);
}

TEST(Frame, EveryTruncationFailsCleanly) {
    for (unsigned fmt : {PARC_FORMAT_V0, PARC_FORMAT_V1}) {
        std::vector<uint8_t> content;
        const auto frame = mixed_frame(&content, fmt);
        for (size_t n = 0; n < frame.size(); ++n) {
            std::vector<uint8_t> cut(frame.begin(), frame.begin() + (long)n);
            parc_err e = decompress(cut, nullptr);
            ASSERT_NE(e, PARC_OK)
                << "fmt " << fmt << " prefix of " << n << " bytes decoded";
        }
        // the empty frame too
        const auto empty = compress({}, 12, nullptr, fmt);
        for (size_t n = 0; n < empty.size(); ++n) {
            std::vector<uint8_t> cut(empty.begin(), empty.begin() + (long)n);
            ASSERT_NE(decompress(cut, nullptr), PARC_OK) << "fmt " << fmt << n;
        }
    }
}

TEST(Frame, EveryByteMutationFailsOrDecodesIdentically) {
    for (unsigned fmt : {PARC_FORMAT_V0, PARC_FORMAT_V1}) {
        std::vector<uint8_t> content;
        const auto frame = mixed_frame(&content, fmt);
        for (size_t pos = 0; pos < frame.size(); ++pos) {
            for (uint8_t flip : {uint8_t{0x01}, uint8_t{0xFF}}) {
                auto bad = frame;
                bad[pos] ^= flip;
                std::vector<uint8_t> out;
                parc_err e = decompress(bad, &out);
                if (e == PARC_OK) {
                    ASSERT_EQ(out, content)
                        << "fmt " << fmt << " byte " << pos << " ^ "
                        << int{flip} << " decoded to different content";
                }
            }
        }
    }
}
