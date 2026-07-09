# parc — parallel adaptive compressor

*(working name — trivial to change; it only appears in the CLI name and the frame magic bytes)*

## Goal

A lossless compression tool in C that is fast and ratio-competitive across mixed
workloads (text, structured data, binaries, already-compressed data), multithreaded
for both compression and decompression, built alongside first-class measurement and
test-corpus tooling.

**Contract:** `decompress(compress(x)) == x` bit-for-bit, always. Compressed output
is *not* guaranteed byte-identical run-to-run — any valid encoding of the input is
acceptable. This frees the scheduler to use dynamic work splitting and adaptive
block sizing wherever that wins speed or ratio.

## Components

### 1. Core codec library (`libparc`, C11)

- **Container format designed for parallelism.** Input split into independent
  blocks with a block index in the frame. Independent blocks are what make
  *decompression* parallel too (gzip/xz single-stream can't do this). Block size
  is a tunable per level (256 KiB – 4 MiB); may be chosen adaptively at runtime
  since output determinism is not a contract.
- **Two-stage pipeline per block:** LZ77-family matching (hash-table greedy at
  fast levels, hash-chain lazy matching at high levels) → entropy stage
  (canonical Huffman fast path; FSE/tANS at higher levels).
- **Content adaptivity:** cheap per-block entropy probe; incompressible blocks
  stored raw (keeps speed up on JPEG/encrypted input); optional filters later
  (delta for numeric arrays, etc.).
- **Integrity:** per-block xxHash64 + whole-stream hash in the trailer. Corrupt
  or truncated input must produce a clean error — never a crash, hang, OOM, or
  silent wrong output.
- **Library discipline:** no globals, caller-provided allocators optional,
  streaming-friendly API, versioned frame header so the format can evolve.

### 2. CLI (`parc`)

`compress` / `decompress` / `verify` / `inspect` (dump frame metadata), levels
1–9, `--threads N` (default: online CPUs), stdin/stdout streaming, sensible exit
codes. Single-file semantics first; multi-file archiving is a possible later layer.

### 3. Entropy analyzer (`parcent`) — the `ent` replacement

A standalone tool (C11, links `libparc` utility code) fixing everything `ent`
lacks:

- **Everything `ent` reports, done right:** order-0 Shannon entropy (bits/byte),
  chi-square *with p-value*, arithmetic mean, Monte Carlo π, serial correlation —
  so it's a drop-in upgrade.
- **What `ent` can't do:**
  - Order-1 and order-2 conditional entropy (catches structure that order-0
    misses entirely — `ent` calls Markov text "high entropy" when it's very
    compressible).
  - Sliding-window entropy profile with configurable window/stride, so mixed
    files (e.g. a binary with an embedded compressed blob) show *where* the
    entropy lives instead of one misleading average.
  - Min-entropy estimate and byte histogram; autocorrelation at multiple lags.
  - A practical compressibility estimate (fast LZ probe), reported alongside the
    theoretical entropy.
  - Output as human table, JSON, or CSV; per-block breakdown.
  - Multithreaded and streaming — handles multi-GB files at I/O speed.
- Doubles as the corpus classifier: the benchmark suite uses it to tag every
  corpus item with an entropy class.

### 4. Performance measurement (Google Benchmark)

- **Microbenchmarks** for hot internals: match finder, entropy coder, bitstream
  I/O, hashing — catch regressions at the function level.
- **End-to-end throughput benchmarks** over corpus files: compression and
  decompression separately, as custom counters (bytes/sec, ratio, threads);
  Google Benchmark natively provides repetitions, median/CV/stddev, and JSON
  output.
- **In-process baselines:** the same benchmark binaries link zlib, lz4, and zstd
  and run them on identical buffers — apples-to-apples comparison with no
  subprocess noise.
- Results archived as JSON per git commit so regressions are diffable; a
  weighted **corpus score** makes "good across a variety of use cases" one
  trackable number, broken down by data category.

### 5. Corpus tooling

- **Synthetic generators (`parcgen`, C)** — seeded and fully reproducible, fast
  enough to generate gigabytes on the fly: parameterized-entropy byte streams,
  Markov-chain text, structured JSON/CSV/log data with realistic repetition,
  binary record arrays, run-length-heavy and sparse data, pure random
  (incompressibility check), and file trees mixing many tiny files with a few
  huge ones.
- **Real-data fetcher** (small script) — Silesia, Canterbury, enwik8; downloaded
  once, checksum-verified, cached. Synthetic data always lies a little; these
  calibrate the generators.
- A manifest per corpus item (size, entropy class from `parcent`) so benchmark
  results are broken down by data category.

## Testing (bulletproof, CTest + GoogleTest)

- **CTest** is the single entry point: unit suites, property suites, fuzz smoke
  runs, and golden-fixture checks all registered as tests; the sanitizer builds
  run the same test list.
- **GoogleTest unit tests** per module: bitstream reader/writer, entropy coder
  round-trips, match-finder correctness, frame parsing, error paths. (Tests are
  C++17 exercising the C API — standard practice for C libraries.)
- **Property-based roundtrip tests:** seeded-RNG GoogleTest cases feeding
  arbitrary inputs (sizes 0 to hundreds of MB, all entropy classes via the
  `parcgen` generators) through compress→decompress and asserting bit-identity —
  the single highest-value test in a compressor. Every failure's seed is
  committed as a regression case.
- **Fuzzing** the decoder with libFuzzer (AFL++ as a second engine): truncated,
  bit-flipped, and adversarial frames must fail cleanly.
- **Sanitizer matrix in CI:** all tests under ASan+UBSan; threaded paths under
  TSan. In C this matrix *is* the memory-safety story — non-negotiable.
- **Golden fixtures:** committed compressed files with known decode hashes, so a
  format-breaking change fails CI loudly.
- **Concurrency stress:** oversubscribed pools, 1..2×CPUs threads, huge/tiny
  inputs, thread-count sweeps asserting identical *decoded* output (we verify
  functional equivalence, not byte-identical compressed output).

## Build & toolchain

- Core library and tools: C11 + pthreads. Tests/benchmarks: C++17 (GoogleTest,
  Google Benchmark via FetchContent or system packages).
- CMake presets: `debug`, `release`, `asan`, `ubsan`, `tsan`, `fuzz`;
  warnings-as-errors (`-Wall -Wextra -Wconversion`).
- CI (GitHub Actions): build × sanitizer matrix through CTest, fuzz smoke run,
  benchmark job publishing the corpus score per commit.
