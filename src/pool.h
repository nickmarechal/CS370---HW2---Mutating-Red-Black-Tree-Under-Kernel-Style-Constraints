#ifndef POOL_H
#define POOL_H

#include <stddef.h>

/* Slab-style pool for fixed-size objects (spec 8.2). Private to this
 * project: NOT part of the frozen include/rbtree.h. */

typedef struct rb_pool rb_pool_t;

/* NULL on allocation failure, or if obj_size can never fit in a slab. */
rb_pool_t *pool_create(size_t obj_size);

/* O(1). NULL only if a new slab was needed and rb_malloc failed; the pool
 * is then unchanged. */
void *pool_alloc(rb_pool_t *p);

/* O(1). Puts obj back on the free list. NULL is a no-op, like free(NULL). */
void pool_free(rb_pool_t *p, void *obj);

/* slabs: slabs owned. live: objects handed out and not freed.
 * free_objs: objects pool_alloc can hand out WITHOUT calling rb_malloc
 * (free list + never-used slots in the newest slab). Any out-pointer may be
 * NULL. Always: live + free_objs == slabs * pool_objs_per_slab(p). */
void pool_stats(const rb_pool_t *p,
                size_t *slabs, size_t *live, size_t *free_objs);

/* Frees every slab and the pool itself, whether or not objects are still
 * live. NULL-safe. */
void pool_destroy(rb_pool_t *p);

/* ---- beyond the spec: geometry and checks, used by the tests ---------- */

enum { POOL_SLAB_SIZE = 4096 };

/* Byte written over every freed object in debug builds (no NDEBUG). */
#define POOL_POISON 0xDD

/* How one slab is carved for a given obj_size. objs_per_slab == 0 means the
 * object can never fit (then stride is 0 too). Always, when it fits:
 * header + objs_per_slab * stride + slack == POOL_SLAB_SIZE. */
struct pool_geometry {
    size_t align;           /* alignment every slot guarantees */
    size_t header;          /* in-band slab header, rounded up to align */
    size_t stride;          /* distance between slots, a multiple of align */
    size_t objs_per_slab;
    size_t slack;           /* tail bytes too small for another slot */
};

struct pool_geometry pool_geometry(size_t obj_size);
size_t pool_objs_per_slab(const rb_pool_t *p);

/* 0 if the pool is consistent, otherwise a code naming what broke:
 * 1 free list longer than recorded (or a cycle), 2 a free-list entry that is
 * not a slot in one of our slabs, 3 free list shorter than recorded,
 * 4 live + free_objs != slabs * objs_per_slab. Walks the free list, so it
 * is for tests, not for every call. */
int pool_check(const rb_pool_t *p);

#endif
