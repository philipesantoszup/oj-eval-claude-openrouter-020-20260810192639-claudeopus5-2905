#include "buddy.h"

#include <stdlib.h>

#define NULL ((void *)0)

#define PAGE_SHIFT 12
#define PAGE_SIZE ((long)1 << PAGE_SHIFT)
#define MAX_RANK 16
/* number of 4K pages covered by a block of the given rank */
#define RANK_PAGES(r) ((long)1 << ((r) - 1))

typedef unsigned long long word_t;
#define WORD_BITS 64

/*
 * Book-keeping.
 *
 * The pool is described page by page:
 *   g_order[i] != 0  <=>  page i is the first page of a block (free or busy),
 *                         and the value is that block's rank.
 *   g_busy[i]        <=>  the block starting at page i is allocated.
 *
 * Free blocks of each rank are additionally tracked in a bitmap indexed by
 * block number (page index >> (rank - 1)), so allocation can always pick the
 * free block with the lowest address in (amortised) constant time.
 */
static unsigned char *g_order;
static unsigned char *g_busy;
static word_t *g_bitmap[MAX_RANK + 1];
static long g_words[MAX_RANK + 1];  /* bitmap size, in words */
static long g_hint[MAX_RANK + 1];   /* no free block lives before this word */
static long g_count[MAX_RANK + 1];  /* number of free blocks of this rank */

static unsigned char *g_base;
static long g_npages;

static void bitmap_set(int rank, long idx) {
    long blk = idx >> (rank - 1);
    long w = blk / WORD_BITS;
    g_bitmap[rank][w] |= (word_t)1 << (blk % WORD_BITS);
    if (w < g_hint[rank]) g_hint[rank] = w;
}

static void bitmap_clear(int rank, long idx) {
    long blk = idx >> (rank - 1);
    g_bitmap[rank][blk / WORD_BITS] &= ~((word_t)1 << (blk % WORD_BITS));
}

/* lowest-addressed free block of this rank, -1 if there is none */
static long bitmap_first(int rank) {
    long w;
    for (w = g_hint[rank]; w < g_words[rank]; ++w) {
        if (g_bitmap[rank][w]) {
            g_hint[rank] = w;
            return ((w * WORD_BITS) + __builtin_ctzll(g_bitmap[rank][w]))
                   << (rank - 1);
        }
    }
    g_hint[rank] = g_words[rank];
    return -1;
}

static void mark_free(int rank, long idx) {
    g_order[idx] = (unsigned char)rank;
    g_busy[idx] = 0;
    bitmap_set(rank, idx);
    g_count[rank]++;
}

static void unmark_free(int rank, long idx) {
    bitmap_clear(rank, idx);
    g_count[rank]--;
    g_order[idx] = 0;
}

static void release_pool(void) {
    int r;
    if (g_order) free(g_order);
    if (g_busy) free(g_busy);
    g_order = NULL;
    g_busy = NULL;
    for (r = 1; r <= MAX_RANK; ++r) {
        if (g_bitmap[r]) free(g_bitmap[r]);
        g_bitmap[r] = NULL;
        g_words[r] = 0;
        g_hint[r] = 0;
        g_count[r] = 0;
    }
    g_base = NULL;
    g_npages = 0;
}

/* page index of p, or -1 if p is not the start of a page inside the pool */
static long page_index(void *p) {
    unsigned long base, addr, off;
    if (g_base == NULL || p == NULL) return -1;
    base = (unsigned long)g_base;
    addr = (unsigned long)p;
    if (addr < base) return -1;
    off = addr - base;
    if (off % (unsigned long)PAGE_SIZE) return -1;
    if (off / (unsigned long)PAGE_SIZE >= (unsigned long)g_npages) return -1;
    return (long)(off / (unsigned long)PAGE_SIZE);
}

/* find the block currently containing page idx; returns its rank and stores
 * the index of its first page in *head */
static int block_of(long idx, long *head) {
    int r;
    for (r = 1; r <= MAX_RANK; ++r) {
        long start = idx & ~(RANK_PAGES(r) - 1);
        if (g_order[start] == (unsigned char)r) {
            *head = start;
            return r;
        }
    }
    return -1;
}

int init_page(void *p, int pgcount) {
    long i;
    int r;

    if (p == NULL || pgcount <= 0) return -EINVAL;

    release_pool();

    g_order = (unsigned char *)calloc((size_t)pgcount, 1);
    g_busy = (unsigned char *)calloc((size_t)pgcount, 1);
    if (g_order == NULL || g_busy == NULL) {
        release_pool();
        return -ENOSPC;
    }
    for (r = 1; r <= MAX_RANK; ++r) {
        long nblk = ((long)pgcount >> (r - 1)) + 1;
        g_words[r] = (nblk + WORD_BITS - 1) / WORD_BITS;
        g_bitmap[r] = (word_t *)calloc((size_t)g_words[r], sizeof(word_t));
        if (g_bitmap[r] == NULL) {
            release_pool();
            return -ENOSPC;
        }
        g_hint[r] = g_words[r];
        g_count[r] = 0;
    }

    g_base = (unsigned char *)p;
    g_npages = pgcount;

    /* carve the pool into the largest properly aligned free blocks */
    for (i = 0; i < g_npages;) {
        int best = 1;
        for (r = MAX_RANK; r >= 1; --r) {
            if ((i & (RANK_PAGES(r) - 1)) == 0 &&
                i + RANK_PAGES(r) <= g_npages) {
                best = r;
                break;
            }
        }
        mark_free(best, i);
        i += RANK_PAGES(best);
    }

    return OK;
}

void *alloc_pages(int rank) {
    int r;
    long idx;

    if (rank < 1 || rank > MAX_RANK) return ERR_PTR(-EINVAL);
    if (g_base == NULL) return ERR_PTR(-ENOSPC);

    for (r = rank; r <= MAX_RANK; ++r)
        if (g_count[r] > 0) break;
    if (r > MAX_RANK) return ERR_PTR(-ENOSPC);

    idx = bitmap_first(r);
    if (idx < 0) return ERR_PTR(-ENOSPC);
    unmark_free(r, idx);

    /* split down to the requested rank, keeping the lower half */
    while (r > rank) {
        --r;
        mark_free(r, idx + RANK_PAGES(r));
    }

    g_order[idx] = (unsigned char)rank;
    g_busy[idx] = 1;
    return (void *)(g_base + idx * PAGE_SIZE);
}

int return_pages(void *p) {
    long idx = page_index(p);
    int r;

    if (idx < 0) return -EINVAL;
    if (g_order[idx] == 0 || !g_busy[idx]) return -EINVAL;

    r = g_order[idx];
    g_order[idx] = 0;
    g_busy[idx] = 0;

    /* coalesce with free buddies as far up as possible */
    while (r < MAX_RANK) {
        long buddy = idx ^ RANK_PAGES(r);
        if (buddy + RANK_PAGES(r) > g_npages) break;
        if (g_order[buddy] != (unsigned char)r || g_busy[buddy]) break;
        unmark_free(r, buddy);
        if (buddy < idx) idx = buddy;
        ++r;
    }

    mark_free(r, idx);
    return OK;
}

int query_ranks(void *p) {
    long idx = page_index(p);
    long head = 0;
    int r;

    if (idx < 0) return -EINVAL;

    r = block_of(idx, &head);
    if (r < 0) return -EINVAL;
    /* a free page reports the rank of the largest free block holding it */
    if (!g_busy[head]) return r;
    /* an allocated block may only be queried through its own address */
    if (head != idx) return -EINVAL;
    return r;
}

int query_page_counts(int rank) {
    if (rank < 1 || rank > MAX_RANK) return -EINVAL;
    if (g_base == NULL) return 0;
    return (int)g_count[rank];
}
