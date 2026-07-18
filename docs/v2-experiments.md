# v2 format — speed optimization log

`PARC_FORMAT_V2` (wire `FRAME_VERSION_V2`) is a **deliberately unfinalized**
sandbox for the literal/entropy stage. v0 and v1 are frozen released formats and
must stay bit-compatible; **v2 may change freely** — its bytes are not a contract
yet. This log is the durable memory of an autonomous optimization loop: what was
tried, what the numbers said, what was kept. Each loop iteration reads it first so
it never repeats a lever or silently regresses.

## Goal & guardrails (set by the user)

- **Objective:** squeeze maximum compress/decompress throughput out of parc, using
  the speed-vs-ratio benchmark as the judge. Strong bias toward **speed** over
  marginal ratio.
- **Ratio floor (aggressive):** accept a change if it is **ratio-neutral (within
  ~0.5%) with any measurable speedup**, OR it costs **≤5% ratio for a ≥10%
  speedup**. Never cross a 5% ratio regression autonomously.
- **Correctness is non-negotiable:** every kept change must pass the full `ctest`
  suite (currently 132 tests) AND an ASan build+test.
- **v0/v1 are disposable.** This is a toy project with no users; the user is happy
  to delete v0/v1 once v2 is solid. During the loop keep v1 alive anyway — it's the
  benchmark's comparison baseline (parc-2 vs parc-1 is how every experiment is
  judged). Do not spend effort preserving v0/v1 bit-compatibility beyond keeping
  their tests green; once v2 clearly wins, a dedicated cleanup commit removes them.
- **Commit policy:** each verified win = its own commit on `phase5-decode-speed`,
  **pushed** to `origin` for remote backup.
- **SIMD library — MANDATORY:** any SIMD work uses **Google Highway** (`hwy`), not
  raw intrinsics or another wrapper. This is a user directive. Add `hwy` via
  FetchContent in CMake like googletest/googlebenchmark already are. Do not
  hand-roll AVX2 intrinsics.
  - **C/C++ boundary is a non-issue.** Highway is C++-only; the codec is C. SIMD
    kernels go in a `.cc` TU exposing `extern "C"` entry points (Highway
    `HWY_NAMESPACE` + static/dynamic dispatch). These kernels are **coarse** — one
    call per stream/block over thousands of bytes — so the cross-language call is
    amortized to nothing. Never structure SIMD as a per-symbol cross-TU call.
    (LTO is already ON in build-release: `CMAKE_INTERPROCEDURAL_OPTIMIZATION=ON`,
    single gcc toolchain, so cross-language inlining happens too — but it isn't
    needed for coarse kernels.)
  - **Pivot to C++ only for ergonomics, not speed** — and the if/when is Claude's
    call (user delegated it, happy either way). If the `extern "C"` glue becomes
    painful or we want fine-grained Highway integration, convert the specific hot
    TU(s) to C++ (they compile as C++ nearly as-is) — a per-file pivot, not a
    whole-project rewrite. Barrier is low: the project already builds & links C++
    (gtest + gbench), so the toolchain/standard/CMake C++ path is established.
    Performance never requires it.

## Iteration protocol (each loop turn)

1. Read this file. Pick the next untried lever from the backlog (or generate one).
2. Implement it behind v2 only (v0/v1 output unchanged).
3. `ninja -C build && (cd build && ctest -j) ` → must be 132/132.
4. Rebuild `build-release parc_bench`; measure with the **quick bench** below.
   If promising, confirm with the **full bench**.
5. Decide against the guardrails. **Keep** → update the log table + commit + push.
   **Reject** → `git checkout -- <files>` (or revert), record why in the log so
   it's never retried blindly.
6. Periodically run an ASan test pass on kept changes.

### Quick bench (fast inner signal, ~40s)
```
./build-release/bench/parc_bench --benchmark_min_time=0.3s \
  --benchmark_filter='(compress|decompress)/parc-[12]/(silesia/dickens|silesia/webster|silesia/mozilla|enwik8)'
```
Compare `parc-2` against `parc-1` on the same files: `bytes_per_second` (both
directions) and `ratio`. Use ≥0.3s min_time; single-iteration numbers (esp.
enwik8) are noisy — bump min_time or `--benchmark_repetitions=3` before trusting a
small delta.

### Full bench (confirm a win, archive)
```
tools/run_bench.sh --benchmark_filter='parc-2|parc-1'   # archives bench/results/<rev>.json
python3 tools/plot_bench.py                             # refresh docs/benchmarks/*.png
```

## Baseline — commit `a16aaa5` (v2 = Huffman literals, pre-optimization)

Quick bench, `--benchmark_min_time=0.15s` (noisy, single-iter enwik8):

| file            | dir     | parc-1 MiB/s | parc-2 MiB/s | parc-1 ratio | parc-2 ratio |
|-----------------|---------|-------------:|-------------:|-------------:|-------------:|
| enwik8          | decode  | 368.5        | 359.1        | 0.3475       | 0.3473       |
| enwik8          | encode  | 70.6         | 74.1         | 0.3475       | 0.3473       |
| silesia/dickens | decode  | 336.1        | 334.6        | 0.3585       | 0.3586       |
| silesia/dickens | encode  | 72.4         | 73.0         | 0.3585       | 0.3586       |
| silesia/webster | decode  | 398.6        | 397.9        | 0.2781       | 0.2782       |
| silesia/webster | encode  | 91.9         | 92.4         | 0.2781       | 0.2782       |

**Headline:** current v2 (canonical Huffman literals) is **ratio-neutral but not
yet faster at decode** — slightly slower than v1 on enwik8/dickens, tied on
webster. The intended "Huffman decodes faster than tANS" win has not materialized;
finding out why (and fixing it) is experiment #1.

## Experiment #1 — decode profile & strategic pivot (commit `a16aaa5`)

Profiled single-thread v2 decode of enwik8 (`build-relprof`, PARC_PROF=ON,
`parc t -T 1`), splitting the literal Huffman stage from the sequence stages by
temporarily billing it to a separate timer. **Clean v2 decode split (enwik8, L3):**

| component                                    |  time  | % decode |
|----------------------------------------------|-------:|---------:|
| reconstruct (match copy + offset + extrabits)| 75.1ms |    42%   |
| sequence decode (LL/ML/OF tANS, 3 streams)   | 63.3ms |    35%   |
| literal decode (Huffman, 1 stream)           | 24.8ms |    14%   |
| block_hash + stream_hash + io               | ~15ms  |    ~9%   |

**Why Huffman literals didn't win:** both tANS and this Huffman decoder are
register-held, one-table-lookup-per-symbol loops, so per-symbol cost is equal;
the aggregate `entropy_decode` was unchanged (v1 87.3ms vs v2 88.7ms). And the
literal stream is **only 14% of decode** — the v2 premise optimized the small
corner. v1's aggregate entropy_decode (87ms) ≈ v2's sequences(63)+literals(25).

**Pivot — where the decode time actually is:**
1. **reconstruct — 42%.** Match/literal copy, offset resolve, extra-bits. Wire-
   format-neutral to speed up; benefits every version. Highest priority.
2. **sequence decode — 35%.** The three LL/ML/OF FSE streams dominate literals
   2.5×. Batching/interleaving/faster-table here beats any literal work.
3. **literal decode — 14%.** Even a 2× literal speedup is only ~7% of decode.
   Deprioritized. Multi-symbol Huffman table stays on the backlog but below 1&2.

**SIMD note (answering the standing question):** entropy decode (both tANS and
Huffman) is a serial bit-accumulator recurrence — each symbol's bit offset
depends on the previous symbol's code length — so the compiler cannot
autovectorize it and hand-SIMD needs *independent* streams. The 4-stream
interleaved layout (reserved v2 mode bit) is the only route to ILP/SIMD in the
literal/sequence decode. Reconstruct's copy loop is memcpy-shaped and already
compiler-vectorizable (wild_copy). When SIMD is pursued, use **Google Highway**
(see the guardrail above) — not raw intrinsics.

**Verdict:** no code change kept — diagnosis only. Redirects all further work
from literals to reconstruct + sequences.

## Lever backlog (priority order, revised after experiment #1)

**Tier 1 — reconstruct (42% of decode):**
- Profile the reconstruct loop internally: how much is extra-bits decode vs
  offset resolve vs the actual copy? (Split like exp #1 did for entropy.)
- Larger/branch-lighter copy units in `wild_copy` / `copy_match`; 32-byte chunks;
  specialize the common short-match case.
- Prefetch match source / destination ahead of the copy.
- Fuse extra-bits decode into the sequence decode (avoid a second pass over seqs).
- Reduce per-sequence branching (repeat-offset resolve is branchy).

**Tier 2 — sequence decode (35% of decode, 3 tANS streams):**
- Interleave/batch the LL/ML/OF FSE decode so the 3 streams' lookups overlap
  (ILP); or a single fused pass.
- Faster FSE table / wider tables / fewer renormalizations.
- Consider whether LL/ML/OF even need full tANS at these ratios, or a cheaper
  code (this is a wire-format change → v2 only).

**Tier 3 — literals (only 14%; deprioritized):**
- Multi-symbol Huffman root table (decode 2 short codes per lookup) — real but
  small (~7% ceiling). 4-stream interleaved literals (reserved mode bit) — the
  SIMD/ILP route, bigger but more invasive.
- Wider root_bits to cut long-code fallbacks (measure table-build cost).

**Encode-side (secondary):**
- Faster histogram / Huffman length building.
- Cheaper match finding at fast levels (partly done in Phase 5).

Ratio levers (only if ~free or they unlock a bigger speed win):
- Huffman table transmission cost (256×4 bits) is heavy for small blocks — RLE /
  delta the code-length table, or reuse a table across blocks.

Cleanup (once v2 clearly wins, not a speed lever):
- Delete v0/v1 encode paths + `PARC_FORMAT_V0/V1` and their tests; make v2 the only
  format. Keep only until then because v1 is the bench baseline.

## Experiment log

| # | rev | lever | decode Δ | encode Δ | ratio Δ | verdict |
|---|-----|-------|----------|----------|---------|---------|
| 0 | a16aaa5 | baseline established (v2=Huffman literals) | — | — | — | — |
| 1 | a16aaa5 | decode profile + strategic pivot to reconstruct/sequences | — | — | — | diagnosis only, no code |
