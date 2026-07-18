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

## Baseline — commit `<scaffold>` (v2 = Huffman literals, pre-optimization)

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

## Lever backlog (unordered; refine as we learn)

Decode-side (primary):
- **Diagnose why Huffman literals aren't beating tANS.** Profile the v2 decode
  (`PARC_PROF` stage timers already exist). Is the literal stage even the
  bottleneck, or is it match reconstruction / sequence decode? Measure before
  optimizing — the literal decoder may be fine and the win is elsewhere.
- Wider Huffman root table (root_bits) to cut long-code fallbacks; measure table
  build cost vs decode gain.
- Double/quad literal decode per refill (decode 2 symbols between refills when
  root_bits*2 fit the accumulator) — the classic libdeflate trick.
- 4-stream interleaved literals (parallel Huffman decoders hiding latency) — the
  "mode bit" is already reserved for this in the wire layout.
- Batch/branchless sequence (LL/ML/OF) decode; fuse extra-bits.
- Reconstruct loop: overlap-safe wildcopy tuning, larger copy units.
- Prefetch destination / literal source ahead.

Encode-side (secondary but in scope):
- Faster histogram / Huffman length building.
- Cheaper match finding at fast levels (already partly done in Phase 5).

Ratio levers (only if ~free or they unlock a bigger speed win):
- Huffman table transmission cost (256×4 bits) is heavy for small blocks — RLE /
  delta the code-length table, or reuse a table across blocks.

Cleanup (once v2 clearly wins, not a speed lever):
- Delete v0/v1 encode paths + `PARC_FORMAT_V0/V1` and their tests; make v2 the only
  format. Keep only until then because v1 is the bench baseline.

## Experiment log

| # | rev | lever | decode Δ | encode Δ | ratio Δ | verdict |
|---|-----|-------|----------|----------|---------|---------|
| — | —   | (baseline established) | —        | —        | —       | —       |
