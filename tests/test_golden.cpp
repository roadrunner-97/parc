// Golden fixtures: committed v0 frames that must stay decodable forever.
// Each fixture decodes and is compared against regenerated parcgen output
// (generator output is itself a pinned stability contract). If these tests
// fail, the format changed: that requires a version bump, not a fixture
// update (docs/FORMAT.md §4).
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "parc/parc.h"
#include "gen.h"
#include "util/buf.h"
#include "util/rng.h"

namespace {

std::vector<uint8_t> read_file(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    EXPECT_NE(f, nullptr) << path;
    std::vector<uint8_t> v;
    if (f) {
        uint8_t buf[4096];
        size_t got;
        while ((got = fread(buf, 1, sizeof buf, f)) > 0)
            v.insert(v.end(), buf, buf + got);
        fclose(f);
    }
    return v;
}

std::vector<uint8_t> decode(const std::vector<uint8_t> &frame) {
    FILE *fin = tmpfile();
    EXPECT_EQ(fwrite(frame.data(), 1, frame.size(), fin), frame.size());
    rewind(fin);
    FILE *fout = tmpfile();
    EXPECT_EQ(parc_decompress_stream(fin, fout, nullptr), PARC_OK);
    rewind(fout);
    std::vector<uint8_t> v;
    uint8_t buf[4096];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, fout)) > 0)
        v.insert(v.end(), buf, buf + got);
    fclose(fin);
    fclose(fout);
    return v;
}

void expect_golden(const char *file, uint64_t seed, size_t n,
                   parc_err (*g)(parc_rng *, size_t, parc_buf *)) {
    auto frame = read_file(std::string(GOLDEN_DIR "/") + file);
    ASSERT_FALSE(frame.empty()) << file;
    parc_rng rng;
    parc_rng_seed(&rng, seed);
    parc_buf b;
    parc_buf_init(&b);
    ASSERT_EQ(g(&rng, n, &b), PARC_OK);
    std::vector<uint8_t> want(b.data, b.data + b.len);
    parc_buf_free(&b);
    EXPECT_EQ(decode(frame), want) << file;
}

}  // namespace

TEST(Golden, Text64k) { expect_golden("text-64k.parc", 1001, 65536,
                                      parc_gen_text); }
TEST(Golden, JsonLog64k) { expect_golden("jsonlog-64k.parc", 1002, 65536,
                                         parc_gen_json_log); }
TEST(Golden, Random16k) { expect_golden("random-16k.parc", 1003, 16384,
                                        parc_gen_random); }

TEST(Golden, Empty) {
    auto frame = read_file(GOLDEN_DIR "/empty.parc");
    ASSERT_EQ(frame.size(), 37u);
    EXPECT_TRUE(decode(frame).empty());
}
