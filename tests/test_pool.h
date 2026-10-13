#ifndef TEST_POOL_H
#define TEST_POOL_H

/* Unit tests for src/pool.c through its own API, before the tree uses it.
 * Returns the number of failed checks (0 = green). */
int pool_unit_tests(void);

#endif
