#include "sweep.h"
#include "fault_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Defined in tests/fault_alloc.c; not in any header on purpose. */
size_t rb_debug_live_allocs(void);

/* The scenario: insert NKEYS keys, delete the 100 even ones, overwrite 50 of
 * the odd ones (spec 8.1's example sizes). KEY_STRIDE shares no factor with
 * NKEYS, so (k * KEY_STRIDE) % NKEYS visits every index exactly once, in a
 * scrambled but fixed order. SWEEP_CAP turns a sweep that never reaches a
 * clean run into a reported failure instead of a hang. */
enum { NKEYS = 200, KEY_STRIDE = 37, SWEEP_CAP = 100000 };

static char keys[NKEYS][8];             /* "s000" .. "s199": index order = sorted */

/* ---- reporting -------------------------------------------------------- */

static int         sweep_failures;      /* failed checks in this call */
static const char *cur_label = "";      /* "plain", later "pooled" */
static long        cur_n;               /* which n is being swept (0 = self-test) */

/* One stderr line per failed check, tagged with the n that produced it, so a
 * failure arrives already saying which allocation was failed. */
#define SWEEP_CHECK(cond, ...)                                          \
    do {                                                                \
        if (!(cond)) {                                                  \
            sweep_failures++;                                           \
            fprintf(stderr, "sweep[%s] n=%ld: ", cur_label, cur_n);     \
            fprintf(stderr, __VA_ARGS__);                               \
            fprintf(stderr, "\n");                                      \
        }                                                               \
    } while (0)

/* ---- values and the counting destructor -------------------------------- */

/* Ownership is invisible to rb_validate, so count it: every value the tree
 * frees passes through here. A failed insert must leave this unchanged. */
static long sweep_values_freed;

static void sweep_free(void *p)
{
    sweep_values_freed++;
    free(p);
}

/* Values use plain malloc, not rb_malloc: they belong to the harness until
 * the tree accepts them, and the injector must never fail them. */
static int *new_value(int tag)
{
    int *v = malloc(sizeof *v);
    if (v == NULL) {
        fprintf(stderr, "sweep: out of memory in the harness itself\n");
        exit(2);
    }
    *v = tag;
    return v;
}

/* ---- the reference model --------------------------------------------- */

struct model {
    bool   present[NKEYS];
    int   *value[NKEYS];                /* the pointer the tree must hold */
    size_t count;
};

/* Every present key maps to its model value and every absent key is absent.
 * Reports only the first mismatch, so one bug is one line, not 200. */
static void check_model(const rbtree_t *t, const struct model *m,
                        const char *when)
{
    for (int i = 0; i < NKEYS; i++) {
        void *want = m->present[i] ? m->value[i] : NULL;
        void *got  = rb_find(t, keys[i]);
        if (got != want) {
            SWEEP_CHECK(false, "%s: rb_find(%s) is %p, model says %p",
                        when, keys[i], got, want);
            return;
        }
    }
}

/* Was allocation number fault_at made between these two totals? That is
 * exactly "this call is the one the injected fault landed on". */
static bool hit_fault(long total_before, long total_after, long fault_at)
{
    return fault_at > total_before && fault_at <= total_after;
}

/* ---- "exactly as it was": the five checks after a failed insert -------- */

struct before {
    size_t size;                        /* rb_size before the call */
    void  *find_k;                      /* rb_find(key) before the call */
    long   freed;                       /* destructor count before the call */
};

static void assert_unchanged(const rbtree_t *t, const struct model *m, int k,
                             const struct before *b)
{
    int    v    = rb_validate(t);
    size_t size = rb_size(t);
    void  *got  = rb_find(t, keys[k]);

    SWEEP_CHECK(v == 0, "failed insert %s: rb_validate returned %d",
                keys[k], v);                                        /* 1 */
    SWEEP_CHECK(size == b->size, "failed insert %s: rb_size %zu != %zu before",
                keys[k], size, b->size);                            /* 2 */
    SWEEP_CHECK(got == b->find_k, "failed insert %s: rb_find is %p, was %p before",
                keys[k], got, b->find_k);                           /* 3 */
    check_model(t, m, "after failed insert");                       /* 4 */
    SWEEP_CHECK(sweep_values_freed == b->freed,
                "failed insert %s: caller's value was freed (%ld destructor call(s))",
                keys[k], sweep_values_freed - b->freed);            /* 5 */
}

/* ---- one operation each ---------------------------------------------- */

/* Insert a key the tree does not hold yet. This is the only operation in the
 * scenario that allocates, so it is where the injected fault lands. */
static void insert_new(rbtree_t *t, struct model *m, int i, long fault_at,
                       long *accepted)
{
    int *v = new_value(i);
    struct before b = { rb_size(t), rb_find(t, keys[i]), sweep_values_freed };

    long total0 = fault_alloc_total();
    int  rc     = rb_insert(t, keys[i], v);
    bool hit    = hit_fault(total0, fault_alloc_total(), fault_at);

    if (rc == 0) {
        SWEEP_CHECK(!hit, "insert %s hit the injected fault but returned 0", keys[i]);
        m->present[i] = true;
        m->value[i]   = v;
        m->count++;
        (*accepted)++;
        return;
    }

    SWEEP_CHECK(rc == -1, "insert %s returned %d, expected 0 or -1", keys[i], rc);
    SWEEP_CHECK(hit, "insert %s returned -1 without an injected fault", keys[i]);
    assert_unchanged(t, m, i, &b);

    /* Ownership never moved, so v is still ours to free. If the tree freed it
     * anyway (check 5 just failed), freeing it again would be a double free
     * and crash the run, so only free it when the destructor did not run. */
    if (sweep_values_freed == b.freed) {
        free(v);
    }
}

/* Overwrite a key the tree already holds: must allocate nothing and free
 * exactly the old value. If the key is absent (its insert was the one that
 * failed), this is really a new insert. */
static void overwrite_key(rbtree_t *t, struct model *m, int i, long fault_at,
                          long *accepted)
{
    if (!m->present[i]) {
        insert_new(t, m, i, fault_at, accepted);
        return;
    }

    int *v      = new_value(1000 + i);
    long freed0 = sweep_values_freed;
    long total0 = fault_alloc_total();
    int  rc     = rb_insert(t, keys[i], v);
    long allocs = fault_alloc_total() - total0;

    SWEEP_CHECK(allocs == 0, "overwrite %s made %ld allocation(s); it must make none",
                keys[i], allocs);
    SWEEP_CHECK(rc == 0, "overwrite %s returned %d", keys[i], rc);
    if (rc != 0) {
        free(v);                        /* not accepted: still ours */
        return;
    }
    SWEEP_CHECK(sweep_values_freed == freed0 + 1,
                "overwrite %s freed %ld old value(s), expected 1",
                keys[i], sweep_values_freed - freed0);
    m->value[i] = v;
    (*accepted)++;
}

/* Delete: must allocate nothing, return 0 and free the value if present,
 * return -1 if absent (e.g. its insert was the one that failed). */
static void delete_key(rbtree_t *t, struct model *m, int i)
{
    long freed0 = sweep_values_freed;
    long total0 = fault_alloc_total();
    int  rc     = rb_delete(t, keys[i]);
    long allocs = fault_alloc_total() - total0;

    SWEEP_CHECK(allocs == 0, "delete %s made %ld allocation(s); it must make none",
                keys[i], allocs);
    if (!m->present[i]) {
        SWEEP_CHECK(rc == -1, "delete of absent %s returned %d", keys[i], rc);
        return;
    }
    SWEEP_CHECK(rc == 0, "delete %s returned %d", keys[i], rc);
    if (rc == 0) {
        SWEEP_CHECK(sweep_values_freed == freed0 + 1,
                    "delete %s freed %ld value(s), expected 1",
                    keys[i], sweep_values_freed - freed0);
        m->present[i] = false;
        m->value[i]   = NULL;
        m->count--;
    }
}

/* ---- the scenario ---------------------------------------------------- */

/* One full run. fault_at is the absolute allocation number that fails. The
 * run keeps going after a failed operation (the fault fires once), which
 * also proves the tree is still usable afterwards. */
static void run_scenario(rbtree_t *(*create)(rb_value_free_fn), long fault_at)
{
    struct model m = {0};
    long accepted = 0;                  /* values the tree took ownership of */
    long freed0   = sweep_values_freed;

    /* Leaks are measured against the count at the start of THIS run, not
     * against 0: the counter is never reset, so one leaked block would
     * otherwise make every later run fail too and hide which n leaked. */
    size_t live0 = rb_debug_live_allocs();

    long     total0 = fault_alloc_total();
    rbtree_t *t     = create(sweep_free);
    bool     hit    = hit_fault(total0, fault_alloc_total(), fault_at);

    if (t == NULL) {
        SWEEP_CHECK(hit, "create returned NULL without an injected fault");
        SWEEP_CHECK(rb_debug_live_allocs() == live0,
                    "failed create leaked %zu block(s) during this run",
                    rb_debug_live_allocs() - live0);
        return;
    }
    SWEEP_CHECK(!hit, "create hit the injected fault but returned a tree");

    for (int k = 0; k < NKEYS; k++) {                   /* phase 1: 200 inserts */
        insert_new(t, &m, (k * KEY_STRIDE) % NKEYS, fault_at, &accepted);
    }
    for (int i = 0; i < NKEYS; i += 2) {                /* phase 2: 100 deletes */
        delete_key(t, &m, i);
    }
    for (int i = 1; i < NKEYS; i += 4) {                /* phase 3: 50 overwrites */
        overwrite_key(t, &m, i, fault_at, &accepted);
    }

    int v = rb_validate(t);
    SWEEP_CHECK(v == 0, "end of run: rb_validate returned %d", v);
    SWEEP_CHECK(rb_size(t) == m.count, "end of run: rb_size %zu, model %zu",
                rb_size(t), m.count);
    check_model(t, &m, "end of run");

    rb_destroy(t);
    SWEEP_CHECK(rb_debug_live_allocs() == live0,
                "leaked %zu block(s) during this run (checked after rb_destroy)",
                rb_debug_live_allocs() - live0);
    SWEEP_CHECK(sweep_values_freed - freed0 == accepted,
                "ownership: tree accepted %ld value(s) but freed %ld",
                accepted, sweep_values_freed - freed0);
}

/* ---- public entry points --------------------------------------------- */

int fault_sweep(rbtree_t *(*create)(rb_value_free_fn value_free),
                const char *label)
{
    for (int i = 0; i < NKEYS; i++) {
        snprintf(keys[i], sizeof keys[i], "s%03d", i);
    }
    sweep_failures = 0;
    cur_label      = label;

    long runs = 0, faults = 0, clean_allocs = -1;

    /* Invariant: every run 1..n-1 had its fault fire, i.e. made at least
     * that many allocations. The first run that makes fewer than n is the
     * clean run, and the scenario's allocation count is finite, so it comes. */
    for (long n = 1; n <= SWEEP_CAP; n++) {
        cur_n = n;
        long start = fault_alloc_total();
        fault_alloc_arm(n);
        run_scenario(create, start + n);
        fault_alloc_disarm();           /* never leak an armed fault into the next run */
        runs++;

        long allocs = fault_alloc_total() - start;
        if (allocs < n) {               /* fault never fired: clean run, done */
            clean_allocs = allocs;
            break;
        }
        faults++;
    }
    cur_n = 0;
    SWEEP_CHECK(clean_allocs >= 0, "no clean run within %d runs: sweep did not terminate",
                SWEEP_CAP);

    printf("sweep[%s]: %ld runs, %ld faults injected, %ld allocations in a clean run, "
           "%d failure(s)\n", label, runs, faults, clean_allocs, sweep_failures);
    return sweep_failures;
}

int fault_injector_selftest(void)
{
    sweep_failures = 0;
    cur_label      = "selftest";
    cur_n          = 0;

    size_t live0  = rb_debug_live_allocs();
    long   total0 = fault_alloc_total();

    fault_alloc_arm(1);
    void *a = rb_malloc(16);
    SWEEP_CHECK(a == NULL, "arm(1): the next rb_malloc should fail");
    SWEEP_CHECK(rb_debug_live_allocs() == live0, "a failed rb_malloc changed the live count");
    void *b = rb_malloc(16);
    SWEEP_CHECK(b != NULL, "arm(1) should fire once, then allow the next one");

    fault_alloc_arm(3);
    void *c1 = rb_malloc(16);
    void *c2 = rb_malloc(16);
    void *c3 = rb_malloc(16);
    SWEEP_CHECK(c1 != NULL && c2 != NULL, "arm(3): the 1st and 2nd should succeed");
    SWEEP_CHECK(c3 == NULL, "arm(3): the 3rd should fail");

    fault_alloc_arm(2);
    fault_alloc_disarm();
    void *d1 = rb_malloc(16);
    void *d2 = rb_malloc(16);
    SWEEP_CHECK(d1 != NULL && d2 != NULL, "disarm should cancel a pending arm");

    SWEEP_CHECK(fault_alloc_total() - total0 == 7,
                "total moved by %ld, expected 7 (failed calls count too)",
                fault_alloc_total() - total0);

    rb_free(b);
    rb_free(c1);
    rb_free(c2);
    rb_free(d1);
    rb_free(d2);
    SWEEP_CHECK(rb_debug_live_allocs() == live0, "self-test left %zu block(s) allocated",
                rb_debug_live_allocs() - live0);

    if (sweep_failures == 0) {
        printf("fault injector self-test: ok\n");
    }
    return sweep_failures;
}
