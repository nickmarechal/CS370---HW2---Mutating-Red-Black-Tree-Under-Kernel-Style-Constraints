#ifndef SWEEP_H
#define SWEEP_H

#include "rbtree.h"

/* Both return the number of checks that failed (0 = green). */

/* Checks the injector itself: arm, disarm, total, live count. */
int fault_injector_selftest(void);

/* Fault sweep (spec 8.1): for n = 1, 2, 3, ... run the fixed scenario with
 * the n-th allocation failing, until a run finishes without hitting it.
 * create is rb_create for the plain build; M2 passes rb_create_pooled. */
int fault_sweep(rbtree_t *(*create)(rb_value_free_fn value_free),
                const char *label);

#endif
