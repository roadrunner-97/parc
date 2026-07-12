#include "codec/lz.h"

#include <assert.h>
#include <math.h>
#include <string.h>

#define HASH_SIZE (1u << PARC_LZ_HASH_BITS)
#define NO_POS UINT32_MAX

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define PARC_LZ_LITTLE_ENDIAN 1
#else
#define PARC_LZ_LITTLE_ENDIAN 0
#endif

static uint32_t read32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4); /* byte order irrelevant: only compared to itself */
    return v;
}

static uint64_t read64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, 8); /* native load; only ever compared to another read64 */
    return v;
}

static uint32_t hash4(uint32_t v)
{
    return (v * 2654435761u) >> (32 - PARC_LZ_HASH_BITS);
}

/* Read the 5 bytes at p into the low 40 bits of a word (zero-extended). One
 * wide load masked to 40 bits — cheaper than a 5-byte memcpy on the hot path.
 * Reads 8 bytes, so requires p + 8 <= end; callers guard the block tail. Only
 * ever fed to hash5 and compared to itself, so byte order is irrelevant. */
static uint64_t read5(const uint8_t *p)
{
    return read64(p) & 0xFFFFFFFFFFULL;
}

/* Hash the 5-byte key in v's low 40 bits. Multiply-shift with a 64-bit odd
 * constant, taking the top PARC_LZ_HASH_BITS bits. Hashing 5 bytes (vs 4) puts
 * only positions sharing a 5-byte prefix on each chain, so chains carry truer
 * candidates — a shallower chain then reaches the same matches, which is what
 * lets the fast-tier configs below halve their depth. Used by the hash-chain
 * matcher; greedy (L1) keeps hash4. */
static uint32_t hash5(uint64_t v)
{
    return (uint32_t)((v * 0x9E3779B185EBCA87ULL) >> (64 - PARC_LZ_HASH_BITS));
}

/* Length of the common prefix of src[a..] and src[b..], capped at max bytes.
 * Requires a <= b and b + max <= n so every wide load stays in the buffer.
 *
 * The body compares 8 bytes at a time: XOR two native-order words, and the
 * first differing byte is the lowest-address set byte — the low byte on
 * little-endian (count trailing zero bits) or the high byte on big-endian
 * (count leading zero bits). A byte-wise tail finishes the last < 8 bytes.
 * Bit-identical to the byte-at-a-time compare it replaces. */
static size_t match_len(const uint8_t *src, size_t a, size_t b, size_t max)
{
    size_t l = 0;
    while (l + 8 <= max) {
        uint64_t x = read64(src + a + l) ^ read64(src + b + l);
        if (x) {
#if PARC_LZ_LITTLE_ENDIAN
            return l + ((size_t)__builtin_ctzll(x) >> 3);
#else
            return l + ((size_t)__builtin_clzll(x) >> 3);
#endif
        }
        l += 8;
    }
    while (l < max && src[a + l] == src[b + l])
        ++l;
    return l;
}

size_t parc_lz_greedy(const uint8_t *src, size_t n, parc_tok *toks,
                      uint32_t *htab)
{
    size_t nt = 0;

    assert(n <= PARC_LZ_MAX_BLOCK);
    memset(htab, 0xFF, HASH_SIZE * sizeof *htab); /* all NO_POS */

    size_t i = 0;
    while (i + PARC_LZ_MIN_MATCH <= n) {
        uint32_t four = read32(src + i);
        uint32_t h = hash4(four);
        uint32_t cand = htab[h];
        htab[h] = (uint32_t)i;

        if (cand == NO_POS || read32(src + cand) != four) {
            toks[nt].dist = 0;
            toks[nt].len_or_lit = src[i];
            ++nt;
            ++i;
            continue;
        }

        /* First 4 bytes already matched above, so len >= MIN_MATCH. */
        size_t len = match_len(src, cand, i, n - i);
        toks[nt].dist = (uint32_t)(i - cand);
        toks[nt].len_or_lit = (uint32_t)len;
        ++nt;

        /* index the positions the match skips so later data can refer
         * back into it */
        size_t stop = i + len;
        if (stop + PARC_LZ_MIN_MATCH > n)
            stop = n >= PARC_LZ_MIN_MATCH ? n - PARC_LZ_MIN_MATCH + 1 : 0;
        for (size_t p = i + 1; p < stop; ++p)
            htab[hash4(read32(src + p))] = (uint32_t)p;
        i += len;
    }
    while (i < n) {
        toks[nt].dist = 0;
        toks[nt].len_or_lit = src[i];
        ++nt;
        ++i;
    }
    return nt;
}

/* Per-level matcher parameters. Level 1 is the greedy matcher (max_chain 0);
 * 2..7 deepen the hash-chain lazy search and raise the "good enough"
 * nice_len; 8..9 feed the chain into the cost-based optimal parse. The
 * fast-tier depths (2..5) are tuned for the 5-byte chain hash: because hash5
 * chains carry truer candidates, half the old depth reaches essentially the
 * same matches, so these levels are ~1.3-2.4x faster at near-equal ratio. */
parc_lz_cfg parc_lz_cfg_for_level(unsigned level)
{
    static const parc_lz_cfg tbl[PARC_LZ_LEVEL_MAX + 1] = {
        {0, 0, 0},       /* unused: level 0 resolves to a default upstream */
        {0, 0, 0},       /* 1: greedy */
        {4, 32, 0},      /* 2 */
        {8, 64, 0},      /* 3 */
        {16, 64, 0},     /* 4 */
        {32, 128, 0},    /* 5 */
        {128, 256, 0},   /* 6 */
        {256, 512, 0},   /* 7 */
        {128, 128, 1},   /* 8: optimal */
        {512, 258, 1},   /* 9: optimal */
    };
    if (level < 1)
        level = 1;
    else if (level > PARC_LZ_LEVEL_MAX)
        level = PARC_LZ_LEVEL_MAX;
    return tbl[level];
}

/* Longest match for position i against the chain starting at cand (all chain
 * positions are < i). Returns the match length (>= PARC_LZ_MIN_MATCH) and its
 * distance in *dist, or 0 if no match of at least the minimum length exists. */
static uint32_t longest_match(const uint8_t *src, size_t n, size_t i,
                              uint32_t cand, const uint32_t *prev,
                              parc_lz_cfg cfg, uint32_t *dist)
{
    size_t max_len = n - i;
    size_t best = PARC_LZ_MIN_MATCH - 1; /* only >= MIN_MATCH counts */
    uint32_t best_dist = 0;
    uint32_t chain = cfg.max_chain;

    while (cand != NO_POS && chain--) {
        /* The chain walk is bound by the cache-missing prev[] pointer chase and
         * the random src[cand] probe. Prefetch the next link and its bytes so
         * those misses overlap the current candidate's work. (prev[next] and
         * src[next] for next == NO_POS are wild addresses, but __builtin_prefetch
         * never faults and ASan does not instrument it.) */
        uint32_t next = prev[cand];
        __builtin_prefetch(&prev[next]);
        __builtin_prefetch(src + next);
        /* best < max_len always holds here (we break when best reaches
         * max_len), so src[i + best] and src[cand + best] are in bounds. */
        if (src[cand + best] == src[i + best]) {
            size_t l = match_len(src, cand, i, max_len);
            if (l > best) {
                best = l;
                best_dist = (uint32_t)(i - cand);
                if (best >= cfg.nice_len || best >= max_len)
                    break;
            }
        }
        cand = next;
    }
    if (best < PARC_LZ_MIN_MATCH)
        return 0;
    *dist = best_dist;
    return (uint32_t)best;
}

size_t parc_lz_chain(const uint8_t *src, size_t n, parc_tok *toks,
                     uint32_t *head, uint32_t *prev, parc_lz_cfg cfg)
{
    size_t nt = 0;

    assert(n <= PARC_LZ_MAX_BLOCK && cfg.max_chain >= 1);
    memset(head, 0xFF, HASH_SIZE * sizeof *head); /* all NO_POS */

    /* prev_len/prev_dist describe the match found at position s-1, deferred
     * one step so a longer match starting at s can supersede it (lazy). */
    uint32_t prev_len = 0, prev_dist = 0;
    int deferred = 0; /* a token for position s-1 is pending */
    size_t s = 0;

    while (s + PARC_LZ_MIN_MATCH <= n) {
        uint32_t cur_len = 0, cur_dist = 0;
        /* read5 loads 8 bytes; the final positions with < 8 bytes left can
         * only start a short match, which we forgo (never chained, never
         * referenced) rather than read past the block. */
        if (s + 8 <= n) {
            uint32_t h = hash5(read5(src + s));
            uint32_t cand = head[h];
            prev[s] = cand;
            head[h] = (uint32_t)s;
            if (cand != NO_POS && prev_len < cfg.nice_len)
                cur_len = longest_match(src, n, s, cand, prev, cfg, &cur_dist);
        }

        if (deferred && prev_len >= PARC_LZ_MIN_MATCH && prev_len >= cur_len) {
            /* commit the match at s-1; s-1..end-1 are consumed. s-1 and s are
             * already in the chain; insert the rest of the matched span so
             * later positions can reference into it. */
            toks[nt].dist = prev_dist;
            toks[nt].len_or_lit = prev_len;
            ++nt;
            size_t end = (s - 1) + prev_len;
            for (size_t p = s + 1; p < end && p + 8 <= n; ++p) {
                uint32_t hp = hash5(read5(src + p));
                prev[p] = head[hp];
                head[hp] = (uint32_t)p;
            }
            s = end;
            deferred = 0;
            prev_len = 0;
        } else {
            if (deferred) {
                toks[nt].dist = 0;
                toks[nt].len_or_lit = src[s - 1];
                ++nt;
            }
            prev_len = cur_len;
            prev_dist = cur_dist;
            deferred = 1;
            ++s;
        }
    }

    /* flush the deferred position (if any) and the tail as literals */
    for (size_t p = deferred ? s - 1 : s; p < n; ++p) {
        toks[nt].dist = 0;
        toks[nt].len_or_lit = src[p];
        ++nt;
    }
    return nt;
}

/* ---- optimal parse ---- */

/* Estimated bit costs are carried in fixed point: bits * OPT_FIX. */
#define OPT_FIX 256u

/* bit_length(v): 0 for 0, else index of the highest set bit + 1. */
static unsigned blen(uint32_t v)
{
    return v == 0 ? 0 : 32u - (unsigned)__builtin_clz(v);
}

/* Sequence-symbol cost estimates in bits*OPT_FIX. A match always pays a litLen
 * symbol and a matchLen symbol (+ its exact bucket extra bits). The offset part
 * depends on whether the distance reuses one of the 3 recent offsets: a repeat
 * hit costs a small symbol and *no* extra bits (offset code 0/1/2 in §3), while
 * a new offset costs a larger symbol plus its bucket extra bits — which for a
 * far match is the dominant term. Modelling the repeat cache is what lets the
 * parse deliberately reuse offsets, exactly what the sequence coder rewards. */
#define C_LITLEN (3u * OPT_FIX)
#define C_MATCHLEN (4u * OPT_FIX)
#define C_OFFNEW (5u * OPT_FIX)
static const uint32_t C_OFFREP[3] = {4u * OPT_FIX, 4u * OPT_FIX, 5u * OPT_FIX};

/* Offset-part cost (bits*OPT_FIX) for a match of distance dist given the recent
 * offsets r[3]. *ri receives the repeat index hit (0..2), or -1 for a new
 * offset. Mirrors the encoder's rep test in block.c: r[0] wins ties. */
static uint64_t offset_cost(uint32_t dist, const uint32_t r[3], int *ri)
{
    if (dist == r[0]) {
        *ri = 0;
        return C_OFFREP[0];
    }
    if (dist == r[1]) {
        *ri = 1;
        return C_OFFREP[1];
    }
    if (dist == r[2]) {
        *ri = 2;
        return C_OFFREP[2];
    }
    *ri = -1;
    unsigned ofb = blen(dist - 1);
    return C_OFFNEW + (uint64_t)(ofb ? ofb - 1u : 0u) * OPT_FIX;
}

/* Full estimated cost of one match sequence (bits*OPT_FIX), rep-aware. */
static uint64_t match_cost(uint32_t len, uint32_t dist, const uint32_t r[3],
                           int *ri)
{
    unsigned mlb = blen(len - PARC_LZ_MIN_MATCH);
    uint64_t ml = C_MATCHLEN + (uint64_t)(mlb ? mlb - 1u : 0u) * OPT_FIX;
    return C_LITLEN + ml + offset_cost(dist, r, ri);
}

/* Apply the recent-offset MTF update for a match of distance dist that hit
 * repeat index ri (-1 for a new offset): write the post-match cache to out. */
static void rep_update(const uint32_t r[3], uint32_t dist, int ri,
                       uint32_t out[3])
{
    if (ri == 0) {
        out[0] = r[0];
        out[1] = r[1];
        out[2] = r[2];
    } else if (ri == 1) {
        out[0] = r[1];
        out[1] = r[0];
        out[2] = r[2];
    } else if (ri == 2) {
        out[0] = r[2];
        out[1] = r[0];
        out[2] = r[1];
    } else {
        out[0] = dist;
        out[1] = r[0];
        out[2] = r[1];
    }
}

/* Per-byte literal cost from the block's order-0 histogram (bits*OPT_FIX):
 * common bytes are cheaper, so the parse won't trade a good match for literals
 * that are actually expensive. Bytes absent from the block get a high cost
 * (they are never emitted as literals anyway). */
static void build_lit_prices(const uint8_t *src, size_t n, uint32_t *litp)
{
    uint32_t freq[256] = {0};
    for (size_t i = 0; i < n; ++i)
        freq[src[i]]++;
    for (unsigned b = 0; b < 256; ++b) {
        if (freq[b] == 0) {
            litp[b] = 16u * OPT_FIX;
            continue;
        }
        double bits = log2((double)n / (double)freq[b]);
        if (bits < 0.0625)
            bits = 0.0625; /* floor so a dominant byte still costs something */
        else if (bits > 16.0)
            bits = 16.0;
        litp[b] = (uint32_t)(bits * OPT_FIX + 0.5);
    }
}

typedef struct opt_match {
    uint32_t len;
    uint32_t dist;
} opt_match;

/* Collect the match frontier at position i: walk the chain (most-recent first)
 * and record every strict length improvement as (len, dist). Entries come out
 * in increasing length; because recent positions sit at smaller distances, the
 * first entry reaching a given length also carries a near-minimal distance for
 * it. Returns the entry count (<= PARC_OPT_MATCHES). */
static uint32_t find_matches(const uint8_t *src, size_t n, size_t i,
                             uint32_t cand, const uint32_t *prev,
                             parc_lz_cfg cfg, opt_match *out)
{
    size_t max_len = n - i;
    size_t best = PARC_LZ_MIN_MATCH - 1; /* only strictly longer counts */
    uint32_t chain = cfg.max_chain;
    uint32_t cnt = 0;

    while (cand != NO_POS && chain--) {
        /* best < max_len always holds (we break when best reaches max_len),
         * so src[i + best] and src[cand + best] are in bounds. */
        if (src[cand + best] == src[i + best]) {
            size_t l = match_len(src, cand, i, max_len);
            if (l > best) {
                best = l;
                opt_match m = {(uint32_t)l, (uint32_t)(i - cand)};
                /* keep the longest even once the array is full */
                out[cnt < PARC_OPT_MATCHES ? cnt : PARC_OPT_MATCHES - 1] = m;
                if (cnt < PARC_OPT_MATCHES)
                    ++cnt;
                if (l >= cfg.nice_len || l >= max_len)
                    break;
            }
        }
        cand = prev[cand];
    }
    return cnt;
}

size_t parc_lz_optimal(const uint8_t *src, size_t n, parc_tok *toks,
                       uint32_t *head, uint32_t *prev, parc_lz_cfg cfg,
                       uint64_t *price, uint32_t *bt_len, uint32_t *bt_dist,
                       uint32_t *rep)
{
    assert(n <= PARC_LZ_MAX_BLOCK && cfg.max_chain >= 1);
    memset(head, 0xFF, HASH_SIZE * sizeof *head); /* all NO_POS */

    uint32_t litp[256];
    build_lit_prices(src, n, litp);

    /* recent-offset cache carried across chunks, mirroring the v1 encoder's
     * {1,2,3} init and MTF update (block.c). Within a chunk the cache along the
     * best path to each node lives in rep[3*k..3*k+2]. */
    uint32_t carry_rep[3] = {1, 2, 3};

    size_t nt = 0;
    size_t c = 0;
    while (c < n) {
        size_t clen = n - c > PARC_OPT_CHUNK ? PARC_OPT_CHUNK : n - c;

        /* price[k] = min estimated cost to encode the chunk's first k bytes;
         * bt_dist/bt_len[k] record the token arriving at k (dist 0 = literal
         * byte in bt_len). price[0] = 0; every k is reachable via literals. */
        for (size_t k = 0; k <= clen; ++k)
            price[k] = UINT64_MAX;
        price[0] = 0;
        rep[0] = carry_rep[0];
        rep[1] = carry_rep[1];
        rep[2] = carry_rep[2];

        /* When a position yields a match at least nice_len long, the optimal
         * path almost certainly takes it, so the interior positions are still
         * inserted into the chain (later matches may reference them) but their
         * expensive frontier search is skipped up to skip_until. This keeps
         * deep chains affordable on repetitive data. */
        size_t skip_until = 0;

        for (size_t ii = 0; ii < clen; ++ii) {
            size_t pos = c + ii;
            uint64_t pc = price[ii];
            const uint32_t *r = &rep[3 * ii];

            uint64_t lp = pc + litp[src[pos]];
            if (lp < price[ii + 1]) {
                price[ii + 1] = lp;
                bt_len[ii + 1] = src[pos];
                bt_dist[ii + 1] = 0;
                rep[3 * ii + 3] = r[0]; /* literal: cache unchanged */
                rep[3 * ii + 4] = r[1];
                rep[3 * ii + 5] = r[2];
            }

            if (pos + PARC_LZ_MIN_MATCH > n)
                continue;

            /* Probe the three recent offsets directly: a repeat match is priced
             * cheap (no offset extra bits), so even a short one can beat a
             * longer new-offset match or set up a cheaper continuation. These
             * candidates are invisible to the hash-chain frontier below. */
            size_t rmax = (clen - ii < n - pos ? clen - ii : n - pos);
            for (int j = 0; j < 3; ++j) {
                uint32_t rd = r[j];
                if (rd == 0 || rd > pos)
                    continue;
                size_t rl = match_len(src, pos - rd, pos, rmax);
                if (rl < PARC_LZ_MIN_MATCH)
                    continue;
                int ri;
                uint64_t rp = pc + match_cost((uint32_t)rl, rd, r, &ri);
                if (rp < price[ii + rl]) {
                    price[ii + rl] = rp;
                    bt_len[ii + rl] = (uint32_t)rl;
                    bt_dist[ii + rl] = rd;
                    rep_update(r, rd, ri, &rep[3 * (ii + rl)]);
                }
            }

            uint32_t h = hash4(read32(src + pos));
            uint32_t cand = head[h];
            prev[pos] = cand;
            head[h] = (uint32_t)pos;

            if (pos < skip_until)
                continue; /* inside a committed long match: index only */

            opt_match out[PARC_OPT_MATCHES];
            uint32_t cnt = find_matches(src, n, pos, cand, prev, cfg, out);
            if (cnt == 0)
                continue;

            uint32_t bl = out[cnt - 1].len;
            if (out[cnt - 1].len >= cfg.nice_len) /* skip the interior span */
                skip_until = pos + out[cnt - 1].len;
            if (bl > clen - ii) /* a match may not cross the chunk boundary */
                bl = (uint32_t)(clen - ii);
            if (bl < PARC_LZ_MIN_MATCH)
                continue;

            /* always price the full longest match so long runs are taken in
             * one step (bounds the DP work on repetitive data) */
            uint32_t di = 0;
            while (di + 1 < cnt && out[di].len < bl)
                ++di;
            int ri;
            uint64_t mp = pc + match_cost(bl, out[di].dist, r, &ri);
            if (mp < price[ii + bl]) {
                price[ii + bl] = mp;
                bt_len[ii + bl] = bl;
                bt_dist[ii + bl] = out[di].dist;
                rep_update(r, out[di].dist, ri, &rep[3 * (ii + bl)]);
            }

            /* price a bounded window of short lengths, each at the smallest
             * distance that reaches it, so a shorter match here can win when
             * it sets up a cheaper continuation */
            uint32_t fk = 0, budget = PARC_OPT_BUDGET;
            for (uint32_t ell = PARC_LZ_MIN_MATCH; ell <= bl && budget;
                 ++ell, --budget) {
                while (fk < cnt && out[fk].len < ell)
                    ++fk;
                if (fk >= cnt)
                    break;
                uint64_t p = pc + match_cost(ell, out[fk].dist, r, &ri);
                if (p < price[ii + ell]) {
                    price[ii + ell] = p;
                    bt_len[ii + ell] = ell;
                    bt_dist[ii + ell] = out[fk].dist;
                    rep_update(r, out[fk].dist, ri, &rep[3 * (ii + ell)]);
                }
            }
        }

        /* backtrack the chunk into tokens (emitted in reverse, then flipped) */
        size_t start = nt;
        size_t j = clen;
        while (j > 0) {
            uint32_t d = bt_dist[j], l = bt_len[j];
            toks[nt].dist = d;
            toks[nt].len_or_lit = l;
            ++nt;
            j -= (d == 0) ? 1 : l;
        }
        for (size_t a = start, b = nt; a + 1 < b; ++a, --b) {
            parc_tok t = toks[a];
            toks[a] = toks[b - 1];
            toks[b - 1] = t;
        }

        /* rep[3*clen..] is the cache along the chosen (backtracked) path's final
         * node, so it is the recent-offset state to resume from next chunk. */
        carry_rep[0] = rep[3 * clen];
        carry_rep[1] = rep[3 * clen + 1];
        carry_rep[2] = rep[3 * clen + 2];

        c += clen;
    }
    return nt;
}
