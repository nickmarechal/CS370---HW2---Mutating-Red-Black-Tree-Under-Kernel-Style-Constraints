#include "rbtree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Defined in src/rbtree.c; declared here because rbtree.h is frozen. */
size_t rb_debug_live_allocs(void);

/* Invoked as: ./build/fuzz <op-count> [seed]
 *
 * Random inserts, finds and deletes against a reference model, with
 * rb_validate and a full in-order comparison every VALIDATE_EVERY ops.
 *
 * The model is deliberately dumb: one "present" flag and one expected value
 * pointer per possible key. Keys come from a small fixed space so deletes
 * and overwrites actually hit existing keys instead of mostly missing.
 * Because the keys are zero-padded, walking the model by index IS sorted
 * order, which is what rb_foreach must produce.
 *
 * The seed is printed up front and on every failure, so any failure
 * reproduces exactly:  ./build/fuzz <ops> <seed>. */

enum { KEYSPACE = 512, VALIDATE_EVERY = 100, DEFAULT_SEED = 20260914 };

static char keys[KEYSPACE][8];              /* "k0000" .. "k0511" */

struct model {
    bool   present[KEYSPACE];
    int   *value[KEYSPACE];                 /* pointer the tree must hold */
    size_t count;
};

/* xorshift64*: a few lines of our own so the same seed gives the same op
 * sequence on every machine. rand() differs between libcs, which would make
 * a failure found under valgrind on Linux unreproducible on a Mac. */
static unsigned long long rng_state;

static unsigned long long rng_next(void)
{
    unsigned long long x = rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng_state = x;
    return x * 2685821657736338717ULL;
}

/* rb_foreach callback: check the tree's in-order sequence against the
 * model's, key and value both. Any missing, extra or misplaced key knocks
 * the two sequences out of step and shows up as a mismatch. */
struct walk {
    const struct model *m;
    int    next;                            /* model index expected next */
    size_t seen;
    long   errors;
    char   first_error[128];
};

static void walk_cb(const char *key, void *value, void *ctx)
{
    struct walk *w = ctx;

    /* Invariant: every model key below w->next has been matched already. */
    while (w->next < KEYSPACE && !w->m->present[w->next]) {
        w->next++;
    }
    if (w->next >= KEYSPACE) {
        if (w->errors++ == 0) {
            snprintf(w->first_error, sizeof w->first_error,
                     "tree yielded %s after the model ran out of keys", key);
        }
    } else if (strcmp(key, keys[w->next]) != 0 || value != w->m->value[w->next]) {
        if (w->errors++ == 0) {
            snprintf(w->first_error, sizeof w->first_error,
                     "tree yielded %s (value %p), model expected %s (value %p)",
                     key, value, keys[w->next], (void *)w->m->value[w->next]);
        }
    }
    w->next++;
    w->seen++;
}

/* Full check: all five rules, the count, and the complete in-order
 * contents. Returns a message on failure, NULL when everything agrees. */
static const char *check_tree(const rbtree_t *t, const struct model *m)
{
    static char msg[256];

    int rc = rb_validate(t);
    if (rc != 0) {
        snprintf(msg, sizeof msg, "rb_validate returned %d", rc);
        return msg;
    }
    if (rb_size(t) != m->count) {
        snprintf(msg, sizeof msg, "rb_size %zu, model has %zu", rb_size(t), m->count);
        return msg;
    }

    struct walk w = { .m = m, .next = 0, .seen = 0, .errors = 0, .first_error = "" };
    rb_foreach(t, walk_cb, &w);
    if (w.errors != 0) {
        snprintf(msg, sizeof msg, "foreach: %s", w.first_error);
        return msg;
    }
    if (w.seen != m->count) {
        snprintf(msg, sizeof msg, "foreach visited %zu keys, model has %zu",
                 w.seen, m->count);
        return msg;
    }
    return NULL;
}

static void die(rbtree_t *t, long op, unsigned long long seed, const char *what,
                const char *detail)
{
    fprintf(stderr, "fuzz: FAIL at op %ld (seed %llu): %s -- %s\n"
                    "      reproduce with: ./build/fuzz %ld %llu\n",
            op, seed, what, detail, op + 1, seed);
    rb_destroy(t);
    exit(1);
}

int main(int argc, char **argv)
{
    long ops = (argc > 1) ? strtol(argv[1], NULL, 10) : 0;
    unsigned long long seed = (argc > 2) ? strtoull(argv[2], NULL, 10)
                                         : DEFAULT_SEED;
    if (ops <= 0) {
        fprintf(stderr, "usage: %s <op-count> [seed]\n", argv[0]);
        return 2;
    }
    rng_state = seed ? seed : 1;            /* xorshift must never be 0 */

    for (int k = 0; k < KEYSPACE; k++) {
        snprintf(keys[k], sizeof keys[k], "k%04d", k);
    }

    static struct model m;                  /* zeroed: nothing present */
    rbtree_t *t = rb_create(free);          /* the tree owns the values */
    if (t == NULL) {
        fprintf(stderr, "fuzz: rb_create failed\n");
        return 1;
    }

    size_t max_size = 0;
    long validations = 0;
    char what[64];

    /* Invariant: after every op, the tree and the model agree on which keys
     * are present, which value pointer each maps to, and the count. */
    for (long op = 0; op < ops; op++) {
        unsigned long long r = rng_next();
        int k = (int)(r % KEYSPACE);
        int kind = (int)((r >> 32) % 10);   /* 0-3 insert, 4-6 delete, 7-9 find */

        if (kind < 4) {
            int *v = malloc(sizeof *v);
            if (v == NULL) {
                die(t, op, seed, "malloc", "out of memory in the fuzzer itself");
            }
            *v = (int)op;
            snprintf(what, sizeof what, "insert %s%s", keys[k],
                     m.present[k] ? " (overwrite)" : "");
            if (rb_insert(t, keys[k], v) != 0) {
                free(v);                    /* -1: caller still owns it */
                die(t, op, seed, what, "rb_insert returned -1");
            }
            if (!m.present[k]) {
                m.present[k] = true;
                m.count++;
            }
            m.value[k] = v;                 /* overwrite: tree freed the old one */
        } else if (kind < 7) {
            snprintf(what, sizeof what, "delete %s%s", keys[k],
                     m.present[k] ? "" : " (absent)");
            int rc = rb_delete(t, keys[k]);
            int want = m.present[k] ? 0 : -1;
            if (rc != want) {
                char d[64];
                snprintf(d, sizeof d, "rb_delete returned %d, expected %d", rc, want);
                die(t, op, seed, what, d);
            }
            if (m.present[k]) {
                m.present[k] = false;
                m.value[k] = NULL;
                m.count--;
            }
        } else {
            snprintf(what, sizeof what, "find %s%s", keys[k],
                     m.present[k] ? "" : " (absent)");
            void *got = rb_find(t, keys[k]);
            void *want = m.present[k] ? (void *)m.value[k] : NULL;
            if (got != want) {
                char d[64];
                snprintf(d, sizeof d, "rb_find returned %p, expected %p", got, want);
                die(t, op, seed, what, d);
            }
        }

        if (rb_size(t) != m.count) {
            char d[64];
            snprintf(d, sizeof d, "rb_size %zu, model has %zu", rb_size(t), m.count);
            die(t, op, seed, what, d);
        }
        if (m.count > max_size) {
            max_size = m.count;
        }

        if ((op + 1) % VALIDATE_EVERY == 0) {
            const char *bad = check_tree(t, &m);
            validations++;
            if (bad != NULL) {
                die(t, op, seed, what, bad);
            }
        }
    }

    /* One last full check, then tear down and make sure nothing leaked. */
    const char *bad = check_tree(t, &m);
    validations++;
    if (bad != NULL) {
        die(t, ops, seed, "final check", bad);
    }
    size_t final_size = m.count;
    rb_destroy(t);
    if (rb_debug_live_allocs() != 0) {
        fprintf(stderr, "fuzz: FAIL (seed %llu): %zu block(s) still live after "
                        "rb_destroy\n", seed, rb_debug_live_allocs());
        return 1;
    }

    printf("fuzz: %ld ops ok, seed %llu, %ld full checks, peak size %zu, "
           "final size %zu, 0 blocks leaked\n",
           ops, seed, validations, max_size, final_size);
    return 0;
}
