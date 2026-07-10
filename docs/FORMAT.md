# parc frame format, version 0

This document is the normative specification of the parc compressed frame
format, version 0. The implementation follows this document; where they
disagree, this document wins and the implementation is buggy.

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
| 4      | 1    | version   | 0                                            |
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

Stored payload is the raw content bytes verbatim. Packed payload is
specified in §2. Blocks are independent: a packed block never references
bytes outside its own raw content.

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

## 2. Packed block payload

A packed payload is one LSB-first bitstream. Trailing bits of the final
payload byte (fewer than 8) MUST be zero, and `comp_len` MUST equal the
minimal byte count holding the bitstream, i.e. `ceil(bits/8)`.

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

## 3. Integrity and error taxonomy

A conforming decoder verifies, in addition to all structural MUSTs above:

- per-block `raw_hash` (mismatch → checksum error),
- `stream_hash` over the whole content (mismatch → checksum error),
- index consistency, `total_raw`, `trailer_len`, `end_magic`,
- exact end of input after the Footer.

Recommended mapping to `parc_err`: unsupported version/flags →
`PARC_ERR_VERSION`; input ending mid-structure → `PARC_ERR_TRUNCATED`;
hash mismatches → `PARC_ERR_CHECKSUM`; any other violation →
`PARC_ERR_CORRUPT`.

## 4. Versioning

The version byte identifies the frame layout and packed-payload coding as a
whole. Incompatible changes bump it; decoders reject versions they do not
implement. Version 0 is frozen once golden fixtures exist
(`tests/golden/`): any change to this document that alters bytes on the
wire requires version 1.
