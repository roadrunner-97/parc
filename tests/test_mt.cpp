// Phase 4 multithreading suite. The current MT pipeline compresses fixed
// block boundaries in parallel, so frames are bit-identical to the
// single-threaded reference for every thread count — asserted here both
// because FORMAT.md-level determinism is worth keeping and because any
// ordering race shows up as a frame diff long before TSan luck runs out.
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "parc/parc.h"
#include "gen.h"
#include "util/buf.h"
#include "util/rng.h"

namespace {

unsigned hw_threads() {
    unsigned n = std::thread::hardware_concurrency();
    return n ? n : 4;
}

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
                              unsigned block_log, unsigned threads,
                              parc_info *fi = nullptr,
                              unsigned format = PARC_FORMAT_DEFAULT) {
    FILE *fin = file_of(in);
    FILE *fout = tmpfile();
    parc_copts opts = {block_log, threads, 0, format};
    EXPECT_EQ(parc_compress_stream(fin, fout, &opts, fi), PARC_OK);
    auto frame = slurp(fout);
    fclose(fin);
    fclose(fout);
    return frame;
}

// out == nullptr verifies without capturing output.
parc_err decompress(const std::vector<uint8_t> &frame,
                    std::vector<uint8_t> *out, unsigned threads,
                    parc_info *fi = nullptr) {
    FILE *fin = file_of(frame);
    FILE *fout = out ? tmpfile() : nullptr;
    parc_dopts opts = {threads};
    parc_err err = parc_decompress_stream(fin, fout, &opts, fi);
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

void expect_roundtrip(const std::vector<uint8_t> &in, unsigned block_log,
                      unsigned cthreads, unsigned dthreads,
                      unsigned format = PARC_FORMAT_DEFAULT) {
    parc_info ci, di;
    auto st = compress(in, block_log, 1, &ci, format);
    auto mt = compress(in, block_log, cthreads, &di, format);
    ASSERT_EQ(mt, st) << "frame differs, T=" << cthreads;
    EXPECT_EQ(di.raw_bytes, ci.raw_bytes);
    EXPECT_EQ(di.frame_bytes, ci.frame_bytes);
    EXPECT_EQ(di.blocks, ci.blocks);
    EXPECT_EQ(di.stored_blocks, ci.stored_blocks);

    std::vector<uint8_t> out;
    ASSERT_EQ(decompress(mt, &out, dthreads, &di), PARC_OK);
    EXPECT_EQ(out, in) << "content differs, T=" << dthreads;
    EXPECT_EQ(di.raw_bytes, ci.raw_bytes);
    EXPECT_EQ(di.frame_bytes, ci.frame_bytes);
    EXPECT_EQ(di.blocks, ci.blocks);
    EXPECT_EQ(di.stored_blocks, ci.stored_blocks);

    // verify-only path
    EXPECT_EQ(decompress(mt, nullptr, dthreads), PARC_OK);
}

}  // namespace

TEST(Mt, RoundtripAllClassesAndBoundarySizes) {
    struct Gen {
        const char *name;
        parc_err (*fn)(parc_rng *, size_t, parc_buf *);
    };
    const Gen gens[] = {
        {"random", parc_gen_random},  {"text", parc_gen_text},
        {"json-log", parc_gen_json_log},
    };
    // block_log 12 → 4096-byte blocks; sizes straddle every boundary
    const size_t sizes[] = {0, 1, 4095, 4096, 4097, 8191, 8192, 8193, 100000};
    for (const auto &g : gens) {
        for (size_t n : sizes) {
            auto in = gen(g.fn, 0xBEEF + n, n);
            expect_roundtrip(in, 12, 2, 2);
        }
    }
}

TEST(Mt, V0FrameBitIdentical) {
    // The legacy v0 format must still round-trip bit-identically across
    // thread counts (default is now v1, so v0 needs its own coverage).
    const unsigned hw = hw_threads();
    for (size_t n : {size_t{0}, size_t{1}, size_t{8193}, size_t{100000}})
        for (unsigned t : {2u, hw}) {
            auto in = gen(parc_gen_text, 0xC0DE + n, n);
            expect_roundtrip(in, 12, t, t, PARC_FORMAT_V0);
        }
}

TEST(Mt, ThreadCountSweep) {
    const unsigned hw = hw_threads();
    auto in = gen(parc_gen_json_log, 77, 300000);  // 74 blocks at 2^12
    for (unsigned t : {2u, 3u, hw, 2 * hw}) {
        expect_roundtrip(in, 12, t, t);
        // cross thread counts: MT-compressed, differently-MT-decompressed
        auto frame = compress(in, 12, t);
        std::vector<uint8_t> out;
        ASSERT_EQ(decompress(frame, &out, 2 * hw + 1 - t), PARC_OK) << t;
        EXPECT_EQ(out, in) << t;
    }
}

TEST(Mt, ManyBlocksStress) {
    // ~1536 blocks of 4 KiB across all entropy classes glued together
    auto in = gen(parc_gen_json_log, 91, 3u << 20);
    auto rnd = gen(parc_gen_random, 92, 2u << 20);
    auto txt = gen(parc_gen_text, 93, 1u << 20);
    in.insert(in.end(), rnd.begin(), rnd.end());
    in.insert(in.end(), txt.begin(), txt.end());
    expect_roundtrip(in, 12, hw_threads(), hw_threads());
}

TEST(Mt, ThreadsExceedBlocks) {
    auto in = gen(parc_gen_text, 55, 2 * 4096 + 17);  // 3 blocks
    expect_roundtrip(in, 12, 16, 16);
    expect_roundtrip(gen(parc_gen_text, 56, 1), 12, 16, 16);
}

TEST(Mt, EmptyInput) {
    parc_info ci, di;
    auto frame = compress({}, 12, 8, &ci);
    EXPECT_EQ(ci.raw_bytes, 0u);
    EXPECT_EQ(ci.blocks, 0u);
    EXPECT_EQ(frame.size(), 37u);  // 8 header + 29 trailer, zero blocks

    std::vector<uint8_t> out{1, 2, 3};
    ASSERT_EQ(decompress(frame, &out, 8, &di), PARC_OK);
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(di.raw_bytes, 0u);
}

TEST(Mt, BadThreadCountRejected) {
    FILE *fin = file_of({});
    FILE *fout = tmpfile();
    parc_copts copts = {12, PARC_THREADS_MAX + 1, 0, PARC_FORMAT_DEFAULT};
    EXPECT_EQ(parc_compress_stream(fin, fout, &copts, nullptr), PARC_ERR_ARG);
    rewind(fin);
    parc_dopts dopts = {PARC_THREADS_MAX + 1};
    EXPECT_EQ(parc_decompress_stream(fin, fout, &dopts, nullptr),
              PARC_ERR_ARG);
    fclose(fin);
    fclose(fout);
}

TEST(Mt, ErrorTaxonomyExamples) {
    auto text = gen(parc_gen_text, 21, 6000);
    auto rand = gen(parc_gen_random, 22, 3000);
    std::vector<uint8_t> content = text;
    content.insert(content.end(), rand.begin(), rand.end());
    const auto frame = compress(content, 12, 2);  // packed + stored blocks

    // stream-hash flip: structure fine, content hash mismatches
    auto bad = frame;
    bad[frame.size() - 13] ^= 0x10;
    EXPECT_EQ(decompress(bad, nullptr, 4), PARC_ERR_CHECKSUM);

    // payload byte flip inside block 1
    bad = frame;
    bad[8 + 17 + 5] ^= 0xFF;
    parc_err e = decompress(bad, nullptr, 4);
    EXPECT_TRUE(e == PARC_ERR_CHECKSUM || e == PARC_ERR_CORRUPT)
        << parc_err_str(e);

    // cut mid-trailer
    bad = frame;
    bad.resize(frame.size() - 10);
    EXPECT_EQ(decompress(bad, nullptr, 4), PARC_ERR_TRUNCATED);

    // trailing garbage after the footer
    bad = frame;
    bad.push_back(0);
    EXPECT_EQ(decompress(bad, nullptr, 4), PARC_ERR_CORRUPT);
}

TEST(Mt, EveryTruncationFailsCleanly) {
    // small two-block frame so the full sweep stays fast under TSan
    auto in = gen(parc_gen_text, 31, 4096 + 700);
    const auto frame = compress(in, 12, 2);
    for (size_t n = 0; n < frame.size(); ++n) {
        std::vector<uint8_t> cut(frame.begin(), frame.begin() + (long)n);
        ASSERT_NE(decompress(cut, nullptr, 3), PARC_OK)
            << "prefix of " << n << " bytes decoded";
    }
}

TEST(Mt, ByteMutationSampleFailsOrDecodesIdentically) {
    auto in = gen(parc_gen_text, 32, 4096 + 700);
    const auto frame = compress(in, 12, 2);
    for (size_t pos = 0; pos < frame.size(); pos += 7) {
        auto bad = frame;
        bad[pos] ^= 0xFF;
        std::vector<uint8_t> out;
        parc_err e = decompress(bad, &out, 3);
        if (e == PARC_OK) {
            ASSERT_EQ(out, in) << "byte " << pos
                               << " flipped, decoded to different content";
        }
    }
}
