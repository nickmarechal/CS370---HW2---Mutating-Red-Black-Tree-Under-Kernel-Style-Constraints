#include "test_bigstack.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Defined in tests/fault_alloc.c; not in any header on purpose. */
size_t rb_debug_live_allocs(void);

/* Spec 8.3: a kernel thread gets roughly 8-16 KB of stack; we give ours 64. */
enum { SMALL_STACK = 64 * 1024 };

static int big_failures;
static const char *cur_label = "";

#define BIG_CHECK(cond, ...)                                            \
    do {                                                                \
        if (!(cond)) {                                                  \
            big_failures++;                                             \
            fprintf(stderr, "  bigstack[%s] FAIL line %d: ", cur_label, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                               \
            fprintf(stderr, "\n");                                      \
        }                                                               \
    } while (0)

/* Values are fake pointers (i + 1, never dereferenced). The tree owns them,
 * and this destructor only counts, so we can check rb_destroy reached every
 * node without paying for a million value allocations. */
static long values_released;

static void count_release(void *value)
{
    (void)value;
    values_released++;
}

/* rb_foreach callback: count keys and check they strictly increase. */
struct walk {
    const char *prev;
    long        n;
    long        out_of_order;
};

static void walk_cb(const char *key, void *value, void *ctx)
{
    (void)value;
    struct walk *w = ctx;
    if (w->prev != NULL && strcmp(w->prev, key) >= 0) {
        w->out_of_order++;
    }
    w->prev = key;
    w->n++;
}

/* Everything the small-stack thread does. Nothing here may recurse: the
 * whole point is that 64 KiB is all it gets. */
struct job {
    rbtree_t   *t;
    struct walk w;
};

static void *small_stack_main(void *arg)
{
    struct job *j = arg;
    rb_foreach(j->t, walk_cb, &j->w);
    rb_destroy(j->t);
    j->t = NULL;
    return NULL;
}

static long gcd(long a, long b)
{
    while (b != 0) {            /* Euclid: (a, b) -> (b, a mod b); b strictly shrinks */
        long r = a % b;
        a = b;
        b = r;
    }
    return a;
}

int big_stack_test(rbtree_t *(*create)(rb_value_free_fn value_free),
                   const char *label, long n)
{
    big_failures    = 0;
    cur_label       = label;
    values_released = 0;
    size_t live0    = rb_debug_live_allocs();

    /* Build on the main thread (normal stack). Scrambled but fixed order:
     * index (k * stride) % n visits every index once when gcd(stride, n) == 1. */
    rbtree_t *t = create(count_release);
    BIG_CHECK(t != NULL, "create failed");
    if (t == NULL) {
        return big_failures;
    }
    long stride = (gcd(7919, n) == 1) ? 7919 : 1;
    char key[24];               /* room for any long, so gcc -Wformat-truncation stays quiet */
    for (long k = 0; k < n; k++) {
        long i = (long)(((long long)k * stride) % n);
        snprintf(key, sizeof key, "k%07ld", i);
        if (rb_insert(t, key, (void *)(uintptr_t)(i + 1)) != 0) {
            BIG_CHECK(false, "insert %s failed", key);
            rb_destroy(t);
            return big_failures;
        }
    }
    BIG_CHECK(rb_size(t) == (size_t)n, "size %zu after %ld inserts", rb_size(t), n);
    BIG_CHECK(rb_validate(t) == 0, "tree invalid after building it");

    /* Now foreach + destroy on a 64 KiB stack. If setting the size fails we
     * report it rather than quietly running on the default (megabytes) stack. */
    struct job job = { .t = t };
    pthread_attr_t attr;
    pthread_t      tid;
    int rc = pthread_attr_init(&attr);
    if (rc == 0) {
        rc = pthread_attr_setstacksize(&attr, SMALL_STACK);
        BIG_CHECK(rc == 0, "pthread_attr_setstacksize(%d) failed: %d", SMALL_STACK, rc);
        if (rc == 0) {
            rc = pthread_create(&tid, &attr, small_stack_main, &job);
            BIG_CHECK(rc == 0, "pthread_create failed: %d", rc);
            if (rc == 0) {
                pthread_join(tid, NULL);
            }
        }
        pthread_attr_destroy(&attr);
    } else {
        BIG_CHECK(false, "pthread_attr_init failed: %d", rc);
    }
    if (job.t != NULL) {        /* thread never ran: clean up here so nothing leaks */
        rb_destroy(job.t);
        return big_failures;
    }

    BIG_CHECK(job.w.n == n, "foreach visited %ld keys, expected %ld", job.w.n, n);
    BIG_CHECK(job.w.out_of_order == 0, "foreach gave %ld key(s) out of order",
              job.w.out_of_order);
    BIG_CHECK(values_released == n, "destroy released %ld value(s), expected %ld",
              values_released, n);
    BIG_CHECK(rb_debug_live_allocs() == live0, "destroy left %zu block(s)",
              rb_debug_live_allocs() - live0);

    if (big_failures == 0) {
        printf("bigstack[%s]: %ld nodes, foreach + destroy on a %d KiB thread stack: ok\n",
               label, n, SMALL_STACK / 1024);
    }
    return big_failures;
}
