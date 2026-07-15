# Potential speed optimizations

Ways to make parc faster without giving up compression ratio. Everything here
is bit-identical on the wire unless explicitly flagged **[format change]**.
Roughly ordered by expected impact within each section; the bitstream and the
match-copy loop are the two biggest levers because everything hot funnels
through them.

## Profiling — measure before you cut

Two complementary views:

- **Per-stage timers (in-tree).** Build the `prof` preset (`cmake --preset prof
  && cmake --build build-prof`) — an optimized `RelWithDebInfo -O3 -g` build with
  `-DPARC_PROF` (`src/util/prof.{h,c}`). The single-thread compress/decompress
  path (`frame.c`, `block.c`) then prints a stage breakdown (ms / % / MB/s /
  calls) to stderr at end of stream. Stages: `io_read`/`io_write`,
  `lz_match`, `transcode` (v1 tokens→sequences), `entropy_encode`,
  `entropy_decode`, `reconstruct` (extra-bits + offset resolve + match copy),
  `block_hash`, `stream_hash`. Timing is at stage granularity (a handful of
  `CLOCK_MONOTONIC` reads per block, never per symbol), so overhead is noise and
  the numbers are undistorted; off by default and zero-cost (every macro expands
  to `((void)0)`). **Single-thread only** — the counters are lock-free globals,
  so run `parc -T 1`; the MT frame path is not instrumented and its report
  prints nothing. Use it to see the I/O-vs-compute split that a sampling profiler
  blurs, and to confirm which stage a change actually moved. Representative
  numbers (webster/dickens-class text): compress L3 v1 is `lz_match` ~78% /
  `entropy_encode` ~15% / `transcode` ~5%; v1 decode is `reconstruct` ~59%
  (match copy) / `entropy_decode` ~34%. `entropy_decode` reports `-` for MB/s
  (entered once per FSE stream, not once per raw byte); per-stage `bytes` is
  billed at one representative entry per stage so the optimal matcher's second
  pass does not double-count.
- **Instruction/cache attribution (external).** `build-relprof`
  (`RelWithDebInfo`, frame pointers kept) feeds `perf record`/`callgrind` for
  per-symbol instruction counts and D1/LL miss rates — the numbers cited
  throughout this doc (e.g. `parc_lz_chain` ~68% of compress Ir, the
  parallel-vs-AoS `parc_fdec` cachegrind comparison). The stage timers say
  *which stage*; callgrind says *which instruction and why*.

## Bitstream (`src/util/bitstream.c`) — the hottest shared path

- **[DONE]** **Refill the reader 8 bytes at a time.** `parc_br_get` and
  `parc_br_peek` refilled the accumulator one byte per loop iteration. Both now
  route through `br_fill()` (`bitstream.c`): when `pos + 8 <= len` a single
  unaligned little-endian 64-bit load (`bs_read_le64`, endian-safe like
  `xxh64.c`) tops the accumulator to ≥57 bits in one step; the byte-at-a-time
  loop remains only as the tail fallback. To keep the LSB-first accumulator
  model exactly (`pos`/`nbits`/`bits_consumed` are byte-granular), only whole
  bytes are merged per load (`take = floor((64 - nbits) / 8)`), so the fast
  path is bit-identical to the byte loop it replaces — golden/format tests
  unchanged, clean under ASan+UBSan. ~10% faster end-to-end verify on webster
  (frame timing is diluted by checksum + match copy; the isolated
  entropy-symbol path gains more).
- **[DONE]** **Flush the writer 8 bytes at a time.** `parc_bw_put` drained one
  byte per loop iteration with a bounds check per byte. It now has a fast path
  (`bitstream.c`): when `pos + 8 <= cap` a single little-endian 64-bit store
  (`bs_write_le64`, endian-safe and symmetric to `bs_read_le64`) drains every
  pending whole byte after one capacity check. All 8 acc bytes are stored but
  `pos` only advances by `whole = nbits >> 3` (1..8), so the residual < 8 bits
  stay in `acc` for the next put; the extra stored bytes sit at uncommitted
  indices (`pos` is the authoritative length) and are overwritten by later puts
  or trimmed at finish, so the fast path is bit-identical to the byte loop it
  replaces. The byte-at-a-time loop remains as the tail fallback (`pos + 8 >
  cap`), preserving the exact sticky-`failed` contract. Golden/format tests
  unchanged (wire output byte-identical), clean under ASan+UBSan, all 126 tests
  pass. ~3-4% faster end-to-end compress on writer-heavy paths (L1 fmt1, L1
  Huffman fmt0 on enwik8); frame timing is diluted by the matcher + checksum,
  so higher levels are matcher-bound and the isolated bitstream-write path
  gains more.
- **[DONE]** **Make `parc_bw_put` / `parc_br_get` / `parc_br_peek` `static
  inline` in the header.** They are called once per symbol/extra-bits field
  from four different translation units. The three hot functions plus their
  helpers (`parc_bs_read_le64`/`parc_bs_write_le64`/`parc_br_fill`, renamed
  from the file-local `bs_*`/`br_fill` and now prefixed since they live in a
  shared header) moved verbatim into `bitstream.h` as `static inline`; only the
  cold lifecycle/query functions (`parc_bw_init`/`parc_bw_finish`/
  `parc_br_init`/`parc_br_err`/`parc_br_bits_consumed`) remain out-of-line in
  `bitstream.c`. This guarantees inlining in *every* build (not only under the
  release preset's LTO) and lets the compiler keep `acc`/`nbits` in registers
  across consecutive puts/gets in the same caller. Pure perf, no format impact:
  wire output byte-identical (webster f0/f1 `cmp`-equal to the pre-change
  binary, golden/round-trip tests unchanged), clean under `-Wconversion
  -Werror` and ASan+UBSan, all 126 tests pass. Best-of-8 end-to-end on webster:
  decode −13% (verify f0, Huffman) and −5% (verify f1, FSE) — those paths are
  reader-bound; compress stayed within run-to-run noise (~1%) because it is
  matcher-bound and the writer path is diluted, exactly as the earlier
  bitstream items predicted.
- **Hoist the per-call overhead out of decode loops.** Every `parc_br_get`
  recomputes `avail = nbits + (len - pos) * 8` (a multiply) and re-tests
  `failed` (`bitstream.c:87-93`). A "local reader" pattern — copy
  `acc/nbits/pos` into locals, refill once per loop iteration, validate once
  at the end (the sticky-error contract already permits exactly this) —
  removes a branch, a multiply, and two memory round-trips per symbol.

## LZ decode — match copy (`src/codec/block.c`)

- **[DONE]** **Replace the byte-by-byte match copy with wide copies.** Both
  `blk_decompress_v0` and `blk_decompress_v1` did `dst[pos+k] =
  dst[pos+k-dist]` one byte at a time, always — this is usually the #1 cost in
  an LZ decoder. Now routed through `copy_match()` (`block.c`), which seeds one
  period and grows the run by doubling (bulk `memcpy` of disjoint ranges),
  handling all offsets incl. `dist=1` with no output-buffer slack. Bit-
  identical wire output; +9–17% decode on match-heavy files (nci, reymont,
  kennedy.xls), a few % on short-match text. Original notes for reference:
  - `dist >= 8` (or 16): copy in 8/16-byte chunks (`memcpy` per chunk;
    overlap at ≥8 distance is safe for 8-byte chunks).
  - `dist < 8`: use the offset-doubling trick — replicate the pattern until
    it is ≥8 bytes wide, then wide-copy.
- **[DONE]** **Unconditional wildcopy for matches and literals.** `copy_match`
  and the v1 per-sequence literal copy now go through `wild_copy()` (`block.c`),
  which copies in unconditional 16-byte chunks (one `movups`) rounding the length
  up, with no per-chunk length dispatch — the libc `memcpy` size-class branch was
  the bulk of the cost since most LZ copies are short. `dist >= 16` matches
  wildcopy directly (source/dest stay ≥16 apart, so each chunk is disjoint);
  `dist < 16` keeps the correct period-doubling (a fixed 16-back source only
  reproduces the period when `dist | 16`). Every decode destination and the
  literal source now carry `PARC_WILDCOPY_SLACK` (32) trailing bytes so the
  ≤15-byte overrun stays in-bounds: `raw` (`frame.c`), `s->out` (`mt.c`),
  `dx->lit` (`block.c`); internal callers of `parc_blk_decompress` (the block
  tests) size `dst` accordingly. No format impact — bit-identical roundtrip on
  enwik8, all 132 tests pass under release and ASan (incl.
  `BlockDecode.SurvivesArbitraryGarbage`). Measured enwik8 L3 verify:
  `reconstruct` 136.7 ms → 83.4 ms (732 → 1200 MB/s), total decode 243 → 190 ms
  (1.28×); real `decompress` ~410 → ~550 MB/s. `entropy_decode` (the FSE literal
  stream) is now the dominant decode stage (47%), pointing at interleaved FSE
  states (v2) next. Original notes for reference:
  - `dist >= 8` (or 16): copy in 8/16-byte chunks (`memcpy` per chunk;
    overlap at ≥8 distance is safe for 8-byte chunks).
  - `dist < 8`: use the offset-doubling trick — replicate the pattern until
    it is ≥8 bytes wide, then wide-copy.

## LZ compress — matchers (`src/codec/lz.c`)

- **[DONE]** **Extend matches 8 bytes at a time.** The greedy extension loop
  and the rescan inside `longest_match` compared one byte per iteration. Both
  now route through `match_len()` (`lz.c`): it XORs two native-order 64-bit
  words per step and locates the first differing byte with `__builtin_ctzll`
  (little-endian) / `__builtin_clzll` (big-endian) `>> 3`, then finishes the
  last `< 8` bytes byte-wise. Callers pass `max = n - i` with the second
  position as the wide-load base, so `b + max == n` bounds every load inside
  the buffer (confirmed clean under ASan+UBSan). Output is bit-identical — it
  finds the same matches, just faster: all 32 corpus outputs (4 files × levels
  1/3/6/9 × formats 0/1) byte-for-byte unchanged, golden tests unchanged.
  Compress speedup grows with level as predicted: ~5% (L1), ~13% (L6), ~11%
  (L9) on nci.
- **[DONE]** **Prefetch the next chain candidate.** `longest_match` now reads
  `next = prev[cand]` up front and issues `__builtin_prefetch(&prev[next])` +
  `__builtin_prefetch(src + next)` before comparing the current candidate, so
  the cache-missing `prev[]` pointer chase and the random `src[cand]` probe
  overlap the current candidate's work. `next == NO_POS` yields wild prefetch
  addresses, which is safe (`__builtin_prefetch` never faults; ASan does not
  instrument it). Bit-identical (same matches found); marginal alone on the
  shallow fast-tier chains, additive with the depth retune below.
- **[TRIED — REVERTED]** **Start the rescan from where it can win.**
  `longest_match`/`find_matches` guard with `src[cand + best] == src[i + best]`
  then re-compare from `l = 0`. Widening the guard to the 8-byte window ending
  at `best` (one `read64==read64` when `best >= 7`, byte compare otherwise) is
  bit-identical — a window mismatch implies the common prefix is `<= best`, so
  no candidate is wrongly dropped. But it measured **worse**: `parc_lz_chain`
  +12.6% instructions (668.8M → 753.4M, webster 12 MiB L3). `match_len` is
  already a wide, inlined 8-byte-at-a-time compare, so the "rescan" it would
  skip is cheap, while the wider guard adds two loads and a `best >= 7` branch
  to *every* candidate — most of which the single-byte guard already rejects.
  Not worth it for this matcher; left as-is.
- **[DONE]** **Hash 5 bytes instead of 4 in the chain matcher.** `parc_lz_chain`
  (levels 2–7) now hashes 5 bytes (`hash5`/`read5`, one masked 64-bit load;
  greedy L1 keeps `hash4`), so each chain holds only positions sharing a 5-byte
  prefix — truer candidates that reach real matches with less walking. Measured:
  ~2% *better* ratio at roughly neutral speed on its own (the 5-byte read must be
  a single wide masked load, not a 5-byte `memcpy`, which was a 2× trap). The real
  payoff came from spending that ratio cushion on **halved fast-tier chain depths**
  (levels 2–5 `max_chain` {8,16,32,64}→{4,8,16,32}): webster L3/L4/L5 compress
  1.33×/1.82×/2.43× faster at flat ratio; alice 1.2–1.4×; structured/binary trades
  ~1–2% ratio for the speed. Wire-neutral (changes which matches are found, decode
  untouched); ratio ladder stays monotonic. `read5` loads 8 bytes so the last <8
  positions of a block are left unchained (they can only start a short match).
- **Skip the 128 KiB `memset` of the hash table per block.** Both matchers
  clear `head`/`htab` every block (`lz.c:27`, `lz.c:132`). For the 4 MiB
  default block this is noise, but with small `block_log` it is a real
  per-block tax. A generation/epoch tag packed into the table entry (e.g.
  16-bit epoch + position) or position-offset validation removes the clear
  entirely.
- **Fuse the histogram pass into tokenization.** Both `blk_compress_v0`
  (`block.c:145-153`) and `blk_compress_v1` (`block.c:313-351`) write the
  full token array (8 bytes/token) to memory, then immediately re-walk it to
  histogram / transcode. Counting frequencies while emitting tokens saves one
  full pass over a multi-megabyte array (cache traffic, not ALU).

### Matcher throughput — the dominant compress cost (trades ratio)

A callgrind profile of the default level (L3, `parc_lz_chain`, webster 12 MiB)
puts **`parc_lz_chain` at ~68% of compress instructions**, with
`parc_fse_encode` (~14%) and `encode_block` (~14%) next. Compress is also
~4–5× slower than decompress end-to-end, so the matcher is *the* speed lever.
Unlike the entropy/bitstream items above, the ratio-neutral matcher wins are
now largely exhausted (the guard-widening experiment above measured worse), so
further matcher speed **costs compression ratio** and must be justified per step
on the full `speed_vs_ratio` corpus (`tools/plot_bench.py --run`). Two open
directions, in increasing ratio budget:

- **Small ratio budget (~1%): retune the fast-tier chain search.** Tighten
  `max_chain`/`nice_len` for levels 2–5 (`parc_lz_cfg_for_level`, `lz.c`) and/or
  add cheaper early-exits (e.g. break the chain walk once a `nice_len`-class
  match is found, or cap total `match_len` work per position), keeping each
  level's ratio regression under ~1% on the corpus. Conservative — preserves the
  ladder's shape; this is a continuation of the 5-byte-hash + halved-depth retune
  already on this branch. Measure the ladder stays monotonic.
- **Larger budget: a genuinely fast tier for L1–L2.** Replace the hash-chain
  walk at the fast end with an LZ4-style structure — a single-entry hash table
  (or a small N-way bucket, N≈2–4) probed once per position, no chain pointer
  chase — accepting a few % ratio loss for a large throughput jump at the fast
  end. Bigger change (new matcher path + level wiring, `lz.c`/`block.c`), and it
  reshapes where L1–L2 sit on the speed-vs-ratio curve; worth it only if a fast
  tier well below zstd-3's ratio but at much higher speed is a goal for the
  ladder.

## Entropy coders

### FSE (`src/codec/fse.c`)

- **[DONE]** **Batch bit reads in `parc_fse_decode`.** The loop made one
  `parc_br_get` call per symbol — call overhead, sticky-error branch, avail
  multiply, and a fill each time. It now runs a local reader (`fse.c`):
  `acc`/`nbits`/`pos` are copied into locals held in registers across the
  whole loop, and a `FSE_REFILL(need)` macro tops the accumulator up from one
  unaligned 64-bit load (`parc_bs_read_le64`, the same fast path as
  `parc_br_fill`) whenever it dips below the bits about to be read. Since
  `table_log <= 12`, one ≥57-bit refill feeds ≥4 symbols, so the per-symbol
  body drops to a table lookup, a shift, and a single `nbits < nb` compare;
  truncation is tested once per refill (near the tail) instead of per get,
  which the sticky-error contract permits. The refill pulls the same whole
  bytes in the same order as the byte-at-a-time reader and keeps
  `pos*8 - nbits` equal to `bits_consumed`, so the reader state written back
  to `r` is exactly what the per-`get` path would have left for the
  downstream streams (ml/of/literal FSE tables, extra-bits, padding check) —
  bit-identical wire behavior. Decode-only, ratio unchanged: golden/round-trip
  tests unchanged, all 126 tests pass, clean under ASan+UBSan (incl.
  `BlockDecode.SurvivesArbitraryGarbage`, which drives the truncation path)
  and under clang `-Wconversion -Werror`. Best-of-3 end-to-end v1 verify:
  reymont +15%, xml +9%, enwik8 +8%, webster +8%, nci +6% — the gain scales
  with how FSE-decode-bound the file is, exactly as predicted. (The
  last-iteration `if (i + 1 < count)` branch is left in place; peeling it
  showed no measurable gain and the branch predicts near-perfectly.)
- **Interleave 2–4 FSE states.** A single ANS state is a serial dependency
  chain (state → table → state); modern cores can run 2-4 chains in parallel.
  Encoding alternate symbols with independent states doubles decode
  throughput. **[format change]** — worth considering for a v2, it is why
  zstd uses interleaved streams.
- **[DONE, PARTIAL]** **Emit the `grp` array through a register-held
  accumulator.** `parc_fse_encode`'s second pass called `parc_bw_put` per
  group; since `w` is a pointer the compiler cannot prove `w->dst` and `grp`
  don't alias, so `acc`/`nbits`/`pos` round-tripped to memory every symbol.
  That loop now copies the writer state into locals held in registers across
  the whole emit and inlines `parc_bw_put`'s body (same 8-byte fast-path store,
  same per-byte tail, same sticky-`failed` contract — bit-identical wire
  output; the discarded-on-overflow bitstream makes the post-failure state
  divergence unobservable). This is the encode-side mirror of the
  `parc_fse_decode` local-reader. Measured: `parc_fse_encode` −16% instructions
  (163.1M → 137.0M, webster 12 MiB L3), −2.6% program total; bit-identical
  across all corpus files × levels 3/6/9 × formats 0/1, clean under ASan+UBSan,
  132 tests pass. *Still open:* the first pass still writes `grp[]` then re-reads
  it (two memory passes); collapsing to one pass would need a reverse-built
  scratch buffer.
- **[DONE]** **Use a multi-lane histogram in `emit_fse_stream`.** The single
  `freq[syms[i]]++` loop stalled on store-to-load forwarding whenever nearby
  symbols repeat (very common in literals). Now `hist_u8` (`block.c`)
  accumulates into four independent 256-entry tables (processing 4 symbols per
  iteration) and sums them at the end (the zstd `HIST_count` trick), breaking
  the dependency so the increments pipeline. Bit-identical counts; the
  histogram cost roughly halved (~18.7M → 10.4M instructions on webster L3,
  −0.85% program total on top of the emit change), same tests/sanitizers clean.
- **Cheapen `parc_fse_normalize`'s leftover loop.** Distributing the rounding
  leftover is an O(alphabet) scan *per unit* of leftover (`fse.c:73-87`);
  worst case that is O(alphabet × table_size). Fine today, but a
  single-pass largest-remainder selection (partial sort / heap over
  remainders) makes table build O(alphabet log alphabet) worst case.

### Huffman (`src/codec/huffman.c`)

- **Decode from a local bit buffer instead of peek+get per symbol.**
  `parc_hdec_get` (`huffman.c:260-272`) does a `parc_br_peek` (refill loop)
  then a `parc_br_get` (avail check + second pass over the same state) for
  every symbol. With a guaranteed-filled local buffer: one table lookup, one
  shift by `len`, refill every few symbols. Combined with the 8-byte refill
  this is the classic libdeflate structure, several times faster than the
  current per-symbol path.
- **Pack "symbol + extra bits count" decode into one step.** In v0 decode,
  a match costs: main symbol lookup → `bucket_val` get → dist symbol lookup →
  `bucket_val` get, each a separate reader call (`block.c:222-247`). With a
  local bit buffer these fuse into straight-line code; the length-bucket
  table cells could also pre-store `base` and `nbits` so decode is
  `base + peek(nbits)` without the branch in `bucket_val` (`block.c:40-45`).
- **Two-symbol root-table entries.** When two consecutive codes fit in
  `root_bits` (common: literal alphabets average ≲8 bits), a wider table can
  emit 2 symbols per lookup (libdeflate/zlib-ng style). Decoder-only change,
  no format impact; costs a bigger table build per block.
- **Table-driven `bit_reverse`.** `bit_reverse` (`huffman.c:144-152`) loops
  bit-by-bit and runs once per code cell during `parc_hdec_init`/
  `parc_henc_init` — per block. A 256-entry byte-reverse LUT (two lookups +
  shift) shrinks per-block table-build time; matters for small blocks.

## v1 sequence decode (`src/codec/block.c`)

- **[done] Batched extra-bits reads.** The ll/ml/of extra-bits loops each ran
  a full `parc_br_get` (avail multiply + sticky branch + refill) per sequence.
  They now share `decode_bucket_extras`, a register-held local reader
  (acc/nbits/pos in registers, one wide refill per several fields, truncation
  tested once per refill) that is `always_inline`d so each call specializes on
  its constant `sub`/`add` and the `b == 0` branch folds. The offset pass reads
  new-offset extras and resolves the recent-offset cache (scalar r0/r1/r2 slots,
  not an indexed rotate) in a single fused pass over `sym[]`/`dist[]`.
  Bit-exact. Net −10.5% instructions on dickens decode; +4–12% wall-clock on
  decode-bound text (alice29, plrabn12, webster, samba). The remaining
  format-mandated separation (symbols, then extras, in distinct stream regions)
  still forces one pass per stream. A further idea: table-ize `(base[b],
  nbits[b])` so even the `b == 0` test disappears — untried.
- **[remaining] Faster literal decode.** After the above, `read_fse_stream`
  (dominated by the literal-stream FSE decode) is the largest addressable chunk
  (~31%). Note the "two-symbol per lookup" trick does **not** transfer from
  Huffman to FSE/tANS: each transition consumes a data-dependent number of bits
  and the next state depends on the actual bits read, so a single state index
  cannot precompute the second symbol. The only genuine FSE-decode speedup left
  is **interleaving 2–4 states** (see the FSE section) — a **[format change]**
  for a v2, since it reshapes the bitstream.
- **[TRIED — REVERTED]** **Pack `parc_fdec` into an array-of-structs.** The
  decode table is three parallel arrays (`new_state`/`symbol`/`nbits`) indexed
  by state; the theory (zstd's `FSE_decode_t` layout) was that folding them into
  one 4-byte-per-cell array cuts the hot decode step (`parc_fse_decode`) from
  three loads to one and improves locality. Measured **worse**: +3.0%
  instructions on reymont decode (143.7M → 148.0M total-Ir, the delta is all in
  FSE decode since nothing else changed), and a 1–4% wall-clock regression on
  webster/reymont/xml verify. Cachegrind explains it: **D1 miss rate and LL refs
  were identical** before and after — the tables (`table_log <= 12` ⇒ ≤ 16 KB)
  are already L1-resident, so packing removes no cache misses, and the AoS cell
  only *adds* sub-word extraction (byte/`u16` reads out of the packed word) that
  the three scaled-index loads avoided. The premise (three cache-missing
  accesses per symbol) does not hold at this table size. Left as parallel
  arrays.

## Checksumming (`src/util/xxh64.c`, frame layer)

- **Stop hashing every byte twice.** On compress, each block is hashed once
  for the block header (`frame.c:165`) and again for the stream hash
  (`frame.c:178`) — two full passes over all input (and the MT reader thread
  does the stream hash serially, `frame_mt.c:44`, making it a pipeline
  bottleneck at high thread counts). Both hashes are format-pinned, but the
  two updates can be fused into one pass that feeds both states from each
  32-byte chunk while it is hot in L1 — roughly halves checksum cost from
  the memory system's point of view. Same story on decompress
  (`frame.c:281` + `frame.c:296`).
- **XXH3 instead of XXH64.** XXH64 is scalar-only by construction (~10
  GB/s); XXH3 vectorizes (AVX2/NEON, ~30-60 GB/s). **[format change]** —
  candidate for a v2 frame.

## Multithreading (`src/codec/mt.c`, `src/codec/frame_mt.c`)

- **Move per-block work off the reader thread.** On MT compress the reader
  does `fread` + stream-hash; on MT decompress the reader also parses headers
  and appends index entries. The stream hash is order-serial but does not
  have to run on the *reader*: shifting it to the writer thread (compress:
  `s->in` is still valid at write time) rebalances the pipeline when the
  reader is the bottleneck.
- **Overlap I/O with `posix_fadvise`/readahead and bigger stdio buffers.**
  Block reads are sequential `fread`s of `bs` bytes; `setvbuf` with a large
  buffer (or switching the frame layer to `pread`/`readv`) reduces syscall
  and copy overhead, especially at small `block_log`.
- **Use the index for parallel random-access decompression.** The trailer
  index records every block's offset/lengths, but decompression streams
  blocks through a serial reader. For seekable inputs, workers could `pread`
  their own blocks directly — removes the reader stage entirely. (The serial
  stream hash still needs an ordered pass, but it can ride the writer.)

## Build / toolchain

- **Ship `-march` variants or runtime dispatch.** The release preset is
  `-O3` + LTO but baseline x86-64: no BMI2 (`shlx`/`shrx` — big for
  bitstream shifts), no AVX2. Either add `-march=x86-64-v3` when acceptable,
  or compile the hot decode/encode kernels twice and dispatch on CPUID
  (what zstd/libdeflate do).
- **Profile-guided optimization.** The codec is branch-heavy
  (literal-vs-match, bucket sizes); a PGO pass over the corpus benchmark
  typically buys 5-10% on this kind of code.
- **`__builtin_expect` on the sticky-error branches.** `failed`,
  `PARC_ERR_*` paths and the `cand == NO_POS` misses are strongly biased;
  annotating them keeps the fall-through path straight-line. Cheap, small,
  additive.
