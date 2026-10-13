#ifndef TEST_BIGSTACK_H
#define TEST_BIGSTACK_H

#include "rbtree.h"

/* M3: build an n-node tree, then run rb_foreach and rb_destroy on it inside
 * a thread whose stack is only 64 KiB. Returns the number of failed checks. */
int big_stack_test(rbtree_t *(*create)(rb_value_free_fn value_free),
                   const char *label, long n);

#endif
