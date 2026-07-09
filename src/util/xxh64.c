#include "util/xxh64.h"

#include <string.h>

#define PRIME64_1 0x9E3779B185EBCA87ULL
#define PRIME64_2 0xC2B2AE3D27D4EB4FULL
#define PRIME64_3 0x165667B19E3779F9ULL
#define PRIME64_4 0x85EBCA77C2B2AE63ULL
#define PRIME64_5 0x27D4EB2F165667C5ULL

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define PARC_XXH_LITTLE_ENDIAN 1
#else
#define PARC_XXH_LITTLE_ENDIAN 0
#endif

#if !PARC_XXH_LITTLE_ENDIAN
static uint64_t bswap64(uint64_t x)
{
    return ((x & 0x00000000000000FFULL) << 56) |
           ((x & 0x000000000000FF00ULL) << 40) |
           ((x & 0x0000000000FF0000ULL) << 24) |
           ((x & 0x00000000FF000000ULL) << 8)  |
           ((x & 0x000000FF00000000ULL) >> 8)  |
           ((x & 0x0000FF0000000000ULL) >> 24) |
           ((x & 0x00FF000000000000ULL) >> 40) |
           ((x & 0xFF00000000000000ULL) >> 56);
}

static uint32_t bswap32(uint32_t x)
{
    return ((x & 0x000000FFu) << 24) | ((x & 0x0000FF00u) << 8) |
           ((x & 0x00FF0000u) >> 8) | ((x & 0xFF000000u) >> 24);
}
#endif

/* Read little-endian words via memcpy (no unaligned dereference). */
static uint64_t read_le64(const void *p)
{
    uint64_t v;
    memcpy(&v, p, sizeof v);
#if !PARC_XXH_LITTLE_ENDIAN
    v = bswap64(v);
#endif
    return v;
}

static uint32_t read_le32(const void *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof v);
#if !PARC_XXH_LITTLE_ENDIAN
    v = bswap32(v);
#endif
    return v;
}

static uint64_t rotl64(uint64_t x, unsigned r)
{
    return (x << r) | (x >> (64u - r));
}

static uint64_t xxh64_round(uint64_t acc, uint64_t input)
{
    acc += input * PRIME64_2;
    acc = rotl64(acc, 31);
    acc *= PRIME64_1;
    return acc;
}

static uint64_t xxh64_merge_round(uint64_t acc, uint64_t val)
{
    val = xxh64_round(0, val);
    acc ^= val;
    acc = acc * PRIME64_1 + PRIME64_4;
    return acc;
}

static uint64_t xxh64_avalanche(uint64_t h)
{
    h ^= h >> 33;
    h *= PRIME64_2;
    h ^= h >> 29;
    h *= PRIME64_3;
    h ^= h >> 32;
    return h;
}

/* Finalize the trailing < 8 / < 4 / < 1 byte tails of [base+off, base+len). */
static uint64_t xxh64_finalize(uint64_t h64, const uint8_t *base, size_t off,
                                size_t len)
{
    while (off + 8 <= len) {
        uint64_t k1 = xxh64_round(0, read_le64(base + off));
        h64 ^= k1;
        h64 = rotl64(h64, 27) * PRIME64_1 + PRIME64_4;
        off += 8;
    }
    if (off + 4 <= len) {
        h64 ^= (uint64_t)read_le32(base + off) * PRIME64_1;
        h64 = rotl64(h64, 23) * PRIME64_2 + PRIME64_3;
        off += 4;
    }
    while (off < len) {
        h64 ^= (uint64_t)base[off] * PRIME64_5;
        h64 = rotl64(h64, 11) * PRIME64_1;
        off += 1;
    }
    return xxh64_avalanche(h64);
}

uint64_t parc_xxh64(const void *data, size_t len, uint64_t seed)
{
    const uint8_t *base = (const uint8_t *)data;
    size_t off = 0;
    uint64_t h64;

    if (len >= 32) {
        uint64_t v1 = seed + PRIME64_1 + PRIME64_2;
        uint64_t v2 = seed + PRIME64_2;
        uint64_t v3 = seed;
        uint64_t v4 = seed - PRIME64_1;
        size_t limit = len - 32;

        do {
            v1 = xxh64_round(v1, read_le64(base + off)); off += 8;
            v2 = xxh64_round(v2, read_le64(base + off)); off += 8;
            v3 = xxh64_round(v3, read_le64(base + off)); off += 8;
            v4 = xxh64_round(v4, read_le64(base + off)); off += 8;
        } while (off <= limit);

        h64 = rotl64(v1, 1) + rotl64(v2, 7) + rotl64(v3, 12) + rotl64(v4, 18);
        h64 = xxh64_merge_round(h64, v1);
        h64 = xxh64_merge_round(h64, v2);
        h64 = xxh64_merge_round(h64, v3);
        h64 = xxh64_merge_round(h64, v4);
    } else {
        h64 = seed + PRIME64_5;
    }

    h64 += (uint64_t)len;
    return xxh64_finalize(h64, base, off, len);
}

void parc_xxh64_init(parc_xxh64_state *st, uint64_t seed)
{
    st->acc[0] = seed + PRIME64_1 + PRIME64_2;
    st->acc[1] = seed + PRIME64_2;
    st->acc[2] = seed;
    st->acc[3] = seed - PRIME64_1;
    st->total_len = 0;
    st->seed = seed;
    st->buf_len = 0;
}

void parc_xxh64_update(parc_xxh64_state *st, const void *data, size_t len)
{
    if (len == 0)
        return;

    const uint8_t *base = (const uint8_t *)data;
    size_t off = 0;
    st->total_len += (uint64_t)len;

    if (st->buf_len + len < 32) {
        memcpy(st->buf + st->buf_len, base, len);
        st->buf_len += len;
        return;
    }

    if (st->buf_len > 0) {
        size_t fill = 32 - st->buf_len;
        memcpy(st->buf + st->buf_len, base, fill);
        st->acc[0] = xxh64_round(st->acc[0], read_le64(st->buf + 0));
        st->acc[1] = xxh64_round(st->acc[1], read_le64(st->buf + 8));
        st->acc[2] = xxh64_round(st->acc[2], read_le64(st->buf + 16));
        st->acc[3] = xxh64_round(st->acc[3], read_le64(st->buf + 24));
        off += fill;
        st->buf_len = 0;
    }

    if (off + 32 <= len) {
        size_t limit = len - 32;
        do {
            st->acc[0] = xxh64_round(st->acc[0], read_le64(base + off)); off += 8;
            st->acc[1] = xxh64_round(st->acc[1], read_le64(base + off)); off += 8;
            st->acc[2] = xxh64_round(st->acc[2], read_le64(base + off)); off += 8;
            st->acc[3] = xxh64_round(st->acc[3], read_le64(base + off)); off += 8;
        } while (off <= limit);
    }

    if (off < len) {
        size_t rem = len - off;
        memcpy(st->buf, base + off, rem);
        st->buf_len = rem;
    }
}

uint64_t parc_xxh64_digest(const parc_xxh64_state *st)
{
    uint64_t h64;

    if (st->total_len >= 32) {
        uint64_t v1 = st->acc[0];
        uint64_t v2 = st->acc[1];
        uint64_t v3 = st->acc[2];
        uint64_t v4 = st->acc[3];
        h64 = rotl64(v1, 1) + rotl64(v2, 7) + rotl64(v3, 12) + rotl64(v4, 18);
        h64 = xxh64_merge_round(h64, v1);
        h64 = xxh64_merge_round(h64, v2);
        h64 = xxh64_merge_round(h64, v3);
        h64 = xxh64_merge_round(h64, v4);
    } else {
        h64 = st->seed + PRIME64_5;
    }

    h64 += st->total_len;
    return xxh64_finalize(h64, st->buf, 0, st->buf_len);
}
