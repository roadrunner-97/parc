// Corpus-driven baseline benchmarks: zlib / lz4 / zstd compress and
// decompress over every manifest entry, reporting bytes_per_second (of
// uncompressed data, both directions) and a `ratio` counter
// (compressed / original — lower is better).
//
// These are the numbers parc is measured against from day one. Archive a
// run per commit with tools/run_bench.sh; filter with e.g.
//   parc_bench --benchmark_filter='zstd.*/silesia'

#include <benchmark/benchmark.h>

#include <lz4.h>
#include <zlib.h>
#include <zstd.h>

#include "parc/parc.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kZlibLevel = 6;
constexpr int kZstdLevel = 3;

std::vector<uint8_t> load_file(const std::string &path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("cannot open " + path);
    auto size = f.tellg();
    std::vector<uint8_t> data(static_cast<size_t>(size));
    f.seekg(0);
    f.read(reinterpret_cast<char *>(data.data()),
           static_cast<std::streamsize>(data.size()));
    if (!f) throw std::runtime_error("short read on " + path);
    return data;
}

// Minimal extraction of "file" values from corpus/manifest.json — the
// manifest is machine-written with one key per line, so a substring scan
// is sufficient and avoids a JSON dependency.
std::vector<std::string> manifest_files(const std::string &manifest_path) {
    std::ifstream f(manifest_path);
    if (!f) throw std::runtime_error("cannot open " + manifest_path);
    std::vector<std::string> files;
    std::string line;
    const std::string key = "\"file\": \"";
    while (std::getline(f, line)) {
        auto at = line.find(key);
        if (at == std::string::npos) continue;
        at += key.size();
        auto end = line.find('"', at);
        if (end == std::string::npos) continue;
        files.push_back(line.substr(at, end - at));
    }
    return files;
}

// ---- whole-buffer codec adapters ---------------------------------------
// Each returns the compressed size; decompressors check exact output size.

size_t zlib_compress(const std::vector<uint8_t> &in,
                     std::vector<uint8_t> &out) {
    uLongf dst = compressBound(static_cast<uLong>(in.size()));
    out.resize(dst);
    if (compress2(out.data(), &dst, in.data(),
                  static_cast<uLong>(in.size()), kZlibLevel) != Z_OK)
        throw std::runtime_error("zlib compress failed");
    return dst;
}

void zlib_decompress(const std::vector<uint8_t> &comp, size_t comp_size,
                     std::vector<uint8_t> &out, size_t orig_size) {
    uLongf dst = static_cast<uLongf>(orig_size);
    if (uncompress(out.data(), &dst, comp.data(),
                   static_cast<uLong>(comp_size)) != Z_OK ||
        dst != orig_size)
        throw std::runtime_error("zlib decompress failed");
}

size_t lz4_compress(const std::vector<uint8_t> &in,
                    std::vector<uint8_t> &out) {
    int bound = LZ4_compressBound(static_cast<int>(in.size()));
    out.resize(static_cast<size_t>(bound));
    int n = LZ4_compress_default(
        reinterpret_cast<const char *>(in.data()),
        reinterpret_cast<char *>(out.data()),
        static_cast<int>(in.size()), bound);
    if (n <= 0) throw std::runtime_error("lz4 compress failed");
    return static_cast<size_t>(n);
}

void lz4_decompress(const std::vector<uint8_t> &comp, size_t comp_size,
                    std::vector<uint8_t> &out, size_t orig_size) {
    int n = LZ4_decompress_safe(
        reinterpret_cast<const char *>(comp.data()),
        reinterpret_cast<char *>(out.data()),
        static_cast<int>(comp_size), static_cast<int>(orig_size));
    if (n < 0 || static_cast<size_t>(n) != orig_size)
        throw std::runtime_error("lz4 decompress failed");
}

size_t zstd_compress(const std::vector<uint8_t> &in,
                     std::vector<uint8_t> &out) {
    size_t bound = ZSTD_compressBound(in.size());
    out.resize(bound);
    size_t n = ZSTD_compress(out.data(), bound, in.data(), in.size(),
                             kZstdLevel);
    if (ZSTD_isError(n)) throw std::runtime_error("zstd compress failed");
    return n;
}

void zstd_decompress(const std::vector<uint8_t> &comp, size_t comp_size,
                     std::vector<uint8_t> &out, size_t orig_size) {
    size_t n = ZSTD_decompress(out.data(), orig_size, comp.data(), comp_size);
    if (ZSTD_isError(n) || n != orig_size)
        throw std::runtime_error("zstd decompress failed");
}

// parc's v0 API is stdio streams; fmemopen/open_memstream adapt it to the
// whole-buffer shape. The memstream copy is billed to parc — acceptable
// noise until a memory API lands (Phase 5). threads = 1 is the
// single-threaded reference path; >= 2 exercises the Phase 4 pipeline
// (the parc-0-tN codec entries track scaling efficiency per commit).
size_t parc_compress_g(unsigned format, unsigned level, unsigned threads,
                       const std::vector<uint8_t> &in,
                       std::vector<uint8_t> &out) {
    FILE *fin = fmemopen(const_cast<uint8_t *>(in.data()), in.size(), "rb");
    char *buf = nullptr;
    size_t len = 0;
    FILE *fout = open_memstream(&buf, &len);
    parc_copts opts = {0, threads, level, format};
    if (!fin || !fout ||
        parc_compress_stream(fin, fout, &opts, nullptr) != PARC_OK)
        throw std::runtime_error("parc compress failed");
    fclose(fin);
    fclose(fout);
    out.assign(buf, buf + len);
    free(buf);
    return len;
}

size_t parc0_compress_t(unsigned threads, const std::vector<uint8_t> &in,
                        std::vector<uint8_t> &out) {
    return parc_compress_g(PARC_FORMAT_V0, 0, threads, in, out);
}

size_t parc1_compress_t(unsigned threads, const std::vector<uint8_t> &in,
                        std::vector<uint8_t> &out) {
    return parc_compress_g(PARC_FORMAT_V1, 0, threads, in, out);
}

void parc0_decompress_t(unsigned threads, const std::vector<uint8_t> &comp,
                        size_t comp_size, std::vector<uint8_t> &out,
                        size_t orig_size) {
    FILE *fin = fmemopen(const_cast<uint8_t *>(comp.data()), comp_size, "rb");
    char *buf = nullptr;
    size_t len = 0;
    FILE *fout = open_memstream(&buf, &len);
    parc_dopts opts = {threads};
    if (!fin || !fout ||
        parc_decompress_stream(fin, fout, &opts, nullptr) != PARC_OK)
        throw std::runtime_error("parc decompress failed");
    fclose(fin);
    fclose(fout);
    if (len != orig_size) throw std::runtime_error("parc size mismatch");
    memcpy(out.data(), buf, len);
    free(buf);
}

// Single-threaded compression at an explicit level (decompression is
// level-independent, so the parc-0-LN entries reuse parc0_decompress).
size_t parc0_compress_level(unsigned level, const std::vector<uint8_t> &in,
                            std::vector<uint8_t> &out) {
    return parc_compress_g(PARC_FORMAT_V0, level, 1, in, out);
}

size_t parc1_compress_level(unsigned level, const std::vector<uint8_t> &in,
                            std::vector<uint8_t> &out) {
    return parc_compress_g(PARC_FORMAT_V1, level, 1, in, out);
}

size_t parc0_compress(const std::vector<uint8_t> &in,
                      std::vector<uint8_t> &out) {
    return parc_compress_g(PARC_FORMAT_V0, 0, 1, in, out);
}

size_t parc1_compress(const std::vector<uint8_t> &in,
                      std::vector<uint8_t> &out) {
    return parc_compress_g(PARC_FORMAT_V1, 0, 1, in, out);
}

// Decompression auto-detects the wire version from the frame header, so the
// v0 decode adapters serve v1 frames too.
void parc0_decompress(const std::vector<uint8_t> &comp, size_t comp_size,
                      std::vector<uint8_t> &out, size_t orig_size) {
    parc0_decompress_t(1, comp, comp_size, out, orig_size);
}

using compress_fn = std::function<size_t(const std::vector<uint8_t> &,
                                         std::vector<uint8_t> &)>;
using decompress_fn = std::function<void(const std::vector<uint8_t> &, size_t,
                                         std::vector<uint8_t> &, size_t)>;

void bm_compress(benchmark::State &state, const std::string &path,
                 compress_fn fn) {
    auto input = load_file(path);
    std::vector<uint8_t> out;
    size_t comp_size = 0;
    for (auto _ : state) {
        comp_size = fn(input, out);
        benchmark::DoNotOptimize(out.data());
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(input.size()));
    state.counters["ratio"] =
        static_cast<double>(comp_size) / static_cast<double>(input.size());
}

void bm_decompress(benchmark::State &state, const std::string &path,
                   compress_fn cfn, decompress_fn dfn) {
    auto input = load_file(path);
    std::vector<uint8_t> comp;
    size_t comp_size = cfn(input, comp);
    std::vector<uint8_t> out(input.size());
    for (auto _ : state) {
        dfn(comp, comp_size, out, input.size());
        benchmark::DoNotOptimize(out.data());
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(input.size()));
    state.counters["ratio"] =
        static_cast<double>(comp_size) / static_cast<double>(input.size());
}

struct Codec {
    std::string name;
    compress_fn c;
    decompress_fn d;
    // Multithreaded codecs must be timed on wall clock: the calling
    // thread sleeps while workers run, so the default CPU-time-based
    // bytes_per_second would be wildly inflated.
    bool real_time;
};

std::vector<Codec> make_codecs() {
    std::vector<Codec> codecs = {
        {"zlib-6", zlib_compress, zlib_decompress, false},
        {"lz4", lz4_compress, lz4_decompress, false},
        {"zstd-3", zstd_compress, zstd_decompress, false},
        {"parc-0", parc0_compress, parc0_decompress, false},
        {"parc-1", parc1_compress, parc0_decompress, false},
    };
    // level sweep (parc-N is the default level); ratio ladder per commit
    for (unsigned L : {1u, 6u, 8u, 9u}) {
        using namespace std::placeholders;
        codecs.push_back({"parc-0-L" + std::to_string(L),
                          std::bind(parc0_compress_level, L, _1, _2),
                          parc0_decompress, false});
        codecs.push_back({"parc-1-L" + std::to_string(L),
                          std::bind(parc1_compress_level, L, _1, _2),
                          parc0_decompress, false});
    }
    // thread-scaling sweep for the Phase 4 pipeline
    for (unsigned t : {2u, 4u, 8u, 16u}) {
        using namespace std::placeholders;
        codecs.push_back({"parc-0-t" + std::to_string(t),
                          std::bind(parc0_compress_t, t, _1, _2),
                          std::bind(parc0_decompress_t, t, _1, _2, _3, _4),
                          true});
        codecs.push_back({"parc-1-t" + std::to_string(t),
                          std::bind(parc1_compress_t, t, _1, _2),
                          std::bind(parc0_decompress_t, t, _1, _2, _3, _4),
                          true});
    }
    // nproc-threaded level ladder: parc as it is actually run. The frame is
    // bit-identical to the single-thread ladder, so ratio matches exactly and
    // only speed differs; this is the series plotted against the baselines by
    // tools/plot_bench.py. Wall-clock timed like the other MT entries. L == 0
    // is the default level.
    unsigned hw = std::thread::hardware_concurrency();
    if (hw < 1) hw = 1;
    for (unsigned L : {0u, 1u, 6u, 8u, 9u}) {
        using namespace std::placeholders;
        std::string suf = L ? "-L" + std::to_string(L) : "";
        codecs.push_back({"parc-0-mt" + suf,
                          std::bind(parc_compress_g, PARC_FORMAT_V0, L, hw, _1, _2),
                          std::bind(parc0_decompress_t, hw, _1, _2, _3, _4),
                          true});
        codecs.push_back({"parc-1-mt" + suf,
                          std::bind(parc_compress_g, PARC_FORMAT_V1, L, hw, _1, _2),
                          std::bind(parc0_decompress_t, hw, _1, _2, _3, _4),
                          true});
    }
    return codecs;
}

}  // namespace

int main(int argc, char **argv) {
    const std::string corpus_dir = PARC_CORPUS_DIR;
    std::vector<std::string> files;
    try {
        files = manifest_files(corpus_dir + "/manifest.json");
    } catch (const std::exception &e) {
        std::fprintf(stderr, "parc_bench: %s\n", e.what());
        std::fprintf(stderr,
                     "parc_bench: run tools/fetch_corpus.py first\n");
        return 1;
    }

    const auto codecs = make_codecs();
    for (const auto &file : files) {
        std::string path = corpus_dir + "/data/" + file;
        for (const auto &codec : codecs) {
            auto *c = benchmark::RegisterBenchmark(
                "compress/" + codec.name + "/" + file,
                bm_compress, path, codec.c);
            auto *d = benchmark::RegisterBenchmark(
                "decompress/" + codec.name + "/" + file,
                bm_decompress, path, codec.c, codec.d);
            if (codec.real_time) {
                c->UseRealTime();
                d->UseRealTime();
            }
        }
    }

    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
