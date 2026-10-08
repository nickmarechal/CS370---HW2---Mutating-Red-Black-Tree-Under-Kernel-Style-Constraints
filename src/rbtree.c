#include "rbtree.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------
 * Allocation seam.
 *
 * Every allocation in this file goes through these two wrappers, never
 * through malloc/free directly. HW2 replaces the bodies with a
 * fault-injecting allocator; keeping the seam here means that change never
 * has to touch the tree code itself.
 * --------------------------------------------------------------------- */

/* Blocks handed out by rb_malloc that rb_free has not taken back yet.
 * Tests check this is 0 once the tree is destroyed; anything else is a leak,
 * and the number is how many blocks leaked. */
static size_t rb_live_allocs = 0;

static void *rb_malloc(size_t n)
{
    void *p = malloc(n);
    if (p != NULL) { // if memory is NULL, there is no memory or an issue with malloc, still want to allocate it.
        rb_live_allocs++;
    }
    return p;
}

static void rb_free(void *p)
{
    if (p != NULL) {
        rb_live_allocs--; // free(NULL) is legal and frees nothing, count would drop below the truth.
    }
    free(p);
}

/* Deliberately not in rbtree.h -- that header is frozen. Test files declare
 * this themselves and the linker connects them. */
size_t rb_debug_live_allocs(void)
{
    return rb_live_allocs;
}

/* A node is red or black. There is no third color -- deletion's "doubly
 * black" is a way of counting, never a value stored in a node. */
typedef enum { RB_RED, RB_BLACK } rb_color;

struct rbnode {
    char          *key;      /* our own copy of the caller's string; we free it */
    void          *value;    /* the caller's pointer, stored as-is (not copied) */
    rb_color       color;
    struct rbnode *left;     /* NULL when there is no child */
    struct rbnode *right;
    struct rbnode *parent;   /* NULL only for the root */
};

struct rbtree {
    struct rbnode   *root;       /* NULL when the tree is empty */
    rb_value_free_fn value_free; /* NULL means the values are not ours to free */
    size_t           count;      /* keys held, so rb_size answers instantly */
};

/* ------------------------------------------------------------------------
 * Node lifetime. One node is TWO allocations: the struct, and a private copy
 * of the key. Both are made here and both are freed in node_release, so the
 * ownership rules live in exactly one place.
 * --------------------------------------------------------------------- */

/* Returns NULL if either allocation fails. On failure nothing is left
 * allocated -- the caller never sees a half-built node. */
static struct rbnode *node_alloc(const char *key, void *value)
{
    struct rbnode *n = rb_malloc(sizeof *n);
    if (n == NULL) {
        return NULL;
    }

    size_t len = strlen(key) + 1;           /* +1 for the '\0' terminator */
    n->key = rb_malloc(len);
    if (n->key == NULL) {
        goto fail;                          /* unwind the first allocation */
    }
    memcpy(n->key, key, len);

    n->value  = value;
    n->color  = RB_RED;                     /* a new node is always red */
    n->left   = NULL;
    n->right  = NULL;
    n->parent = NULL;
    return n;

fail:
    rb_free(n);
    return NULL;
}

/* Free one node: the value (only if the tree owns values), the key copy, and
 * the struct. Does not touch left/right/parent -- unlinking is the caller's
 * job, and this must never be called on a node still reachable from the tree. */
static void node_release(const rbtree_t *t, struct rbnode *n)
{
    if (t->value_free != NULL) {
        t->value_free(n->value);
    }
    rb_free(n->key);
    rb_free(n);
}

/* rb_validate failure codes. 0 means every rule holds. Nonzero names the rule
 * that broke, which is far more useful in a failing test than a bare 1. */
enum {
    RB_OK              = 0,
    RB_BAD_ORDER       = 1,     /* in-order keys are not strictly increasing */
    RB_BAD_PARENT      = 2,     /* a child's parent pointer does not point back */
    RB_BAD_COUNT       = 3,     /* t->count disagrees with the real node count */
    RB_BAD_ROOT_PARENT = 4,     /* the root has a non-NULL parent */
    RB_BAD_ROOT_COLOR  = 5,     /* rule 2: the root is not black */
    RB_BAD_RED_RED     = 6,     /* rule 4: a red node has a red child */
    RB_BAD_BLACK_HEIGHT= 7,     /* rule 5: left and right black-heights differ */
};

/* ------------------------------------------------------------------------
 * Balancing: rotations and the insert fixup.
 * --------------------------------------------------------------------- */

/* A missing child counts as black (rule 3), so NULL is never red. Every
 * color test below goes through here so that rule lives in exactly one place. */
static bool is_red(const struct rbnode *n)
{
    return n != NULL && n->color == RB_RED;
}

/* Left rotation at x. Precondition: x->right is a real node, never NULL.
 *
 *        x                  y
 *       / \                / \
 *      a   y      ==>      x   c
 *         / \            / \
 *        b   c          a   b
 *
 * Shape only: no key, value, or color changes, and nothing is allocated or
 * freed. The three-way reattach in the middle is what keeps t->root correct
 * when x was the root -- the caller never has to think about it. */
static void rotate_left(rbtree_t *t, struct rbnode *x)
{
    struct rbnode *y = x->right;

    x->right = y->left;                     /* b moves across to x */
    if (y->left != NULL) {
        y->left->parent = x;
    }

    y->parent = x->parent;                  /* y takes x's old place... */
    if (x->parent == NULL) {
        t->root = y;                        /* ...which was the root */
    } else if (x == x->parent->left) {
        x->parent->left = y;
    } else {
        x->parent->right = y;
    }

    y->left = x;                            /* x tucks under y */
    x->parent = y;
}

/* Right rotation at x: the mirror image. Precondition: x->left is not NULL. */
static void rotate_right(rbtree_t *t, struct rbnode *x)
{
    struct rbnode *y = x->left;

    x->left = y->right;
    if (y->right != NULL) {
        y->right->parent = x;
    }

    y->parent = x->parent;
    if (x->parent == NULL) {
        t->root = y;
    } else if (x == x->parent->right) {
        x->parent->right = y;
    } else {
        x->parent->left = y;
    }

    y->right = x;
    x->parent = y;
}

/* Restore the red-black rules after inserting z, which is red.
 *
 * Loop invariant: z is red; the root is black unless z is the root; and the
 * only rule that can be broken is rule 4 at exactly one spot -- z and its
 * parent both red. Rule 5 (equal black-heights on every path) holds the
 * whole time, because a red node adds nothing to any path's black count.
 *
 * Termination: the red-uncle case moves z up two levels without rotating, so
 * it can repeat at most height/2 times. The black-uncle cases rotate and
 * leave z's parent black, so the loop exits on the very next check. At most
 * two rotations in total. */
static void insert_fixup(rbtree_t *t, struct rbnode *z)
{
    while (is_red(z->parent)) {
        struct rbnode *p = z->parent;
        struct rbnode *g = p->parent;       /* p is red, so p is not the root */

        if (p == g->left) {
            struct rbnode *u = g->right;    /* uncle; NULL counts as black */

            if (is_red(u)) {
                /* Case 1: red uncle. Recolor and climb. No pointers move. */
                p->color = RB_BLACK;
                u->color = RB_BLACK;
                g->color = RB_RED;
                z = g;
            } else {
                if (z == p->right) {
                    /* Case 2: zig-zag. Straighten it, then fall into case 3.
                     * The rotation changes who is above whom, so re-read
                     * p and g afterward -- the old ones are stale. */
                    z = p;
                    rotate_left(t, z);
                    p = z->parent;
                    g = p->parent;
                }
                /* Case 3: straight line. One rotation at g fixes it, and
                 * p is now black, so the loop ends. */
                p->color = RB_BLACK;
                g->color = RB_RED;
                rotate_right(t, g);
            }
        } else {
            /* Mirror of the block above: every left and right swapped. */
            struct rbnode *u = g->left;

            if (is_red(u)) {
                p->color = RB_BLACK;
                u->color = RB_BLACK;
                g->color = RB_RED;
                z = g;
            } else {
                if (z == p->left) {
                    z = p;
                    rotate_right(t, z);
                    p = z->parent;
                    g = p->parent;
                }
                p->color = RB_BLACK;
                g->color = RB_RED;
                rotate_left(t, g);
            }
        }
    }

    t->root->color = RB_BLACK;              /* rule 2, unconditionally */
}

rbtree_t *rb_create(rb_value_free_fn value_free)
{
    rbtree_t *t = rb_malloc(sizeof *t);
    if (t == NULL) {
        return NULL;
    }
    t->root = NULL;
    t->value_free = value_free;
    t->count = 0;
    return t;
}

int rb_insert(rbtree_t *t, const char *key, void *value)
{
    struct rbnode *parent = NULL;
    struct rbnode *cur = t->root;
    int cmp = 0;

    /* Invariant: key is not in any node above cur, and if it is in the tree
     * at all it is somewhere in the subtree rooted at cur. */
    while (cur != NULL) {
        cmp = strcmp(key, cur->key);
        if (cmp == 0) {
            /* Key already present: replace the value. Store the new pointer
             * FIRST, then free the old one: value_free is caller code, and
             * while it runs the node must not point at memory being freed
             * (a callback that called rb_find would get a dangling pointer).
             * The tree is the old value's only owner, so freeing it is right
             * (if we own values) -- unless the caller handed back the very
             * pointer we already hold; then there is nothing old to free,
             * and freeing it would leave cur->value dangling for rb_destroy
             * to free again. */
            void *old = cur->value;
            cur->value = value;
            if (t->value_free != NULL && old != value) {
                t->value_free(old);
            }
            return 0;                       /* count unchanged */
        }
        parent = cur;
        cur = (cmp < 0) ? cur->left : cur->right;
    }

    /* Fell off the tree: cur is NULL and parent is the node to hang from.
     * cmp still holds the comparison against parent, so it tells us which side. */
    struct rbnode *n = node_alloc(key, value);
    if (n == NULL) {
        return -1;                          /* tree unchanged; caller keeps value */
    }
    n->parent = parent;

    if (parent == NULL) {
        t->root = n;                        /* first node in an empty tree */
    } else if (cmp < 0) {
        parent->left = n;
    } else {
        parent->right = n;
    }
    t->count++;

    insert_fixup(t, n);
    return 0;
}

void *rb_find(const rbtree_t *t, const char *key)
{
    const struct rbnode *cur = t->root;

    /* Invariant: if key is in the tree, it is in the subtree rooted at cur. */
    while (cur != NULL) {
        int cmp = strcmp(key, cur->key);
        if (cmp == 0) {
            return cur->value;
        }
        cur = (cmp < 0) ? cur->left : cur->right;
    }
    return NULL;
}

/* ------------------------------------------------------------------------
 * Deletion. Kept apart from the insert machinery so a failing delete test
 * points at exactly these functions. Nothing here allocates: the header
 * gives rb_delete no way to report an allocation failure, so it must never
 * need one. No recursion either: delete and its fixup are loops.
 * --------------------------------------------------------------------- */

/* Leftmost node under n: the smallest key in that subtree. Precondition: n
 * is not NULL. Used to find the in-order successor of a node with two
 * children (the minimum of its right subtree). */
static struct rbnode *tree_minimum(struct rbnode *n)
{
    /* Invariant: the smallest key of the original subtree is at or below n. */
    while (n->left != NULL) {
        n = n->left;
    }
    return n;
}

/* Make u's parent point at v instead of u. v may be NULL. Updates t->root
 * when u was the root. Does not touch u's own pointers, and cannot set
 * v->parent when v is NULL -- which is why delete_fixup is handed the
 * parent separately. */
static void transplant(rbtree_t *t, struct rbnode *u, struct rbnode *v)
{
    if (u->parent == NULL) {
        t->root = v;
    } else if (u == u->parent->left) {
        u->parent->left = v;
    } else {
        u->parent->right = v;
    }
    if (v != NULL) {
        v->parent = u->parent;
    }
}

/* Pay off the "doubly black" debt left behind by unlinking a black node.
 *
 * x is whatever took the unlinked node's place -- possibly NULL, since a
 * missing child counts as black -- and x_parent is its parent. Every path
 * through x is now one black short of every other path. The loop climbs
 * toward the root until the debt is paid, in one of three ways: a red node
 * absorbs it by turning black, a rotation drags a red node across from the
 * sibling's side, or the debt reaches the root and simply dissolves (every
 * path lost the same one black, so they are all still equal).
 *
 * Loop invariant: x is doubly black; every other rule holds. The sibling w
 * is never NULL: before the delete both of x_parent's sides had the same
 * black-height, and a black node came off x's side, so the sibling's side
 * still has black-height >= 2 -- which takes at least one real node. That
 * is also why, when x is NULL, "x == x_parent->left" still picks the right
 * side: only the emptied side can be NULL.
 *
 * Termination: case 2 climbs one level per pass, so at most height passes.
 * Case 1 always drops into 2, 3 or 4 with a black sibling; if it is 2, the
 * new x is the parent we just painted red, so the next test exits. Cases
 * 3->4 and 4 end the loop directly. At most three rotations in total. */
static void delete_fixup(rbtree_t *t, struct rbnode *x, struct rbnode *x_parent)
{
    while (x != t->root && !is_red(x)) {
        if (x == x_parent->left) {
            struct rbnode *w = x_parent->right;     /* sibling; never NULL */

            if (is_red(w)) {
                /* Case 1: red sibling. Not a shape we can fix directly, so
                 * rotate to give x a black sibling, then fall through. */
                w->color = RB_BLACK;
                x_parent->color = RB_RED;
                rotate_left(t, x_parent);
                w = x_parent->right;                /* new sibling, black */
            }

            if (!is_red(w->left) && !is_red(w->right)) {
                /* Case 2: black sibling, no red child. Take one black off
                 * the sibling's side as well (paint w red): now both sides
                 * are short by one, so the debt moves up to the parent. */
                w->color = RB_RED;
                x = x_parent;
                x_parent = x->parent;
            } else {
                if (!is_red(w->right)) {
                    /* Case 3: the red child is on the near side. Rotate at
                     * w to move it to the far side, then fall into case 4. */
                    w->left->color = RB_BLACK;
                    w->color = RB_RED;
                    rotate_right(t, w);
                    w = x_parent->right;
                }
                /* Case 4: red child on the far side. Rotate at the parent
                 * so w moves up and the red child comes across to x's side;
                 * the recoloring restores every path's black count. Paid. */
                w->color = x_parent->color;
                x_parent->color = RB_BLACK;
                w->right->color = RB_BLACK;
                rotate_left(t, x_parent);
                x = t->root;                        /* done: ends the loop */
                x_parent = NULL;
            }
        } else {
            /* Mirror of the block above: every left and right swapped. */
            struct rbnode *w = x_parent->left;

            if (is_red(w)) {
                w->color = RB_BLACK;
                x_parent->color = RB_RED;
                rotate_right(t, x_parent);
                w = x_parent->left;
            }

            if (!is_red(w->right) && !is_red(w->left)) {
                w->color = RB_RED;
                x = x_parent;
                x_parent = x->parent;
            } else {
                if (!is_red(w->left)) {
                    w->right->color = RB_BLACK;
                    w->color = RB_RED;
                    rotate_left(t, w);
                    w = x_parent->left;
                }
                w->color = x_parent->color;
                x_parent->color = RB_BLACK;
                w->left->color = RB_BLACK;
                rotate_right(t, x_parent);
                x = t->root;
                x_parent = NULL;
            }
        }
    }

    if (x != NULL) {
        x->color = RB_BLACK;    /* a red x absorbs the debt; the root stays black */
    }
}

int rb_delete(rbtree_t *t, const char *key)
{
    struct rbnode *z = t->root;

    /* Invariant: if key is in the tree, it is in the subtree rooted at z. */
    while (z != NULL) {
        int cmp = strcmp(key, z->key);
        if (cmp == 0) {
            break;
        }
        z = (cmp < 0) ? z->left : z->right;
    }
    if (z == NULL) {
        return -1;                          /* absent: nothing touched */
    }

    if (z->left != NULL && z->right != NULL) {
        /* Two children: hoist. The in-order successor y is the smallest key
         * to z's right, so it has no left child. Swap the payloads: z's slot
         * now holds y's key/value (still in order), and y's node holds the
         * doomed key/value. Then delete y's node instead. Colors stay put --
         * the tree's shape and coloring are untouched, only the payload
         * moved -- and each key and value still has exactly one owner. */
        struct rbnode *y = tree_minimum(z->right);
        char *k = z->key;
        void *v = z->value;
        z->key = y->key;
        z->value = y->value;
        y->key = k;
        y->value = v;
        z = y;
    }

    /* z has at most one child. Splice z out: x takes its place. */
    struct rbnode *x = (z->left != NULL) ? z->left : z->right;  /* may be NULL */
    struct rbnode *x_parent = z->parent;

    transplant(t, z, x);                    /* unlinked: nothing points at z now */
    t->count--;

    if (z->color == RB_BLACK) {
        /* Unlinking a black node left every path through x one black short.
         * Unlinking a red one cost nothing. z is unlinked but not yet freed,
         * and x and x_parent are never z, so the fixup never sees it. */
        delete_fixup(t, x, x_parent);
    }

    /* Free z LAST. node_release runs value_free, which is caller code: if it
     * looks at the tree (rb_size, rb_find, rb_validate), it must see a
     * finished delete -- count right, every rule holding -- not one caught
     * halfway. Nothing above reads z after this line. */
    node_release(t, z);
    return 0;
}

size_t rb_size(const rbtree_t *t)
{
    return (t == NULL) ? 0 : t->count;
}

/* In-order walk of one subtree: everything smaller, then this node, then
 * everything larger. Recursion is allowed here for this assignment (HW2
 * takes it away). The callback must not insert into or delete from the
 * tree it is walking -- the walk holds pointers into it. */
static void foreach_subtree(const struct rbnode *n,
                            void (*fn)(const char *key, void *value, void *ctx),
                            void *ctx)
{
    if (n == NULL) {
        return;
    }
    foreach_subtree(n->left, fn, ctx);
    fn(n->key, n->value, ctx);
    foreach_subtree(n->right, fn, ctx);
}

void rb_foreach(const rbtree_t *t,
                void (*fn)(const char *key, void *value, void *ctx),
                void *ctx)
{
    foreach_subtree(t->root, fn, ctx);
}

/* In-order walk of one subtree, checking every rule at once.
 *
 * The error travels in the RETURN VALUE; the data travels in the pointer
 * parameters. *prev is the last key seen (NULL before the first); every key
 * must be strictly greater. *count accumulates the real node count.
 * *black_height receives this subtree's black-height, but only on success.
 *
 * Every recursive call is followed by an early return if it failed, so the
 * first broken rule found -- however deep -- comes straight back up untouched. */
static int validate_subtree(const struct rbnode *n, const char **prev,
                            size_t *count, size_t *black_height)
{
    if (n == NULL) {
        *black_height = 1;                  /* the invisible black leaf */
        return RB_OK;
    }

    /* Checks on this node first, before descending. */
    if (n->left != NULL && n->left->parent != n) {
        return RB_BAD_PARENT;
    }
    if (n->right != NULL && n->right->parent != n) {
        return RB_BAD_PARENT;
    }
    if (n->color == RB_RED && (is_red(n->left) || is_red(n->right))) {
        return RB_BAD_RED_RED;
    }

    size_t left_bh = 0;
    int rc = validate_subtree(n->left, prev, count, &left_bh);
    if (rc != RB_OK) {
        return rc;                          /* early abort */
    }

    if (*prev != NULL && strcmp(*prev, n->key) >= 0) {
        return RB_BAD_ORDER;
    }
    *prev = n->key;
    (*count)++;

    size_t right_bh = 0;
    rc = validate_subtree(n->right, prev, count, &right_bh);
    if (rc != RB_OK) {
        return rc;                          /* early abort */
    }

    /* Only compare once BOTH sides came back clean. */
    if (left_bh != right_bh) {
        return RB_BAD_BLACK_HEIGHT;
    }

    *black_height = left_bh + (n->color == RB_BLACK ? 1 : 0);
    return RB_OK;
}

int rb_validate(const rbtree_t *t)
{
    if (t->root != NULL) {
        if (t->root->parent != NULL) {
            return RB_BAD_ROOT_PARENT;
        }
        if (t->root->color != RB_BLACK) {
            return RB_BAD_ROOT_COLOR;
        }
    }

    const char *prev = NULL;
    size_t count = 0;
    size_t black_height = 0;                /* received, not compared: every
                                               path below already agreed */
    int rc = validate_subtree(t->root, &prev, &count, &black_height);
    if (rc != RB_OK) {
        return rc;
    }
    if (count != t->count) {
        return RB_BAD_COUNT;
    }
    return RB_OK;
}

/* Free a whole subtree, children before parent, so no freed node is ever
 * read again. Recursion is allowed in this assignment (HW2 takes it away). */
static void destroy_subtree(const rbtree_t *t, struct rbnode *n)
{
    if (n == NULL) {
        return;
    }
    destroy_subtree(t, n->left);
    destroy_subtree(t, n->right);
    node_release(t, n);
}

void rb_destroy(rbtree_t *t)
{
    if (t == NULL) {
        return;                 /* the header promises NULL-safety */
    }

    /* Detach first, free second. Once the walk starts freeing nodes the old
     * subtree is unsafe to read, and value_free is caller code that might
     * read it (rb_find, rb_validate...). With root and count cleared, any
     * such call sees a valid empty tree instead of freed memory. */
    struct rbnode *root = t->root;
    t->root = NULL;
    t->count = 0;
    destroy_subtree(t, root);
    rb_free(t);
}
