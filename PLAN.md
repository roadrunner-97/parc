# parc — implementation plan

Phases are ordered so that measurement exists before the codec does, and
correctness is locked in before performance work starts. Each phase ends green:
all tests pass under the full sanitizer matrix before the next phase begins.

Each phase notes a **delegation** line — which parts are well-specified enough
to hand to a Sonnet or Haiku subagent (spec + interface stubs + acceptance
tests written first, subagent implements until green, result reviewed).

## Phase 1 — Scaffold & foundations

- Repo layout: `src/`, `include/parc/`, `tools/`, `tests/`, `bench/`,
  `corpus/`, `cmake/`.
- CMake presets (`debug`, `release`, `asan`, `ubsan`, `tsan`, `fuzz`),
  GoogleTest + Google Benchmark via FetchContent, CTest wiring, GitHub Actions
  matrix.
- Foundation modules with unit tests: error taxonomy (`parc_err`), xxHash64,
  seeded RNG (xoshiro/PCG — shared by `parcgen` and property tests), bitstream
  reader/writer, arena/buffer helpers.
- **Delegation:** CI config and CMake boilerplate → Haiku. xxHash + RNG +
  bitstream implementations against my stubs and test specs → Sonnet.

## Phase 2 — Measurement & corpus tooling (before any codec code)

- `parcent` entropy analyzer: ent-parity stats first (order-0 entropy,
  chi-square + p-value, mean, Monte Carlo π, serial correlation), then
  order-1/2 entropy, sliding-window profile, min-entropy, LZ-probe
  compressibility, JSON/CSV output, multithreaded streaming over large files.
  Validated against `ent` on identical inputs and against analytically known
  distributions (uniform random = 8.000 bits/byte, constant = 0, biased coin =
  closed-form).
- `parcgen` synthetic corpus generators (seeded, reproducible), each generator
  validated by `parcent` (generator asked for ~4.0 bits/byte must measure so).
- Corpus fetcher script (Silesia, Canterbury, enwik8; checksum-verified cache)
  + manifest format tagging every item with size and entropy class.
- Google Benchmark harness skeleton: corpus-driven benchmark runner with custom
  counters (MB/s, ratio), JSON archiving per commit, in-process zlib/lz4/zstd
  baselines. Establish baseline numbers for the comparison codecs on the full
  corpus — the targets we measure against from day one.
- **Delegation:** this phase is the most subagent-friendly. `parcgen`
  generators and fetcher → Sonnet. `parcent` stats core → Sonnet against my
  test vectors. Benchmark skeleton → Sonnet. Manifest plumbing, README bits →
  Haiku.

## Phase 3 — Minimal correct codec (single-threaded)

- Frame format v0: header (magic, version, flags), block index, per-block
  xxHash64, stream hash trailer. Written spec in `docs/FORMAT.md` from the
  start.
- Stored (raw) blocks + greedy hash-table LZ + canonical Huffman. Ratio will be
  mediocre; the point is a complete, correct pipeline.
- Full correctness rig lands here: GoogleTest unit suites, seeded property
  roundtrip tests across all `parcgen` classes and sizes (incl. 0 bytes, 1
  byte, block-boundary±1), libFuzzer decoder target, golden fixtures,
  corrupted/truncated-input suite (every error path returns a clean error).
- CLI v0: `compress`/`decompress`/`verify` over files and stdin/stdout.
- **Delegation:** little of the codec core; CLI arg parsing and the
  corrupted-input test-case generator → Sonnet.

## Phase 4 — Multithreading (both directions)

- Pthread worker pool; pipelined read → compress N blocks in flight → ordered
  writer. Same structure for decompression using the block index.
- Dynamic scheduling allowed (no output-determinism contract): work stealing,
  adaptive block sizing by observed throughput.
- Tests: TSan on everything; thread-count sweep (1, 2, nproc, 2×nproc)
  asserting decoded output identical to single-threaded reference; stress with
  huge/tiny/empty inputs; scaling efficiency tracked in Google Benchmark.
- **Delegation:** none for the pool/pipeline core; stress-test suite → Sonnet.

## Phase 5 — Ratio & speed (benchmark-driven, iterative)

- Lazy matching + hash chains at higher levels; repeat-offset codes; longer
  matches/windows; level tuning 1–9 (fast levels stay near lz4-class speed,
  high levels chase gzip-then-zstd-class ratio).
- FSE/tANS entropy stage replacing Huffman at higher levels — the single
  biggest ratio lever. Literals/lengths/offsets get separate distributions.
- Every change justified by the corpus score; microbenchmarks guard hot-path
  regressions.
- **Delegation:** codec core stays with me; per-change benchmark sweeps and
  report summaries → Haiku.

## Phase 6 — Adaptivity

- Per-block entropy probe (reusing `parcent` internals) → stored-block bailout
  for incompressible data; target: near-memcpy speed on random/JPEG corpus
  items with ratio ≥ 0.999.
- Optional filters: delta for numeric arrays; measured per corpus category,
  kept only where the corpus score says they pay.

## Phase 7 — Hardening & spec

- Long fuzz campaigns (libFuzzer + AFL++, corpus minimization, CI seed corpus).
- `docs/FORMAT.md` finalized as a normative spec; format version bump policy.
- Failure-injection tests (allocation failure paths), 32-bit/big-endian
  sanity if we care, `-Wconversion`-clean audit.
- **Delegation:** fuzz-campaign babysitting and crash triage summaries → Haiku;
  doc polish → Haiku.

## Definition of "bulletproof" (the standing bar for every phase)

1. All CTest suites green under debug, release, ASan+UBSan, and TSan.
2. Property roundtrip tests: no failing seed uncommitted; failures become
   permanent regression tests.
3. Fuzzers: zero crashes/leaks/hangs on the current seed corpus in CI.
4. Golden fixtures decode to known hashes (format stability).
5. Corpus score and throughput vs. baselines recorded per commit — no silent
   performance regressions.
