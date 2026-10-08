#include "rbtree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Defined in src/rbtree.c. Declared here because rbtree.h is frozen and this
 * is a test-only hook, not part of the public contract. */
size_t rb_debug_live_allocs(void);

/* ---- tiny test harness ------------------------------------------------ */

static int failures = 0;

/* If cond is false, print where and why, and count the failure. Keeps going
 * so one run reports every problem, not just the first. */
#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            failures++;                                               \
            fprintf(stderr, "  FAIL line %d: ", __LINE__);            \
            fprintf(stderr, __VA_ARGS__);                             \
            fprintf(stderr, "\n");                                    \
        }                                                             \
    } while (0)

/* A value_free that counts how many times it was called, so tests can prove
 * the tree freed exactly the values it was supposed to -- no more, no fewer. */
static int values_freed = 0;
static void counting_free(void *p)
{
    values_freed++;
    free(p);
}

/* Every test ends the same way: the tree is gone and nothing leaked. */
static void expect_clean(const char *test)
{
    CHECK(rb_debug_live_allocs() == 0, "%s leaked %zu block(s)",
          test, rb_debug_live_allocs());
}

/* ---- tests ------------------------------------------------------------ */

static void test_empty_tree(void)
{
    rbtree_t *t = rb_create(NULL);
    CHECK(t != NULL, "rb_create returned NULL");
    CHECK(rb_size(t) == 0, "fresh tree size %zu, expected 0", rb_size(t));
    CHECK(rb_validate(t) == 0, "empty tree failed validate");
    CHECK(rb_find(t, "anything") == NULL, "find on empty tree not NULL");
    rb_destroy(t);
    rb_destroy(NULL);           /* the header promises this is safe */
    expect_clean("test_empty_tree");
}

static void test_insert_and_find(void)
{
    /* Deliberately scrambled so the tree is not a straight line. */
    const char *keys[] = { "mango", "apple", "zebra", "kiwi", "banana",
                           "cherry", "peach" };
    const size_t n = sizeof keys / sizeof keys[0];
    int values[7] = { 10, 20, 30, 40, 50, 60, 70 };

    rbtree_t *t = rb_create(NULL);      /* NULL: tree does not own values */

    for (size_t i = 0; i < n; i++) {
        CHECK(rb_insert(t, keys[i], &values[i]) == 0,
              "insert %s failed", keys[i]);
        CHECK(rb_size(t) == i + 1,
              "after inserting %s size is %zu, expected %zu",
              keys[i], rb_size(t), i + 1);
        CHECK(rb_validate(t) == 0,
              "validate failed with code %d after inserting %s",
              rb_validate(t), keys[i]);
    }

    /* Every key finds its own value, and the pointer is the same one. */
    for (size_t i = 0; i < n; i++) {
        CHECK(rb_find(t, keys[i]) == &values[i],
              "find %s returned wrong value", keys[i]);
    }

    CHECK(rb_find(t, "missing") == NULL, "find of absent key not NULL");
    CHECK(rb_find(t, "") == NULL, "find of empty string not NULL");
    CHECK(rb_find(t, "appl") == NULL, "find of a prefix matched a key");

    rb_destroy(t);
    expect_clean("test_insert_and_find");
}

static void test_overwrite_frees_old_value(void)
{
    values_freed = 0;
    rbtree_t *t = rb_create(counting_free);    /* tree OWNS values now */

    int *first = malloc(sizeof *first);
    int *second = malloc(sizeof *second);
    *first = 1;
    *second = 2;

    CHECK(rb_insert(t, "key", first) == 0, "first insert failed");
    CHECK(rb_size(t) == 1, "size after first insert %zu", rb_size(t));

    /* Same key again: value replaced, old value freed, size unchanged. */
    CHECK(rb_insert(t, "key", second) == 0, "overwrite failed");
    CHECK(rb_size(t) == 1, "size after overwrite %zu, expected 1", rb_size(t));
    CHECK(values_freed == 1, "expected old value freed once, got %d",
          values_freed);
    CHECK(rb_find(t, "key") == second, "find did not return the new value");
    CHECK(rb_validate(t) == 0, "validate failed after overwrite");

    rb_destroy(t);
    CHECK(values_freed == 2, "expected 2 values freed total, got %d",
          values_freed);
    expect_clean("test_overwrite_frees_old_value");
}

static void test_overwrite_same_pointer_is_noop(void)
{
    /* Re-inserting the SAME value pointer under the same key must not free
     * it: the tree already owns it, and freeing it would leave a dangling
     * pointer for rb_destroy to free a second time. Under ASan the old code
     * aborts with a double-free here; without ASan the counts catch it. */
    values_freed = 0;
    rbtree_t *t = rb_create(counting_free);
    CHECK(t != NULL, "rb_create returned NULL");

    int *v = malloc(sizeof *v);
    CHECK(v != NULL, "malloc failed");
    *v = 7;

    CHECK(rb_insert(t, "key", v) == 0, "first insert failed");
    CHECK(rb_insert(t, "key", v) == 0, "same-pointer re-insert failed");
    CHECK(values_freed == 0, "re-inserting the same pointer freed it (%d)",
          values_freed);
    CHECK(rb_find(t, "key") == v, "find no longer returns the value");
    CHECK(rb_size(t) == 1, "size %zu, expected 1", rb_size(t));

    rb_destroy(t);
    CHECK(values_freed == 1, "expected exactly 1 free on destroy, got %d",
          values_freed);
    expect_clean("test_overwrite_same_pointer_is_noop");
}

static void test_destroy_frees_every_value(void)
{
    values_freed = 0;
    rbtree_t *t = rb_create(counting_free);
    const int n = 25;

    for (int i = 0; i < n; i++) {
        char key[16];
        snprintf(key, sizeof key, "k%02d", i);
        int *v = malloc(sizeof *v);
        *v = i;
        CHECK(rb_insert(t, key, v) == 0, "insert %s failed", key);
    }
    CHECK(rb_size(t) == (size_t)n, "size %zu, expected %d", rb_size(t), n);
    CHECK(rb_validate(t) == 0, "validate failed, code %d", rb_validate(t));

    rb_destroy(t);
    CHECK(values_freed == n, "destroy freed %d values, expected %d",
          values_freed, n);
    expect_clean("test_destroy_frees_every_value");
}

static void test_keys_are_copied(void)
{
    /* The caller's string buffer changes after insert. The tree must still
     * find the key by its ORIGINAL contents, proving it made its own copy. */
    rbtree_t *t = rb_create(NULL);
    char buf[16];
    int v = 99;

    strcpy(buf, "original");
    CHECK(rb_insert(t, buf, &v) == 0, "insert failed");

    strcpy(buf, "clobbered");            /* caller reuses the buffer */

    CHECK(rb_find(t, "original") == &v, "tree lost the key after caller "
                                          "changed the buffer -- key not copied?");
    CHECK(rb_find(t, "clobbered") == NULL, "tree followed the caller's buffer");

    rb_destroy(t);
    expect_clean("test_keys_are_copied");
}


/* ---- balancing tests ---------------------------------------------------
 * None of these can see inside a node. They prove balance the only way a
 * test can from outside: rb_validate's black-height check. A straight line
 * of nodes cannot have equal black-heights on every path, so if validate
 * passes after every insert, the tree stayed balanced.
 * --------------------------------------------------------------------- */

/* Insert keys in the given order, validating after every one. */
static void insert_all(rbtree_t *t, const char *const *keys, size_t n,
                       const char *test)
{
    for (size_t i = 0; i < n; i++) {
        CHECK(rb_insert(t, keys[i], NULL) == 0,
              "%s: insert %s failed", test, keys[i]);
        CHECK(rb_size(t) == i + 1,
              "%s: after %s size is %zu, expected %zu",
              test, keys[i], rb_size(t), i + 1);
        int rc = rb_validate(t);
        CHECK(rc == 0, "%s: validate code %d after inserting %s",
              test, rc, keys[i]);
    }
}

/* Values are NULL in these tests, so rb_find can't distinguish "found" from
 * "absent". Insert a second time instead: an overwrite keeps size the same,
 * a miss would grow it. That is a real reachability check. */
static void expect_reachable(rbtree_t *t, const char *const *keys, size_t n,
                             const char *test)
{
    size_t before = rb_size(t);
    for (size_t i = 0; i < n; i++) {
        CHECK(rb_insert(t, keys[i], NULL) == 0, "%s: re-insert failed", test);
    }
    CHECK(rb_size(t) == before,
          "%s: re-inserting existing keys changed size %zu -> %zu; "
          "some key became unreachable (stale root?)",
          test, before, rb_size(t));
}

static void test_stale_root_left_rotation(void)
{
    /* a, b, c ascending: the third insert rotates LEFT at the root.
     * b becomes the new root. If t->root still pointed at a, a would have
     * no children and validate would report the wrong count. */
    const char *const keys[] = { "a", "b", "c" };
    rbtree_t *t = rb_create(NULL);
    insert_all(t, keys, 3, "stale_root_left");
    expect_reachable(t, keys, 3, "stale_root_left");
    rb_destroy(t);
    expect_clean("test_stale_root_left_rotation");
}

static void test_stale_root_right_rotation(void)
{
    /* Mirror: c, b, a descending forces a RIGHT rotation at the root. */
    const char *const keys[] = { "c", "b", "a" };
    rbtree_t *t = rb_create(NULL);
    insert_all(t, keys, 3, "stale_root_right");
    expect_reachable(t, keys, 3, "stale_root_right");
    rb_destroy(t);
    expect_clean("test_stale_root_right_rotation");
}

static void test_zigzag_left_right(void)
{
    /* a, c, b: b hangs under c which hangs under a -- a kink. Fixing it
     * takes TWO rotations (case 2 to straighten, case 3 to repair). */
    const char *const keys[] = { "a", "c", "b" };
    rbtree_t *t = rb_create(NULL);
    insert_all(t, keys, 3, "zigzag_lr");
    expect_reachable(t, keys, 3, "zigzag_lr");
    rb_destroy(t);
    expect_clean("test_zigzag_left_right");
}

static void test_zigzag_right_left(void)
{
    /* Mirror kink: c, a, b. */
    const char *const keys[] = { "c", "a", "b" };
    rbtree_t *t = rb_create(NULL);
    insert_all(t, keys, 3, "zigzag_rl");
    expect_reachable(t, keys, 3, "zigzag_rl");
    rb_destroy(t);
    expect_clean("test_zigzag_right_left");
}

static void test_red_uncle_recolor(void)
{
    /* b, a, c: b black root with two red children. Then d goes under c.
     * d's parent c is red and its uncle a is red -> case 1, recolor only.
     * Then e under d: parent d red, uncle NULL (black) -> rotation. */
    const char *const keys[] = { "b", "a", "c", "d", "e" };
    rbtree_t *t = rb_create(NULL);
    insert_all(t, keys, 5, "red_uncle");
    expect_reachable(t, keys, 5, "red_uncle");
    rb_destroy(t);
    expect_clean("test_red_uncle_recolor");
}

static void test_ascending_stress(void)
{
    /* Worst case for an unbalanced tree: 1000 keys in sorted order would
     * make a 1000-deep straight line. Validate after every insert proves
     * the fixup kept it balanced, and hits every case many times over. */
    rbtree_t *t = rb_create(NULL);
    char key[16];
    for (int i = 0; i < 1000; i++) {
        snprintf(key, sizeof key, "k%04d", i);
        CHECK(rb_insert(t, key, NULL) == 0, "ascending: insert %s", key);
        int rc = rb_validate(t);
        CHECK(rc == 0, "ascending: validate code %d after %s", rc, key);
        if (rc != 0) break;                 /* one failure is enough */
    }
    CHECK(rb_size(t) == 1000, "ascending: size %zu", rb_size(t));
    rb_destroy(t);
    expect_clean("test_ascending_stress");
}

static void test_random_stress(void)
{
    /* 2000 distinct keys in a shuffled order. Fixed seed so a failure
     * reproduces exactly. Exercises the mirror cases the ordered inserts
     * don't reach as often. */
    enum { N = 2000 };
    static int order[N];
    for (int i = 0; i < N; i++) {
        order[i] = i;
    }
    srand(20260914);
    for (int i = N - 1; i > 0; i--) {       /* Fisher-Yates shuffle */
        int j = rand() % (i + 1);
        int tmp = order[i];
        order[i] = order[j];
        order[j] = tmp;
    }

    rbtree_t *t = rb_create(NULL);
    char key[16];
    for (int i = 0; i < N; i++) {
        snprintf(key, sizeof key, "r%04d", order[i]);
        CHECK(rb_insert(t, key, NULL) == 0, "random: insert %s", key);
        int rc = rb_validate(t);
        CHECK(rc == 0, "random: validate code %d after op %d (%s)",
              rc, i, key);
        if (rc != 0) break;
    }
    CHECK(rb_size(t) == N, "random: size %zu", rb_size(t));
    rb_destroy(t);
    expect_clean("test_random_stress");
}

/* ---- foreach tests ------------------------------------------------------ */

/* Callback context: records every (key, value) the walk hands us, in the
 * order it hands them. Overflow is counted, not written. */
enum { COLLECT_MAX = 16 };
struct collect {
    const char *keys[COLLECT_MAX];
    void       *values[COLLECT_MAX];
    size_t      n;
};

static void collect_cb(const char *key, void *value, void *ctx)
{
    struct collect *c = ctx;
    if (c->n < COLLECT_MAX) {
        c->keys[c->n] = key;
        c->values[c->n] = value;
    }
    c->n++;
}

static void test_foreach_in_order(void)
{
    /* Scrambled insert; the walk must produce sorted order, each key paired
     * with its OWN value, each exactly once. */
    const char *keys[]   = { "mango", "apple", "zebra", "kiwi", "banana",
                             "cherry", "peach" };
    const char *sorted[] = { "apple", "banana", "cherry", "kiwi", "mango",
                             "peach", "zebra" };
    const size_t n = sizeof keys / sizeof keys[0];
    int values[sizeof keys / sizeof keys[0]];

    rbtree_t *t = rb_create(NULL);
    CHECK(t != NULL, "rb_create returned NULL");
    for (size_t i = 0; i < n; i++) {
        values[i] = (int)i;
        CHECK(rb_insert(t, keys[i], &values[i]) == 0, "insert %s failed", keys[i]);
    }

    struct collect c = { .n = 0 };
    rb_foreach(t, collect_cb, &c);
    CHECK(c.n == n, "foreach visited %zu keys, expected %zu", c.n, n);

    for (size_t j = 0; j < n && j < c.n && j < COLLECT_MAX; j++) {
        CHECK(strcmp(c.keys[j], sorted[j]) == 0,
              "foreach position %zu is %s, expected %s", j, c.keys[j], sorted[j]);
        /* The value handed back must be the one inserted WITH that key. */
        int *expected = NULL;
        for (size_t i = 0; i < n; i++) {
            if (strcmp(keys[i], sorted[j]) == 0) {
                expected = &values[i];
            }
        }
        CHECK(c.values[j] == expected, "foreach gave %s the wrong value", sorted[j]);
    }

    rb_destroy(t);
    expect_clean("test_foreach_in_order");
}

static void test_foreach_empty_and_after_delete(void)
{
    rbtree_t *t = rb_create(NULL);
    CHECK(t != NULL, "rb_create returned NULL");

    struct collect c = { .n = 0 };
    rb_foreach(t, collect_cb, &c);
    CHECK(c.n == 0, "foreach on an empty tree called fn %zu times", c.n);

    int v = 0;
    CHECK(rb_insert(t, "b", &v) == 0, "insert b failed");
    CHECK(rb_insert(t, "a", &v) == 0, "insert a failed");
    CHECK(rb_insert(t, "c", &v) == 0, "insert c failed");
    CHECK(rb_delete(t, "b") == 0, "delete b failed");

    c.n = 0;
    rb_foreach(t, collect_cb, &c);
    CHECK(c.n == 2, "foreach after delete visited %zu keys, expected 2", c.n);
    CHECK(c.n == 2 && strcmp(c.keys[0], "a") == 0 && strcmp(c.keys[1], "c") == 0,
          "foreach after deleting b did not yield a, c");

    rb_destroy(t);
    expect_clean("test_foreach_empty_and_after_delete");
}

/* ---- delete tests (M2) --------------------------------------------------
 * rb_delete has to handle several tree shapes, and a test cannot see node
 * colors. So each case picks an insertion order whose resulting shape is
 * forced by the insert rules -- every shape below was dumped from a debug
 * build, not guessed -- then deletes the key sitting in the target spot.
 *
 * What a test CAN see from outside: the return code, rb_size, rb_find on
 * every key (values are distinct malloc'd ints, so found vs. absent is
 * unambiguous), counting_free (a delete frees exactly one value),
 * rb_validate (a black leaf that is simply unlinked breaks rule 5 -> code 7,
 * so validate is what proves the fixup ran), and rb_debug_live_allocs (the
 * node and its key copy both came back).
 *
 * Notation in the comments: key(R|B)[left, right], "-" is an empty child.
 * ---------------------------------------------------------------------- */

enum { MAX_CASE_KEYS = 8 };

/* One case = an insertion order (which fixes the shape) and a deletion
 * order. Every key is a single letter, so both are plain strings: "bacd"
 * inserts b, a, c, d and "da" deletes d, then a. The full set of checks runs
 * after EVERY delete, so a two-step row (drop a red leaf to reach a shape,
 * then hit the case) and a drain (delete everything) are the same code path
 * as a one-delete row. */
struct del_case {
    const char *name;
    const char *insert;
    const char *victims;
};

static bool deleted_before(const char *victims, size_t upto, char key)
{
    for (size_t j = 0; j < upto; j++) {
        if (victims[j] == key) {
            return true;
        }
    }
    return false;
}

/* Insert every key with its own malloc'd int value (its index in the
 * insertion order), so a test can prove rb_find still returns the right
 * pointer after a delete. */
static rbtree_t *build_owned(const struct del_case *c, int *vals[])
{
    size_t n = strlen(c->insert);
    if (n > MAX_CASE_KEYS) {
        fprintf(stderr, "%s: %zu keys, harness holds %d\n", c->name, n, MAX_CASE_KEYS);
        exit(1);
    }
    rbtree_t *t = rb_create(counting_free);
    if (t == NULL) {
        fprintf(stderr, "%s: rb_create failed\n", c->name);
        exit(1);
    }
    for (size_t i = 0; i < n; i++) {
        char key[2] = { c->insert[i], '\0' };
        vals[i] = malloc(sizeof *vals[i]);
        if (vals[i] == NULL) {
            fprintf(stderr, "%s: malloc failed\n", c->name);
            exit(1);
        }
        *vals[i] = (int)i;
        CHECK(rb_insert(t, key, vals[i]) == 0, "%s: insert %s failed", c->name, key);
    }
    CHECK(rb_validate(t) == 0, "%s: setup tree failed validate", c->name);
    return t;
}

static void run_delete_case(const struct del_case *c)
{
    int *vals[MAX_CASE_KEYS];
    const size_t n = strlen(c->insert);
    const size_t nv = strlen(c->victims);
    values_freed = 0;
    rbtree_t *t = build_owned(c, vals);

    for (size_t j = 0; j < nv; j++) {
        char victim[2] = { c->victims[j], '\0' };
        int rc = rb_delete(t, victim);
        CHECK(rc == 0, "%s: rb_delete(%s) returned %d", c->name, victim, rc);
        if (rc != 0) {
            break;                  /* everything below would fail for the same reason */
        }

        CHECK(rb_size(t) == n - (j + 1), "%s: size %zu after deleting %s, expected %zu",
              c->name, rb_size(t), victim, n - (j + 1));
        CHECK(rb_find(t, victim) == NULL, "%s: %s still found after delete",
              c->name, victim);
        CHECK(values_freed == (int)(j + 1), "%s: %d values freed after %zu deletes",
              c->name, values_freed, j + 1);

        /* Every survivor still answers with its ORIGINAL pointer, and the
         * int behind it is intact. Reading through it is deliberate: under
         * ASan that read is what catches "freed the wrong value". */
        for (size_t i = 0; i < n; i++) {
            if (deleted_before(c->victims, j + 1, c->insert[i])) {
                continue;
            }
            char key[2] = { c->insert[i], '\0' };
            int *p = rb_find(t, key);
            CHECK(p == vals[i], "%s: after deleting %s, find %s gave the wrong pointer",
                  c->name, victim, key);
            CHECK(p != NULL && *p == (int)i, "%s: value behind %s is damaged",
                  c->name, key);
        }

        int v = rb_validate(t);
        CHECK(v == 0, "%s: validate code %d after deleting %s", c->name, v, victim);
        if (v != 0) {
            break;                  /* a broken tree is not safe to keep deleting from */
        }
    }

    rb_destroy(t);
    CHECK(values_freed == (int)n, "%s: %d of %zu values freed by the end",
          c->name, values_freed, n);
    expect_clean(c->name);
}

/* Shapes below were all dumped from a debug build, including the
 * intermediate shape after a preparatory delete -- none are guessed. */
static const struct del_case delete_cases[] = {
    /* -- no fixup needed ------------------------------------------------
     * b(B)[a(R), c(R)]: a red leaf just goes away. The black count on that
     * path was never counting it. */
    { "red_leaf_left",  "bac", "a" },
    { "red_leaf_right", "bac", "c" },
    /* b(B)[a(B), d(B)[c(R), -]]: d is black with ONE child, which the rules
     * force to be red. c moves up and turns black; no loop needed. */
    { "black_one_red_child_left",  "badc", "d" },
    /* Mirror: c(B)[b(B), d(B)[-, e(R)]]. */
    { "black_one_red_child_right", "cbde", "d" },

    /* -- two children: hoist the successor -------------------------------
     * d(B)[b(B)[a(R), c(R)], f(B)[e(R), g(R)]]: b's successor c is its own
     * right child, a red leaf. b's value must be the one freed -- not c's --
     * and c must still be found at its own pointer. */
    { "two_children_left",  "dbfaceg", "b" },
    { "two_children_right", "dbfaceg", "f" },
    /* Same tree, delete the root d: successor e is two levels down. Result
     * e(B)[b(B)[a(R), c(R)], f(B)[-, g(R)]]. */
    { "two_children_deep_successor", "dbfaceg", "d" },
    /* b(B)[a(B), d(R)[c(B), e(B)[-, f(R)]]]: d's successor e has a red right
     * child f, so the one-child splice happens INSIDE the reduction.
     * Result b(B)[a(B), e(R)[c(B), f(B)]]. */
    { "two_children_successor_has_child", "badcef", "d" },

    /* -- the root, in every shape it can have ----------------------------
     * Whoever takes over must end up black, and t->root must move. */
    { "root_two_children",    "bac", "b" },     /* b(B)[a(R), c(R)] */
    { "root_one_right_child", "ab",  "a" },     /* a(B)[-, b(R)]    */
    { "root_one_left_child",  "ba",  "b" },     /* b(B)[a(R), -]    */

    /* -- fixup case 1: red sibling ----------------------------------------
     * b(B)[a(B), d(R)[c(B), e(B)[-, f(R)]]]: a is a black leaf and its
     * sibling d is red. Unlinking a alone leaves the left side one black
     * short (validate code 7). Rotate at b to get a black sibling (c), which
     * has no red child -> case 2: c turns red, the debt lands on b, which is
     * now red, so it turns black. Done. */
    { "case1_red_sibling_left",  "badcef", "a" },
    /* Mirror: e(B)[c(R)[b(B)[a(R), -], d(B)], f(B)], delete f. */
    { "case1_red_sibling_right", "efcdba", "f" },

    /* -- fixup case 2: black sibling, no red child -----------------------
     * b(B)[a(B), c(B)[-, d(R)]]: first drop the red leaf d, leaving
     * b(B)[a(B), c(B)]. Then delete a: sibling c is black with no red child.
     * c turns red, the debt moves up to b -- the root -- and dissolves.
     * Result b(B)[-, c(R)]. */
    { "case2_root_absorbs_left",  "bacd", "da" },
    /* Mirror: c(B)[b(B)[a(R), -], d(B)] -> drop a -> delete d. */
    { "case2_root_absorbs_right", "cdba", "ad" },
    /* b(B)[a(B), d(R)[c(B), e(B)[-, f(R)]]]: drop f, leaving
     * b(B)[a(B), d(R)[c(B), e(B)]]. Delete c: sibling e is black with no red
     * child, so the debt moves up to d -- which is RED, so it just turns
     * black. The cheap case: no rotation. Result b(B)[a(B), d(B)[-, e(R)]]. */
    { "case2_red_parent_absorbs_left",  "badcef", "fc" },
    /* Mirror: e(B)[c(R)[b(B)[a(R), -], d(B)], f(B)] -> drop a -> delete d. */
    { "case2_red_parent_absorbs_right", "efcdba", "ad" },

    /* -- fixup case 3 -> 4: sibling's red child on the NEAR side ----------
     * b(B)[a(B), d(B)[c(R), -]]: delete a. Sibling d's only red child c is on
     * the near side. Rotate at d to push it to the far side, then case 4.
     * Result c(B)[b(B), d(B)]. */
    { "case3_near_red_left",  "badc", "a" },
    /* Mirror: c(B)[a(B)[-, b(R)], d(B)], delete d. Result b(B)[a(B), c(B)]. */
    { "case3_near_red_right", "cdab", "d" },

    /* -- fixup case 4: sibling's red child on the FAR side ----------------
     * b(B)[a(B), c(B)[-, d(R)]]: delete a. One rotation at b and a recolor.
     * Result c(B)[b(B), d(B)]. */
    { "case4_far_red_left",  "bacd", "a" },
    /* Mirror: c(B)[b(B)[a(R), -], d(B)], delete d. Result b(B)[a(B), c(B)]. */
    { "case4_far_red_right", "cdba", "d" },

    /* -- drains: delete everything, three orders, checks after each -------
     * These reach the multi-level climbs a single delete never does. */
    { "drain_insertion_order", "dbfaceg", "dbfaceg" },
    { "drain_reverse",         "dbfaceg", "gecafbd" },
    { "drain_sorted",          "dbfaceg", "abcdefg" },
};

static void test_delete_cases(void)
{
    const size_t n = sizeof delete_cases / sizeof delete_cases[0];
    for (size_t i = 0; i < n; i++) {
        run_delete_case(&delete_cases[i]);
    }
}

static void test_delete_only_node_then_reuse(void)
{
    /* a(B) alone. Deleting it must leave t->root NULL, not dangling: the
     * follow-up insert would otherwise write through a freed node. */
    values_freed = 0;
    rbtree_t *t = rb_create(counting_free);
    CHECK(t != NULL, "rb_create returned NULL");

    int *v = malloc(sizeof *v);
    CHECK(v != NULL, "malloc failed");
    *v = 1;
    CHECK(rb_insert(t, "a", v) == 0, "insert a failed");

    int rc = rb_delete(t, "a");
    CHECK(rc == 0, "only_node: rb_delete(a) returned %d", rc);
    if (rc == 0) {
        CHECK(rb_size(t) == 0, "only_node: size %zu, expected 0", rb_size(t));
        CHECK(rb_find(t, "a") == NULL, "only_node: a still found");
        CHECK(values_freed == 1, "only_node: freed %d values, expected 1",
              values_freed);
        CHECK(rb_validate(t) == 0, "only_node: empty tree failed validate");

        /* The tree must be fully reusable afterward. */
        int *w = malloc(sizeof *w);
        CHECK(w != NULL, "malloc failed");
        *w = 2;
        CHECK(rb_insert(t, "z", w) == 0, "only_node: insert after delete failed");
        CHECK(rb_find(t, "z") == w, "only_node: z not found after re-insert");
        CHECK(rb_size(t) == 1, "only_node: size %zu after re-insert", rb_size(t));
        CHECK(rb_validate(t) == 0, "only_node: validate failed after re-insert");
    }

    rb_destroy(t);
    expect_clean("test_delete_only_node_then_reuse");
}

static void test_delete_absent(void)
{
    /* Misses must return -1 and touch nothing: no size change, no value
     * freed, tree still valid. Note the current stub returns -1 for
     * everything, so most of this passes for free until delete is real --
     * it earns its keep as a regression test afterward. */
    values_freed = 0;
    rbtree_t *t = rb_create(counting_free);
    CHECK(t != NULL, "rb_create returned NULL");

    CHECK(rb_delete(t, "anything") == -1, "delete on empty tree not -1");
    CHECK(rb_size(t) == 0, "delete on empty tree changed size");

    int *v = malloc(sizeof *v);
    CHECK(v != NULL, "malloc failed");
    *v = 1;
    CHECK(rb_insert(t, "b", v) == 0, "insert b failed");

    CHECK(rb_delete(t, "a") == -1, "absent key (left of b) not -1");
    CHECK(rb_delete(t, "c") == -1, "absent key (right of b) not -1");
    CHECK(rb_delete(t, "") == -1, "absent empty-string key not -1");
    CHECK(rb_delete(t, "bb") == -1, "absent key sharing a prefix not -1");
    CHECK(rb_size(t) == 1, "absent-key deletes changed size to %zu", rb_size(t));
    CHECK(values_freed == 0, "absent-key delete freed %d values", values_freed);
    CHECK(rb_find(t, "b") == v, "b lost after absent-key deletes");
    CHECK(rb_validate(t) == 0, "validate failed after absent-key deletes");

    /* Deleting the same key twice: the second one is a miss. */
    int rc = rb_delete(t, "b");
    CHECK(rc == 0, "absent: first delete of b returned %d", rc);
    CHECK(rb_delete(t, "b") == -1, "second delete of b not -1");
    if (rc == 0) {
        CHECK(values_freed == 1, "double delete freed %d values, expected 1",
              values_freed);
        CHECK(rb_size(t) == 0, "size %zu after deleting b, expected 0",
              rb_size(t));
    }

    rb_destroy(t);
    expect_clean("test_delete_absent");
}

/* ---- hardening tests (M3) ----------------------------------------------
 * The edge cases nobody fuzzes into by accident. The fuzzer's op mix keeps
 * the tree at a few hundred keys, so it never sits at zero or one node for
 * long, and every key it uses is five characters. Each test here pins one
 * such corner and runs EVERY operation against it.
 * ---------------------------------------------------------------------- */

static void test_empty_tree_every_op(void)
{
    values_freed = 0;
    rbtree_t *t = rb_create(counting_free);
    CHECK(t != NULL, "rb_create returned NULL");

    /* Three rounds. The tree is empty at the top of each one: fresh the
     * first time, emptied by delete after that -- and it must not be able
     * to tell the difference (root back to NULL, not left dangling). Round
     * 1 uses "" as the key: a legal C string of length zero, so the key
     * copy is exactly one byte, the terminator. */
    for (int round = 0; round < 3; round++) {
        struct collect c = { .n = 0 };

        CHECK(rb_size(t) == 0, "round %d: size %zu, expected 0", round, rb_size(t));
        CHECK(rb_validate(t) == 0, "round %d: empty tree failed validate", round);
        CHECK(rb_find(t, "x") == NULL, "round %d: find on empty not NULL", round);
        CHECK(rb_find(t, "") == NULL, "round %d: find \"\" on empty not NULL", round);
        rb_foreach(t, collect_cb, &c);
        CHECK(c.n == 0, "round %d: foreach on empty called fn %zu times", round, c.n);
        CHECK(rb_delete(t, "x") == -1, "round %d: delete on empty not -1", round);
        CHECK(rb_delete(t, "") == -1, "round %d: delete \"\" on empty not -1", round);
        CHECK(values_freed == round, "round %d: an empty-tree op freed a value (%d)",
              round, values_freed);

        const char *key = (round == 1) ? "" : "x";
        int *v = malloc(sizeof *v);
        CHECK(v != NULL, "malloc failed");
        *v = round;
        CHECK(rb_insert(t, key, v) == 0, "round %d: insert into empty failed", round);
        CHECK(rb_size(t) == 1, "round %d: size %zu after insert, expected 1",
              round, rb_size(t));
        CHECK(rb_find(t, key) == v, "round %d: find after insert wrong", round);
        CHECK(rb_validate(t) == 0, "round %d: validate failed after insert", round);

        CHECK(rb_delete(t, key) == 0, "round %d: delete of the only key failed", round);
        CHECK(values_freed == round + 1, "round %d: %d values freed, expected %d",
              round, values_freed, round + 1);
    }

    /* Destroying an empty tree frees nothing but the tree itself. */
    rb_destroy(t);
    CHECK(values_freed == 3, "destroy of an empty tree freed a value (%d total)",
          values_freed);
    expect_clean("test_empty_tree_every_op");
}

static void test_single_node_every_op(void)
{
    /* One node is the root, so it must be black with no parent (rules 2 and
     * the parent check); rb_validate proves both. Every operation gets
     * pointed at it: a hit, misses on BOTH sides (the search takes a
     * different branch each way), the walk, deletes that miss, and
     * overwrite-only-key -- the spec's third named edge case: replacing the
     * value of the only key frees the old value exactly once, size stays 1. */
    values_freed = 0;
    rbtree_t *t = rb_create(counting_free);
    CHECK(t != NULL, "rb_create returned NULL");

    int *v1 = malloc(sizeof *v1);
    int *v2 = malloc(sizeof *v2);
    CHECK(v1 != NULL && v2 != NULL, "malloc failed");
    *v1 = 1;
    *v2 = 2;

    CHECK(rb_insert(t, "m", v1) == 0, "insert m failed");
    CHECK(rb_size(t) == 1, "size %zu, expected 1", rb_size(t));
    CHECK(rb_validate(t) == 0, "single node failed validate (code %d)", rb_validate(t));

    /* Reads: the hit, then misses to each side of it. */
    CHECK(rb_find(t, "m") == v1, "find m wrong");
    CHECK(rb_find(t, "a") == NULL, "find a (left of m) not NULL");
    CHECK(rb_find(t, "z") == NULL, "find z (right of m) not NULL");
    CHECK(rb_find(t, "mm") == NULL, "find mm (longer, right of m) not NULL");
    CHECK(rb_find(t, "") == NULL, "find \"\" (left of m) not NULL");

    struct collect c = { .n = 0 };
    rb_foreach(t, collect_cb, &c);
    CHECK(c.n == 1, "foreach visited %zu keys, expected 1", c.n);
    CHECK(c.n == 1 && strcmp(c.keys[0], "m") == 0 && c.values[0] == v1,
          "foreach handed back the wrong key/value");

    /* Deletes that miss, both sides: -1, nothing freed, node untouched. */
    CHECK(rb_delete(t, "a") == -1, "delete a (absent, left) not -1");
    CHECK(rb_delete(t, "z") == -1, "delete z (absent, right) not -1");
    CHECK(values_freed == 0, "absent deletes freed %d values", values_freed);
    CHECK(rb_size(t) == 1 && rb_find(t, "m") == v1, "absent deletes damaged the node");

    /* Overwrite-only-key. */
    CHECK(rb_insert(t, "m", v2) == 0, "overwrite failed");
    CHECK(values_freed == 1, "overwrite freed %d values, expected 1", values_freed);
    CHECK(rb_size(t) == 1, "size %zu after overwrite, expected 1", rb_size(t));
    CHECK(rb_find(t, "m") == v2, "find after overwrite did not return the new value");
    CHECK(rb_validate(t) == 0, "validate failed after overwrite");

    /* Delete the only node: value freed once, tree back to empty and valid. */
    CHECK(rb_delete(t, "m") == 0, "delete m failed");
    CHECK(values_freed == 2, "delete freed %d values total, expected 2", values_freed);
    CHECK(rb_size(t) == 0, "size %zu after delete, expected 0", rb_size(t));
    CHECK(rb_find(t, "m") == NULL, "m still found after delete");
    CHECK(rb_validate(t) == 0, "empty tree failed validate after delete");
    c.n = 0;
    rb_foreach(t, collect_cb, &c);
    CHECK(c.n == 0, "foreach after delete called fn %zu times", c.n);

    rb_destroy(t);
    CHECK(values_freed == 2, "destroy of an empty tree freed a value (%d total)",
          values_freed);
    expect_clean("test_single_node_every_op");
}

/* Build a key on the heap: len copies of fill, then tail, then '\0'. Heap,
 * not stack: these are far too big for a stack array, and no VLAs. */
static char *make_long_key(size_t len, char fill, const char *tail)
{
    size_t tlen = strlen(tail);
    char *k = malloc(len + tlen + 1);
    if (k == NULL) {
        fprintf(stderr, "make_long_key: malloc failed\n");
        exit(1);
    }
    memset(k, fill, len);
    memcpy(k + len, tail, tlen + 1);        /* +1 copies the terminator */
    return k;
}

static void test_long_keys(void)
{
    /* Nothing in the tree may assume a key length: the copy is strlen+1
     * bytes, and ordering is by the WHOLE string. So: five keys of 64 KiB
     * that are identical except for their last character, one that is a
     * strict prefix of those (shorter sorts first, and must not overwrite
     * any of them), and a 1 MiB key on its own. Inserted out of order; the
     * walk must sort them by that final character, with lengths intact. */
    enum { PREFIX_LEN = 1 << 16, HUGE_LEN = 1 << 20, NKEYS = 7 };
    values_freed = 0;
    rbtree_t *t = rb_create(counting_free);
    CHECK(t != NULL, "rb_create returned NULL");

    /* sorted[] is in sorted order: bare prefix, then +'0'..+'4', then the
     * 1 MiB run of 'b' (greater at byte 0, so it sorts last). */
    char *sorted[NKEYS];
    sorted[0] = make_long_key(PREFIX_LEN, 'a', "");
    for (int i = 0; i < 5; i++) {
        char tail[2] = { (char)('0' + i), '\0' };
        sorted[1 + i] = make_long_key(PREFIX_LEN, 'a', tail);
    }
    sorted[6] = make_long_key(HUGE_LEN, 'b', "");

    /* Insert scrambled, each with its sorted index as the value. */
    const int order[NKEYS] = { 6, 4, 1, 0, 5, 2, 3 };
    int *vals[NKEYS];
    for (int j = 0; j < NKEYS; j++) {
        int i = order[j];
        vals[i] = malloc(sizeof *vals[i]);
        CHECK(vals[i] != NULL, "malloc failed");
        *vals[i] = i;
        CHECK(rb_insert(t, sorted[i], vals[i]) == 0, "insert of long key %d failed", i);
        CHECK(rb_size(t) == (size_t)(j + 1), "size %zu after %d inserts", rb_size(t), j + 1);
        CHECK(rb_validate(t) == 0, "validate code %d after long key %d", rb_validate(t), i);
    }
    CHECK(rb_size(t) == NKEYS, "size %zu, expected %d -- did the prefix key "
          "overwrite a longer one?", rb_size(t), NKEYS);

    /* Every key found by its full content, with its own value. */
    for (int i = 0; i < NKEYS; i++) {
        CHECK(rb_find(t, sorted[i]) == vals[i], "find of long key %d wrong", i);
    }
    /* One byte shorter than the bare prefix is a different string: a miss. */
    sorted[0][PREFIX_LEN - 1] = '\0';
    CHECK(rb_find(t, sorted[0]) == NULL, "a key one byte short of the prefix matched");
    sorted[0][PREFIX_LEN - 1] = 'a';

    /* The walk: sorted by the LAST character, and no key came back short. */
    struct collect c = { .n = 0 };
    rb_foreach(t, collect_cb, &c);
    CHECK(c.n == NKEYS, "foreach visited %zu keys, expected %d", c.n, NKEYS);
    for (size_t j = 0; j < c.n && j < NKEYS; j++) {
        CHECK(strlen(c.keys[j]) == strlen(sorted[j]),
              "foreach key %zu has length %zu, expected %zu -- truncated copy?",
              j, strlen(c.keys[j]), strlen(sorted[j]));
        CHECK(strcmp(c.keys[j], sorted[j]) == 0, "foreach position %zu out of order", j);
        CHECK(c.values[j] == vals[j], "foreach gave position %zu the wrong value", j);
    }

    /* Delete from the middle of the run, then both extremes. */
    CHECK(rb_delete(t, sorted[3]) == 0, "delete of a middle long key failed");
    CHECK(rb_delete(t, sorted[6]) == 0, "delete of the 1 MiB key failed");
    CHECK(rb_delete(t, sorted[0]) == 0, "delete of the bare prefix failed");
    CHECK(values_freed == 3, "%d values freed after 3 deletes", values_freed);
    CHECK(rb_size(t) == NKEYS - 3, "size %zu after deletes, expected %d",
          rb_size(t), NKEYS - 3);
    CHECK(rb_validate(t) == 0, "validate code %d after long-key deletes", rb_validate(t));
    CHECK(rb_find(t, sorted[3]) == NULL && rb_find(t, sorted[0]) == NULL,
          "a deleted long key is still found");
    CHECK(rb_find(t, sorted[2]) == vals[2] && rb_find(t, sorted[4]) == vals[4],
          "the neighbors of a deleted long key were lost");

    rb_destroy(t);
    CHECK(values_freed == NKEYS, "%d values freed by the end, expected %d",
          values_freed, NKEYS);
    expect_clean("test_long_keys");

    for (int i = 0; i < NKEYS; i++) {
        free(sorted[i]);
    }
}

/* ---- value_free timing --------------------------------------------------
 * value_free is caller code, and the tree must never run it while it is
 * itself half-changed. The M2 review caught three places that did:
 * rb_delete called it before the count and the fixup were done, rb_insert
 * called it while the node still pointed at the dying value, and rb_destroy
 * called it while root still pointed at nodes already freed. This
 * value_free looks back at the tree from INSIDE the callback and records
 * what it saw; the test then checks it always saw a finished operation.
 * ---------------------------------------------------------------------- */

static rbtree_t   *spy_tree;
static const char *spy_key;          /* the key whose value is being freed */
static int         spy_calls;        /* callbacks since the last reset */
static size_t      spy_size;         /* rb_size seen from inside (last call) */
static void       *spy_find;         /* rb_find(spy_key) seen from inside (last call) */
static int         spy_bad_validate; /* calls where rb_validate was nonzero */
static int         spy_found_key;    /* calls where rb_find(spy_key) was non-NULL */

static void spy_reset(const char *key)
{
    spy_key = key;
    spy_calls = 0;
    spy_size = 0;
    spy_find = NULL;
    spy_bad_validate = 0;
    spy_found_key = 0;
}

static void spying_free(void *p)
{
    spy_calls++;
    spy_size = rb_size(spy_tree);
    if (rb_validate(spy_tree) != 0) {
        spy_bad_validate++;
    }
    spy_find = rb_find(spy_tree, spy_key);
    if (spy_find != NULL) {
        spy_found_key++;
    }
    free(p);
}

static void test_value_free_sees_finished_tree(void)
{
    /* b(B)[a(B), d(R)[c(B), e(B)[-, f(R)]]] -- a shape from the delete table
     * where deleting a needs a fixup, after which d is the root with two
     * children (the hoist path). */
    spy_tree = rb_create(spying_free);
    CHECK(spy_tree != NULL, "rb_create returned NULL");
    const char *ins = "badcef";
    for (const char *p = ins; *p != '\0'; p++) {
        char key[2] = { *p, '\0' };
        int *v = malloc(sizeof *v);
        CHECK(v != NULL, "malloc failed");
        *v = *p;
        CHECK(rb_insert(spy_tree, key, v) == 0, "insert %s failed", key);
    }

    /* Delete a black leaf (fixup runs), then a two-children node (hoist).
     * Seen from inside value_free, each delete must already be complete. */
    spy_reset("a");
    CHECK(rb_delete(spy_tree, "a") == 0, "delete a failed");
    CHECK(spy_calls == 1, "delete a ran value_free %d times", spy_calls);
    CHECK(spy_size == 5, "inside delete a: rb_size %zu, expected 5 (count fixed late?)",
          spy_size);
    CHECK(spy_bad_validate == 0, "inside delete a: rb_validate failed (fixup ran late?)");
    CHECK(spy_found_key == 0, "inside delete a: the victim was still findable");

    spy_reset("d");
    CHECK(rb_delete(spy_tree, "d") == 0, "delete d failed");
    CHECK(spy_calls == 1, "delete d ran value_free %d times", spy_calls);
    CHECK(spy_size == 4 && spy_bad_validate == 0 && spy_found_key == 0,
          "inside delete d: size %zu, validate failures %d, victim findable %d",
          spy_size, spy_bad_validate, spy_found_key);

    /* Overwrite. Seen from inside value_free, the node must already hold
     * the NEW value -- never the one being freed. */
    int *nv = malloc(sizeof *nv);
    CHECK(nv != NULL, "malloc failed");
    *nv = 99;
    spy_reset("c");
    CHECK(rb_insert(spy_tree, "c", nv) == 0, "overwrite c failed");
    CHECK(spy_calls == 1, "overwrite ran value_free %d times", spy_calls);
    CHECK(spy_find == nv, "inside overwrite: rb_find returned the value being freed");
    CHECK(spy_size == 4 && spy_bad_validate == 0,
          "inside overwrite: size %zu, validate failures %d", spy_size, spy_bad_validate);

    /* Destroy. Seen from inside value_free, the tree must look empty and
     * valid every time -- the old code walked already-freed nodes here. */
    spy_reset("c");
    rb_destroy(spy_tree);
    CHECK(spy_calls == 4, "destroy ran value_free %d times, expected 4", spy_calls);
    CHECK(spy_size == 0 && spy_bad_validate == 0 && spy_found_key == 0,
          "inside destroy: size %zu, validate failures %d, key findable %d times",
          spy_size, spy_bad_validate, spy_found_key);
    expect_clean("test_value_free_sees_finished_tree");
}

/* ---- main ------------------------------------------------------------- */

int main(void)
{
    test_empty_tree();
    test_insert_and_find();
    test_overwrite_frees_old_value();
    test_overwrite_same_pointer_is_noop();
    test_destroy_frees_every_value();
    test_keys_are_copied();

    test_stale_root_left_rotation();
    test_stale_root_right_rotation();
    test_zigzag_left_right();
    test_zigzag_right_left();
    test_red_uncle_recolor();
    test_ascending_stress();
    test_random_stress();

    test_foreach_in_order();
    test_foreach_empty_and_after_delete();

    test_delete_cases();
    test_delete_only_node_then_reuse();
    test_delete_absent();

    test_empty_tree_every_op();
    test_single_node_every_op();
    test_long_keys();
    test_value_free_sees_finished_tree();

    if (failures != 0) {
        fprintf(stderr, "test_rbtree: %d check(s) FAILED\n", failures);
        return 1;
    }
    printf("test_rbtree: all tests passed, 0 blocks leaked\n");
    return 0;
}
