#include "test_pool.h"
#include "pool.h"
#include "fault_alloc.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Defined in tests/fault_alloc.c; not in any header on purpose. */
size_t rb_debug_live_allocs(void);

/* The node is 48 bytes today; the pool never sees its type, only this size. */
enum { OBJ = 48, MAX_LIVE = 1500 };

static int pool_failures;

#define POOL_CHECK(cond, ...)                                           \
    do {                                                                \
        if (!(cond)) {                                                  \
            pool_failures++;                                            \
            fprintf(stderr, "  pool FAIL line %d: ", __LINE__);         \
            fprintf(stderr, __VA_ARGS__);                               \
            fprintf(stderr, "\n");                                      \
        }                                                               \
    } while (0)

/* live is what the test expects; the rest is the spec's invariant plus the
 * pool's own walk of its free list. */
static void check_pool(const rb_pool_t *p, size_t want_live, const char *when)
{
    size_t slabs, live, free_objs;
    pool_stats(p, &slabs, &live, &free_objs);
    POOL_CHECK(live == want_live, "%s: live %zu, expected %zu", when, live, want_live);
    POOL_CHECK(live + free_objs == slabs * pool_objs_per_slab(p),
               "%s: live %zu + free_objs %zu != %zu slabs * %zu per slab",
               when, live, free_objs, slabs, pool_objs_per_slab(p));
    int c = pool_check(p);
    POOL_CHECK(c == 0, "%s: pool_check returned %d", when, c);
}

/* A tag at both ends of an object. If two live objects ever share a slot,
 * the second one's tag overwrites the first's and check_tag fails. */
static void put_tag(void *obj, size_t id)
{
    size_t back = ~id;
    memcpy(obj, &id, sizeof id);
    memcpy((char *)obj + OBJ - sizeof back, &back, sizeof back);
}

static bool tag_ok(const void *obj, size_t id)
{
    size_t front, back;
    memcpy(&front, obj, sizeof front);
    memcpy(&back, (const char *)obj + OBJ - sizeof back, sizeof back);
    return front == id && back == ~id;
}

/* ---- 1. geometry ------------------------------------------------------ */

static void test_geometry(void)
{
    const size_t fits[] = { 0, 1, 8, 24, 40, 48, 100, 1000, 4000 };
    for (size_t i = 0; i < sizeof fits / sizeof fits[0]; i++) {
        size_t n = fits[i];
        size_t need = n > sizeof(void *) ? n : sizeof(void *);
        struct pool_geometry g = pool_geometry(n);

        POOL_CHECK(g.objs_per_slab >= 1, "size %zu: no slots", n);
        POOL_CHECK(g.header >= sizeof(void *) && g.header % g.align == 0,
                   "size %zu: header %zu not an aligned home for the slab link", n, g.header);
        POOL_CHECK(g.stride >= need && g.stride % g.align == 0,
                   "size %zu: stride %zu too small or not a multiple of %zu", n, g.stride, g.align);
        POOL_CHECK(g.header + g.objs_per_slab * g.stride + g.slack == POOL_SLAB_SIZE,
                   "size %zu: header + slots + slack != %d", n, POOL_SLAB_SIZE);
        POOL_CHECK(g.slack < g.stride, "size %zu: slack %zu could hold another slot", n, g.slack);
    }

    /* Too big for a slab: pool_create refuses without allocating anything. */
    const size_t too_big[] = { 4096, 5000, SIZE_MAX };
    size_t live0 = rb_debug_live_allocs();
    for (size_t i = 0; i < sizeof too_big / sizeof too_big[0]; i++) {
        POOL_CHECK(pool_geometry(too_big[i]).objs_per_slab == 0,
                   "size %zu should not fit", too_big[i]);
        POOL_CHECK(pool_create(too_big[i]) == NULL, "pool_create(%zu) should fail", too_big[i]);
    }
    POOL_CHECK(rb_debug_live_allocs() == live0, "refusing a too-big pool allocated something");

    struct pool_geometry g = pool_geometry(OBJ);
    printf("pool: %d-byte objects: align %zu, header %zu, stride %zu, %zu per slab, slack %zu\n",
           OBJ, g.align, g.header, g.stride, g.objs_per_slab, g.slack);
}

/* ---- 2. alloc / free / reuse under load -------------------------------- */

static void test_alloc_free_stress(void)
{
    size_t live0 = rb_debug_live_allocs();
    rb_pool_t *p = pool_create(OBJ);
    POOL_CHECK(p != NULL, "pool_create(%d) failed", OBJ);
    if (p == NULL) {
        return;
    }
    check_pool(p, 0, "empty pool");

    void  *obj[MAX_LIVE];
    size_t align = pool_geometry(OBJ).align;
    size_t n = 0;

    /* Phase 1: 1000 allocations, every one aligned and tagged. */
    for (; n < 1000; n++) {
        obj[n] = pool_alloc(p);
        POOL_CHECK(obj[n] != NULL, "alloc %zu returned NULL", n);
        if (obj[n] == NULL) {
            pool_destroy(p);
            return;
        }
        POOL_CHECK((uintptr_t)obj[n] % align == 0, "alloc %zu not %zu-aligned", n, align);
        put_tag(obj[n], n);
    }
    check_pool(p, 1000, "after 1000 allocs");

    /* Phase 2: free every third one. */
    size_t live = 1000;
    for (size_t i = 0; i < 1000; i += 3) {
        pool_free(p, obj[i]);
        obj[i] = NULL;
        live--;
    }
    check_pool(p, live, "after freeing every third");

    /* Phase 3: 500 more. The first ones reuse dead slots, the rest are new. */
    for (; n < 1500; n++) {
        obj[n] = pool_alloc(p);
        POOL_CHECK(obj[n] != NULL, "alloc %zu returned NULL", n);
        if (obj[n] == NULL) {
            pool_destroy(p);
            return;
        }
        POOL_CHECK((uintptr_t)obj[n] % align == 0, "alloc %zu not %zu-aligned", n, align);
        put_tag(obj[n], n);
        live++;
    }
    check_pool(p, live, "after 500 more");

    /* No slot was handed out twice: every live tag is still intact. */
    size_t bad = 0;
    for (size_t i = 0; i < n; i++) {
        if (obj[i] != NULL && !tag_ok(obj[i], i)) {
            bad++;
        }
    }
    POOL_CHECK(bad == 0, "%zu live object(s) overwritten: a slot was handed out twice", bad);

    /* Phase 4: destroy with ~1166 objects still live. */
    pool_destroy(p);
    POOL_CHECK(rb_debug_live_allocs() == live0,
               "pool_destroy with live objects left %zu block(s)", rb_debug_live_allocs() - live0);
}

/* ---- 3. small behaviors ------------------------------------------------ */

static void test_lifo_reuse_and_nulls(void)
{
    rb_pool_t *p = pool_create(OBJ);
    POOL_CHECK(p != NULL, "pool_create failed");
    if (p == NULL) {
        return;
    }
    void *a = pool_alloc(p);
    void *b = pool_alloc(p);
    pool_free(p, a);
    void *c = pool_alloc(p);
    POOL_CHECK(c == a, "the most recently freed slot should be reused first");

    size_t live_before;
    pool_stats(p, NULL, &live_before, NULL);        /* NULL out-pointers are allowed */
    pool_free(p, NULL);                             /* no-op, like free(NULL) */
    check_pool(p, live_before, "after pool_free(NULL)");

    pool_free(p, b);
    pool_free(p, c);
    check_pool(p, 0, "everything freed");
    pool_destroy(p);
    pool_destroy(NULL);                             /* NULL-safe */
}

/* ---- 4. poisoning ------------------------------------------------------ */

static void test_poison(void)
{
#ifndef NDEBUG
    rb_pool_t *p = pool_create(OBJ);
    POOL_CHECK(p != NULL, "pool_create failed");
    if (p == NULL) {
        return;
    }
    size_t stride = pool_geometry(OBJ).stride;
    unsigned char *x = pool_alloc(p);
    memset(x, 0x11, OBJ);                           /* looks like a live node */
    pool_free(p, x);

    /* Reading a dead slot on purpose: it is still inside a live slab, which
     * is exactly why ASan cannot catch a stale read here and poison must. */
    void *link;
    memcpy(&link, x, sizeof link);
    POOL_CHECK(link == NULL, "first bytes should be the free-list link (NULL: list was empty)");
    size_t not_poisoned = 0;
    for (size_t i = sizeof(void *); i < stride; i++) {
        if (x[i] != POOL_POISON) {
            not_poisoned++;
        }
    }
    POOL_CHECK(not_poisoned == 0, "%zu byte(s) past the link are not 0x%X", not_poisoned,
               POOL_POISON);
    pool_destroy(p);
#endif
}

/* ---- 5. allocation failure --------------------------------------------- */

static void test_fault_paths(void)
{
    size_t live0 = rb_debug_live_allocs();

    /* pool_create's one allocation fails. */
    fault_alloc_arm(1);
    rb_pool_t *p = pool_create(OBJ);
    fault_alloc_disarm();
    POOL_CHECK(p == NULL, "pool_create should fail when its allocation fails");
    POOL_CHECK(rb_debug_live_allocs() == live0, "failed pool_create leaked");

    p = pool_create(OBJ);
    POOL_CHECK(p != NULL, "pool_create failed");
    if (p == NULL) {
        return;
    }

    /* First alloc needs the first slab; make that fail. Pool must be untouched. */
    fault_alloc_arm(1);
    void *x = pool_alloc(p);
    fault_alloc_disarm();
    POOL_CHECK(x == NULL, "alloc needing a slab should fail when the slab allocation fails");
    size_t slabs;
    pool_stats(p, &slabs, NULL, NULL);
    POOL_CHECK(slabs == 0, "failed slab allocation still counted a slab");
    check_pool(p, 0, "after failed first slab");

    /* Fill exactly one slab. Only the first of these may call rb_malloc. */
    size_t per = pool_objs_per_slab(p);
    long total0 = fault_alloc_total();
    for (size_t i = 0; i < per; i++) {
        POOL_CHECK(pool_alloc(p) != NULL, "alloc %zu in the first slab failed", i);
    }
    POOL_CHECK(fault_alloc_total() - total0 == 1,
               "filling one slab made %ld rb_malloc calls, expected 1", fault_alloc_total() - total0);
    check_pool(p, per, "one full slab");

    /* The next alloc needs slab 2; make that fail. */
    fault_alloc_arm(1);
    x = pool_alloc(p);
    fault_alloc_disarm();
    POOL_CHECK(x == NULL, "alloc needing slab 2 should fail when its allocation fails");
    pool_stats(p, &slabs, NULL, NULL);
    POOL_CHECK(slabs == 1, "failed slab 2 changed the slab count to %zu", slabs);
    check_pool(p, per, "after failed slab 2");

    /* With a dead slot available, alloc must not touch rb_malloc at all:
     * the armed fault must still be waiting afterwards. */
    x = pool_alloc(p);                          /* now succeeds: slab 2 */
    POOL_CHECK(x != NULL, "alloc after disarm failed");
    pool_free(p, x);
    fault_alloc_arm(1);
    total0 = fault_alloc_total();
    void *y = pool_alloc(p);
    POOL_CHECK(y == x && fault_alloc_total() == total0,
               "reusing a dead slot should not call rb_malloc");
    fault_alloc_disarm();

    pool_destroy(p);
    POOL_CHECK(rb_debug_live_allocs() == live0, "fault test left %zu block(s)",
               rb_debug_live_allocs() - live0);
}

int pool_unit_tests(void)
{
    pool_failures = 0;
    test_geometry();
    test_alloc_free_stress();
    test_lifo_reuse_and_nulls();
    test_poison();
    test_fault_paths();
    if (pool_failures == 0) {
        printf("pool unit tests: ok\n");
    }
    return pool_failures;
}
