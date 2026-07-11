# parc frame format, versions 0 and 1

This document is the normative specification of the parc compressed frame
format, versions 0 and 1. The implementation follows this document; where they
disagree, this document wins and the implementation is buggy.

The frame layout (§1) is shared by both versions; the header's version byte
selects the packed-block coding: **version 0** (§2) codes a flat token stream
with canonical Huffman; **version 1** (§3) codes a sequence model with FSE and
repeat-offset codes. Stored blocks and every frame-level structure (headers,
hashes, index, trailer) are identical across versions. A version-1 decoder
MUST also accept version-0 frames.

Conventions:

- All multi-byte integers are **little-endian**, unsigned.
- `u8`/`u32`/`u64` denote 1/4/8-byte unsigned integers.
- Bitstreams are **LSB-first**: successive fields occupy increasingly
  significant bits of each byte, then successive bytes (the pinned order of
  `src/util/bitstream.h`; writing 1 (1 bit), 2 (2 bits), 0x13 (5 bits)
  produces the single byte 0x9D).
- "xxh64(x)" is XXH64 of x with seed 0.
- A decoder MUST treat any violation of a MUST below as an error and produce
  no output beyond what was already emitted. It must never crash, hang, or
  read/write out of bounds on any input.

## 1. Frame layout

```
Frame  := Header Block* EndMarker Trailer Footer
```

A frame encodes exactly one byte string (the *content*), possibly empty.
A decoder MUST reject bytes following the Footer (no concatenation in v0).

### 1.1 Header (8 bytes)

| offset | size | field     | value                                        |
|--------|------|-----------|----------------------------------------------|
| 0      | 4    | magic     | `70 41 72 63` (ASCII "pArc")                 |
| 4      | 1    | version   | 0 or 1 (selects packed coding; see §2, §3)   |
| 5      | 1    | flags     | 0 (no flags defined; nonzero MUST be rejected as unsupported) |
| 6      | 1    | block_log | log2 of the maximum block size; MUST be in [12, 24] |
| 7      | 1    | reserved  | 0 (nonzero MUST be rejected)                 |

`block_size = 1 << block_log`. It is an upper bound on the raw size of every
block in the frame; encoders are free to emit shorter blocks anywhere.

### 1.2 Blocks

Zero or more blocks, back to back. Empty content has zero blocks.

```
Block := type:u8  raw_len:u32  comp_len:u32  raw_hash:u64  payload[comp_len]
```

- `type` MUST be 0x00 (**stored**) or 0x01 (**packed**). 0xFF is the end
  marker (§1.3) and terminates the block sequence; any other value MUST be
  rejected.
- `raw_len` is the number of content bytes this block decodes to. MUST
  satisfy `1 <= raw_len <= block_size`.
- `comp_len` is the payload size in bytes. For stored blocks it MUST equal
  `raw_len`; for packed blocks it MUST satisfy `1 <= comp_len < raw_len`
  (a packed encoding that does not beat stored MUST be emitted as stored).
- `raw_hash` MUST equal xxh64 of the block's decoded bytes.

Stored payload is the raw content bytes verbatim. Packed payload is specified
in §2 (version 0) or §3 (version 1), selected by the frame's version byte.
Blocks are independent: a packed block never references bytes outside its own
raw content.

### 1.3 End marker, trailer, footer

```
EndMarker := 0xFF
Trailer   := block_count:u32  IndexEntry[block_count]  total_raw:u64  stream_hash:u64
IndexEntry:= offset:u64  raw_len:u32  comp_len:u32
Footer    := trailer_len:u32  end_magic (4 bytes: 70 45 6E 64, ASCII "pEnd")
```

- `block_count` MUST equal the number of blocks in the frame.
- Index entry *i* describes block *i* in order: `offset` is the byte offset
  from the start of the frame to the block's `type` byte; `raw_len` and
  `comp_len` MUST equal the block's header fields. Since blocks are
  contiguous, `offset[0] = 8` and
  `offset[i+1] = offset[i] + 17 + comp_len[i]`; a decoder MUST verify every
  entry against the blocks it read.
- `total_raw` MUST equal the sum of all `raw_len` (the content length).
- `stream_hash` MUST equal xxh64 of the entire content.
- `trailer_len` MUST equal the byte count from the EndMarker through the end
  of the Footer inclusive, i.e. `29 + 16 * block_count`. It lets a seeking
  reader locate the index from the end of the frame.

The index is redundant with the block headers by design: it exists so a
seeking decoder can find block boundaries without scanning, and sequential
decoders use it as an integrity cross-check.

## 2. Packed block payload, version 0

Used when the frame's version byte is 0. A packed payload is one LSB-first
bitstream. Trailing bits of the final payload byte (fewer than 8) MUST be
zero, and `comp_len` MUST equal the minimal byte count holding the bitstream,
i.e. `ceil(bits/8)`. These two rules (exact bit accounting) apply to the
version-1 payload of §3 as well.

### 2.1 Value buckets

Match lengths and distances are coded as a *bucket symbol* plus extra bits.
For an integer `v >= 0`:

- `bucket(v) = 0` if `v == 0`, else `bit_length(v)` (index of highest set
  bit, plus one; `bucket(1) = 1`, `bucket(2..3) = 2`, `bucket(4..7) = 3`, …).
- For bucket `b >= 1`, the extra field is `b - 1` bits holding
  `v - 2^(b-1)` (LSB-first). Buckets 0 and 1 have no extra bits.

Since `raw_len <= 2^24`, buckets never exceed 24.

### 2.2 Alphabets

- **Main alphabet**, 282 symbols:
  - 0–255: literal byte values
  - 256: end-of-block (EOB)
  - 257 + b (b in 0..24): match-length bucket `b` of `v = length - 4`
- **Distance alphabet**, 25 symbols: 0–24 are distance buckets of
  `v = distance - 1`.

Minimum match length is 4. Distance `d` counts back from the current
position within the block's raw content; `1 <= d <= bytes decoded so far`.

### 2.3 Code length tables

The payload begins with canonical-Huffman code lengths, 4 bits each
(values 0–15; 0 = symbol absent): 282 main lengths, then 25 distance
lengths (1228 bits total).

Each table MUST be either:

- **complete**: the Kraft sum `Σ 2^(15-len)` over present symbols equals
  `2^15` exactly; or
- **degenerate**: exactly one present symbol, with length 1 (its code is the
  single bit 0; a decoder reading a 1 bit where a degenerate code is
  expected MUST reject the frame); or
- **empty** (distance table only): all lengths 0. The main table MUST NOT be
  empty (EOB always occurs). If the distance table is empty, no
  match-length symbol may appear in the token stream.

Any other table (over- or under-subscribed) MUST be rejected.

Canonical code assignment: codes are assigned in order of (length, symbol),
numerically increasing, shorter lengths first — identical to DEFLATE's rule.
Code bits are written to the bitstream **most-significant bit first**, so a
decoder reading bit-by-bit computes `code = (code << 1) | bit` until the
code matches a symbol at the current length.

### 2.4 Token stream

After the tables, tokens follow until EOB:

- literal symbol (0–255): one content byte.
- length symbol `257 + b`: extra bits of bucket `b` (match length
  `= 4 + v`), then a distance symbol, then its extra bits
  (`distance = 1 + v`). The copy source is `distance` bytes back; overlap is
  allowed (`distance < length` repeats bytes, as in LZ77).
- EOB (256): end of token stream.

A decoder MUST verify: every match stays within already-decoded content
(`distance <= position`), decoded size never exceeds `raw_len`, EOB arrives
exactly at `raw_len` decoded bytes, the bitstream never reads past
`comp_len * 8` bits, and all remaining padding bits after EOB are zero.

## 3. Packed block payload, version 1

Used when the frame's version byte is 1. The payload is one LSB-first
bitstream with the same exact-accounting rules as §2 (minimal `comp_len`,
zero trailing padding). It codes a **sequence model**: the block's literals
form one byte stream, and each match is a *sequence* `(litLen, matchLen,
offCode)` where `litLen` is the count of literals immediately preceding the
match. Trailing literals after the last match belong to no sequence.

Value buckets (§2.1) are reused throughout.

### 3.1 FSE streams

Symbol streams are coded with table-driven asymmetric numeral coding (FSE).
Every FSE stream is self-describing: a **table description** followed by the
**coded symbols**.

**Table description**, in order: `table_log` (4 bits, MUST be in [5, 12]);
`max_symbol` (8 bits); then `max_symbol + 1` normalized counts, each
bucket-coded — a 4-bit bucket `b = bucket(count)` (§2.1) followed by `b - 1`
mantissa bits giving `count` (no mantissa for `b <= 1`). `b` MUST NOT exceed
`table_log + 1` (else the count could exceed the table size; a decoder MUST
reject a larger `b`). Let `T = 1 << table_log`. The counts MUST sum to exactly
`T`, and the count for `max_symbol` MUST be nonzero. A symbol is *present* iff
its count is nonzero.

**Table construction** (both sides build identical tables from the counts).
Spread the alphabet across `T` slots: with
`step = (T >> 1) + (T >> 3) + 3` and slot cursor starting at 0, visit symbols
in increasing order, placing each present symbol into the current slot `count`
times and advancing the cursor by `step` modulo `T` after each placement
(`step` is odd, so every slot is filled exactly once). For each slot `u` in
`0..T-1` holding symbol `s`, let `x` be `count[s]` on the first slot of `s` and
increment it per slot of `s`; then `nbits[u] = table_log - floor(log2(x))` and
`newbase[u] = (x << nbits[u]) - T`.

**Decoding** `n` symbols: read `state` as `table_log` bits. Then for
`i = 0..n-1`: output `symbol[state]`; and if `i < n-1`, read `nbits[state]`
bits as `low` and set `state = newbase[state] + low`. `n` is not stored in the
stream; it is known from context (below). Encoders MUST lay the bitstream so
this forward procedure recovers the symbols in order (standard tANS with the
message processed in reverse and the final state flushed first).

### 3.2 Payload structure

```
Payloadv1 := nSeq:u32(LSB-first, 32 bits)
             (if nSeq > 0: LLStream MLStream OFStream)
             LitStream
```

`nSeq` MUST satisfy `nSeq <= raw_len / 4` (every match covers at least
`MIN_MATCH = 4` bytes). Each of `LLStream`, `MLStream`, `OFStream` is an FSE
stream of `nSeq` symbols followed by that stream's per-sequence **extra bits**
(see below). `LitStream` is an FSE stream of `nLit` literal-byte symbols,
where `nLit = raw_len - Σ matchLen`. `nLit` MUST be `>= 1` (position 0 is
always a literal). The streams are contiguous; each is consumed exactly.

Alphabets and extra bits, per sequence `i`:

- **Literal length** `LLStream`: alphabet 0..24. Symbol `b = bucket(litLen)`;
  extra field `b - 1` bits (none for `b <= 1`) giving `litLen` per §2.1.
- **Match length** `MLStream`: alphabet 0..24. Symbol `b = bucket(matchLen -
  4)`; extra `b - 1` bits; `matchLen = 4 + value`.
- **Offset** `OFStream`: alphabet 0..27. Symbols 0,1,2 are repeat-offset
  codes; symbol `s >= 3` is a new offset with bucket `b = s - 3`, extra field
  `max(b - 1, 0)` bits, distance `= 1 + value` (`value` per §2.1 of `s - 3`).

Extra bits for a stream follow immediately after its coded symbols, in
sequence order `i = 0..nSeq-1`: for `LLStream`/`MLStream` the bucket extra of
`ll`/`ml`; for `OFStream` the offset extra only when `s >= 4` (i.e. bucket
`>= 1`).

### 3.3 Repeat offsets

A decoder maintains a 3-entry recent-offset cache initialised to
`recent = {1, 2, 3}` at the start of the block. For each sequence's offset
symbol `s`:

- `s < 3`: the distance is `recent[s]`; move that entry to the front, shifting
  the entries above it down (move-to-front).
- `s >= 3`: the distance is the decoded new offset; shift `recent` down by one
  (dropping `recent[2]`).

In both cases the resulting distance becomes `recent[0]`. Encoders emit the
smallest matching repeat code when a match's distance equals a cached offset,
and a new-offset code otherwise; the cache update is identical on both sides.

### 3.4 Reconstruction

Decode the three sequence streams (when `nSeq > 0`), then `LitStream`. Emit
output by walking the sequences in order: copy `litLen` literals from the
literal stream, then copy `matchLen` bytes from `distance` back (overlapping
copies repeat, as in §2.4); finally emit the remaining literals. A decoder
MUST verify: each `distance <= ` bytes emitted so far; each match stays within
`raw_len`; literals are not over-consumed; and the total emitted equals
`raw_len`. All of §2's exact-accounting checks (minimal `comp_len`, zero
padding) apply.

## 4. Integrity and error taxonomy

A conforming decoder verifies, in addition to all structural MUSTs above:

- per-block `raw_hash` (mismatch → checksum error),
- `stream_hash` over the whole content (mismatch → checksum error),
- index consistency, `total_raw`, `trailer_len`, `end_magic`,
- exact end of input after the Footer.

Recommended mapping to `parc_err`: unsupported version/flags →
`PARC_ERR_VERSION`; input ending mid-structure → `PARC_ERR_TRUNCATED`;
hash mismatches → `PARC_ERR_CHECKSUM`; any other violation →
`PARC_ERR_CORRUPT`.

## 5. Versioning

The version byte identifies the packed-payload coding (§2 for 0, §3 for 1);
the frame layout (§1), stored blocks, and all integrity structures are shared.
A version-1 decoder MUST accept both version-0 and version-1 frames and
dispatch the packed payload by the header's version byte; it MUST reject
versions greater than the highest it implements with a version error.

Both versions are frozen: golden fixtures exist for each (`tests/golden/`,
`*-v1.parc` for version 1), and any change to this document that alters bytes
on the wire for an existing version is a bug. New incompatible codings take
the next version number.
