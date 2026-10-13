#include "pool.h"
#include "fault_alloc.h"   /* rb_malloc / rb_free: slabs come through the seam */

#include <stdint.h>
#include <string.h>

/* In-band slab header: the first `header` bytes of every 4096-byte slab.
 * In-band means one rb_malloc per slab, so growing the pool has exactly one
 * way to fail. The cost is these bytes at the front of each slab. */
struct slab {
    struct slab *next;          /* every slab, newest first: pool_destroy's list */
};

struct rb_pool {
    struct pool_geometry g;
    struct slab *slabs;         /* every slab this pool owns */
    char        *bump;          /* next never-used slot in the newest slab */
    size_t       uncarved;      /* never-used slots left from bump onward */
    void        *free_list;     /* dead slots; each one's first bytes hold the next link */
    size_t       nslabs;
    size_t       live;          /* handed out and not freed yet */
    size_t       nfree;         /* length of free_list */
};

/* ---- geometry --------------------------------------------------------- */

static size_t round_up(size_t x, size_t a)
{
    return (x + a - 1) / a * a;
}

/* Alignment: pool_create only gets a size, never a type, so every slot is
 * aligned for the strictest type C has (max_align_t). rb_malloc returns
 * blocks aligned that way, and header and stride are both multiples of it,
 * so slab + header + k * stride is aligned for every k. */
struct pool_geometry pool_geometry(size_t obj_size)
{
    struct pool_geometry g;
    size_t need = obj_size > sizeof(void *) ? obj_size : sizeof(void *); /* a dead slot holds the link */

    g.align  = alignof(max_align_t);
    g.header = round_up(sizeof(struct slab), g.align);

    if (need > POOL_SLAB_SIZE - g.header) {     /* can never fit (also avoids overflow in round_up) */
        g.stride        = 0;
        g.objs_per_slab = 0;
        g.slack         = POOL_SLAB_SIZE - g.header;
        return g;
    }
    g.stride        = round_up(need, g.align);
    g.objs_per_slab = (POOL_SLAB_SIZE - g.header) / g.stride;
    g.slack         = POOL_SLAB_SIZE - g.header - g.objs_per_slab * g.stride;
    return g;
}

/* ---- the free-list link ----------------------------------------------- */

/* The link lives in the first bytes of a dead slot. memcpy instead of a
 * pointer cast, so there is no question of reading the slot as a type it
 * was never written as. */
static void *link_get(const void *obj)
{
    void *next;
    memcpy(&next, obj, sizeof next);
    return next;
}

static void link_set(void *obj, void *next)
{
    memcpy(obj, &next, sizeof next);
}

/* ---- the API ---------------------------------------------------------- */

rb_pool_t *pool_create(size_t obj_size)
{
    struct pool_geometry g = pool_geometry(obj_size);
    if (g.objs_per_slab == 0) {
        return NULL;                            /* object too big for any slab */
    }

    rb_pool_t *p = rb_malloc(sizeof *p);
    if (p == NULL) {
        return NULL;
    }
    p->g         = g;
    p->slabs     = NULL;                        /* lazy: first slab on first pool_alloc */
    p->bump      = NULL;
    p->uncarved  = 0;
    p->free_list = NULL;
    p->nslabs    = 0;
    p->live      = 0;
    p->nfree     = 0;
    return p;
}

void *pool_alloc(rb_pool_t *p)
{
    void *obj;

    if (p->free_list != NULL) {                 /* 1. reuse a dead slot */
        obj          = p->free_list;
        p->free_list = link_get(obj);
        p->nfree--;
    } else if (p->uncarved > 0) {               /* 2. next never-used slot */
        obj      = p->bump;
        p->bump += p->g.stride;
        p->uncarved--;
    } else {                                    /* 3. grow by one slab */
        struct slab *s = rb_malloc(POOL_SLAB_SIZE);
        if (s == NULL) {
            return NULL;                        /* nothing changed yet: pool as it was */
        }
        s->next  = p->slabs;
        p->slabs = s;
        p->nslabs++;

        char *first = (char *)s + p->g.header;
        obj         = first;                    /* slot 0 goes out right now */
        p->bump     = first + p->g.stride;
        p->uncarved = p->g.objs_per_slab - 1;   /* the tail slack is never a slot */
    }

    p->live++;
    return obj;
}

void pool_free(rb_pool_t *p, void *obj)
{
    if (obj == NULL) {
        return;
    }
#ifndef NDEBUG
    /* Poison the whole slot first, THEN let the link take its first bytes.
     * ASan sees the 4096-byte slab as one live block and cannot tell this
     * slot is dead, so a stale pointer would still read the old node. With
     * the poison, anything past the link reads 0xDD instead. */
    memset(obj, POOL_POISON, p->g.stride);
#endif
    link_set(obj, p->free_list);
    p->free_list = obj;
    p->nfree++;
    p->live--;
}

void pool_stats(const rb_pool_t *p,
                size_t *slabs, size_t *live, size_t *free_objs)
{
    if (slabs != NULL) {
        *slabs = p->nslabs;
    }
    if (live != NULL) {
        *live = p->live;
    }
    if (free_objs != NULL) {
        *free_objs = p->nfree + p->uncarved;
    }
}

void pool_destroy(rb_pool_t *p)
{
    if (p == NULL) {
        return;
    }
    /* Never looks inside objects, so live ones do not matter: they are
     * just bytes inside slabs that are about to go.
     * Invariant: s and every slab after it are still allocated. */
    struct slab *s = p->slabs;
    while (s != NULL) {
        struct slab *next = s->next;            /* read before freeing s */
        rb_free(s);
        s = next;
    }
    rb_free(p);
}

/* ---- for tests -------------------------------------------------------- */

size_t pool_objs_per_slab(const rb_pool_t *p)
{
    return p->g.objs_per_slab;
}

/* Is o the start of a slot inside one of p's slabs? Compared as integers
 * because < between pointers into different blocks is undefined in C. */
static bool slot_in_pool(const rb_pool_t *p, const void *o)
{
    uintptr_t a = (uintptr_t)o;
    for (const struct slab *s = p->slabs; s != NULL; s = s->next) {
        uintptr_t first = (uintptr_t)s + p->g.header;
        uintptr_t end   = first + p->g.objs_per_slab * p->g.stride;
        if (a >= first && a < end) {
            return (a - first) % p->g.stride == 0;
        }
    }
    return false;
}

int pool_check(const rb_pool_t *p)
{
    size_t walked = 0;
    for (const void *o = p->free_list; o != NULL; o = link_get(o)) {
        if (++walked > p->nfree) {
            return 1;                           /* longer than recorded, or a cycle */
        }
        if (!slot_in_pool(p, o)) {
            return 2;
        }
    }
    if (walked != p->nfree) {
        return 3;
    }
    if (p->live + p->nfree + p->uncarved != p->nslabs * p->g.objs_per_slab) {
        return 4;
    }
    return 0;
}
