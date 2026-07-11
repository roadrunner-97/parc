# parc — implementation plan

**Status (2026-07-11):** Phases 1–4 complete; Phase 5 underway. Phase 2
delivered stats core
(+ merge, LZ probe), parcgen generators + CLI, parcent CLI (ent-validated,
multithreaded, with `tools/parcent/ent-diff.py` as a standing CTest
differential test against `ent`), corpus fetcher + manifest, benchmark
harness with zlib/lz4/zstd baselines archived in `bench/results/`.
Phase 3 delivered format v0 (`docs/FORMAT.md`, normative): stored + packed
(greedy hash-table LZ + canonical Huffman) blocks, per-block xxh64, index
trailer, stream hash; codec in `src/codec/` (huffman/lz/block/frame);
`parc` CLI (compress/decompress/verify, stdin/stdout); correctness rig:
unit + property roundtrip suites, exhaustive truncation/byte-mutation
corruption tests, golden fixtures in `tests/golden/`, libFuzzer targets
(`fuzz/`, `fuzz` preset, smoke-run in CTest); parc-0 wired into the corpus
benchmark. First fuzz campaign (2026-07-10, 3.5 h/target, coverage-guided):
clean — 5.5M decode + 4.6M roundtrip execs, zero crashes/leaks/hangs.
First corpus numbers (single-threaded, see `bench/results/`): ratio sits
between lz4 and zlib-6 (e.g. enwik8 0.403 vs lz4 0.573 / zlib 0.365 /
zstd-3 0.354); compression 180–550 MB/s (5–8x zlib-6); decompression
90–390 MB/s is the known gap — bit-serial Huffman decode, addressed by
table-based decode + FSE in Phase 5.
Phase 4 delivered multithreading both directions: generic ordered pipeline
(`src/codec/mt.c` — caller thread reads blocks in sequence, N pthread
workers transform out of order, one writer thread re-serializes; 2N slots
in flight), frame callbacks in `src/codec/frame_mt.c` over shared wire
internals (`frame_int.h`), `parc_copts.threads`/`parc_dopts.threads` API
(0/1 = single-threaded reference path kept intact), CLI `-T/--threads`
(0 = nproc). Frames are bit-identical for every thread count (asserted in
`tests/test_mt.cpp`: thread sweep 1/2/nproc/2nproc vs single-threaded
reference, many-block stress, threads > blocks, empty input, MT corruption
and full truncation sweeps); whole matrix green incl. TSan. enwik8 spot
check (24-core): 0.54 s → 0.055 s compress, 0.56 s → 0.056 s decompress
at T=16 (~10x, ~1.8 GB/s both ways, saturating ~T=16); bench harness
gained parc-0-t{2,4,8,16} entries (wall-clock timed) to track scaling per
commit. First archived MT run (`bench/results/92cee24.json`, in-memory
harness incl. per-call thread spawn): enwik8 t16 compress 1205 MiB/s
(6.9x), decompress 768 MiB/s (5.2x); near-linear to t4, tapering beyond
t8.
Phase 5 (in progress) began with the match stage, format-preserving so the
decoder, golden fixtures and wire format are untouched (no version bump). A
compression level 1..9 (`parc_copts.level`, CLI `-L`, default 3) drives the
matcher: level 1 is the existing greedy hash-table matcher; 2..9 use a new
hash-chain + one-step-lazy matcher (`parc_lz_chain`, `parc_lz_cfg_for_level`
in `src/codec/lz.c`) with deepening chain length and nice-length. The chain
array (`parc_blk_cctx.prev`, ~4x block size) is allocated only at chain
levels; MT path threads level through unchanged. Correctness rides the
existing rig: per-level token-invariant + rebuild tests (`test_lz.cpp`),
all-levels block roundtrip (`test_block.cpp`), the roundtrip fuzzer now
fuzzes level too; full matrix green incl. TSan; roundtrip fuzz smoke clean.
enwik8 single-threaded: the default (L3) ratio is 0.348 — past zstd-3 (0.354)
and zlib-6 (0.365) — vs the old greedy 0.403, at 49 MiB/s compress (L1 0.403
@ 170, L6 0.339 @ 11, L9 0.336 @ 3.9). Bench gained `parc-0-L{1,6,9}`
entries.
Then the decode gap: the bit-serial Huffman walk (up to 15 branchy bit-reads
per symbol) became a direct root table. `parc_hdec` indexes the next
`root_bits = min(maxlen, 11)` stream bits into a table of packed
`(symbol << 4) | len` cells; codes longer than 11 bits (rare, by
construction rare symbols) fall back to the old bit-serial walk. A new
`parc_br_peek` reads the max code width without consuming (zero-fills past
end, never fails), then `parc_br_get` consumes the resolved length — so exact
bit-accounting (minimal `comp_len`, zero padding, truncation) is preserved.
Decoder-internal, no format change: golden fixtures decode bit-identically.
Correctness rides the rig plus a long-code fallback test and a peek unit
test; 87k corrupt-input decode-fuzz execs clean; full matrix green incl.
TSan. Single-threaded decode is ~1.5x faster (enwik8 173 -> 261 MiB/s,
webster 205 -> 311, xml 543 -> 820).
Then the FSE entropy stage + repeat-offset codes landed as **format version 1**
(the first wire bump; `docs/FORMAT.md` gained a normative §3 and §5). v1 codes a
zstd-style sequence model: block literals form one FSE stream; each match is a
`(litLen, matchLen, offset)` sequence with three more FSE streams; offsets carry
repeat-offset codes over a 3-entry move-to-front cache (init {1,2,3}). New
`src/codec/fse.c` is a self-contained table-driven ANS coder over `uint8`
symbols (largest-remainder normalization, bucket-coded count tables); the
encoder lays its groups down in decode order so decode is an ordinary forward
`parc_br` walk (no backward reader). The entropy backend stays encapsulated in
`block.c` (v0 Huffman and v1 FSE side by side, dispatched by version); a
`parc_blk_dctx` holds v1 decode scratch, threaded through `frame.c`/`frame_mt.c`
(per-thread on the MT path). Decoder accepts both versions, dispatching on the
header byte; encoder picks via `parc_copts.format` (default v1) / CLI `-f`; v0
frames stay byte-frozen and MT frames bit-identical for both versions. Correctness
rides the rig plus a new `test_fse.cpp`, per-version block/frame/MT sweeps, v1
golden fixtures (`tests/golden/*-v1.parc`), the roundtrip fuzzer now fuzzing
format too and the decode fuzzer seeded with v1 frames; full matrix green incl.
TSan, fuzz campaign clean. On the real corpus v1 beats v0 everywhere at L6:
enwik8 0.3387 -> 0.3368, webster -0.4%, xml -1.9%, mozilla -3.2%, nci -3.5%,
samba -3.7%; on random-literal synthetic data the two are ~parity (the sequence
model's litLen overhead roughly cancels the FSE/rep-offset gains when nothing
recurs). FSE-over-Huffman on literals is inherently marginal (as zstd's
Huffman-literals design implies); v1's real-data win comes from separate
distributions + repeat offsets, and is the foundation for the matching work that
makes the sequence model pay more. Bench gained `parc-1[-L*/-t*]` entries. Next
Phase 5 levers: wider/faster decode, and richer matching (optimal parse, larger
windows) to exploit the sequence model.

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
  Order-1 entropy carries a Miller-Madow bias correction so small samples
  (a few-KiB random file under-fills the 65536 pair contexts) don't read as
  false structure; it's first-order, so deep undersampling is only reduced,
  not removed. *Future:* if that residual bias bites, upgrade the order-1 (and
  order-2) estimator to NSB or Chao-Shen for near-unbiased entropy at small N.
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
