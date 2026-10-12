#include "fault_alloc.h"

#include <stdlib.h>

/* Blocks handed out by rb_malloc that rb_free has not taken back yet.
 * static: no other file can see this variable. They read it through
 * rb_debug_live_allocs() instead, so it is defined exactly once per binary. */
static size_t rb_live_allocs = 0;

/* Fault injector state, also private to this file.
 * fault_countdown: 0 means disarmed. k > 0 means the k-th rb_malloc from
 *   now returns NULL. It counts down to 0 as it fires, so it fires once.
 * fault_total: every rb_malloc call ever made, the failed ones included. */
static long fault_countdown = 0;
static long fault_total = 0;

void *rb_malloc(size_t n)
{
    fault_total++;
    if (fault_countdown > 0) {
        fault_countdown--;
        if (fault_countdown == 0) {
            return NULL;        /* injected failure: no malloc, nothing to count */
        }
    }

    void *p = malloc(n);
    if (p != NULL) {
        rb_live_allocs++;       /* count only blocks we actually got */
    }
    return p;
}

void rb_free(void *p)
{
    if (p != NULL) {
        rb_live_allocs--;       /* free(NULL) frees nothing, so it must not count */
    }
    free(p);
}

/* arm(1) fails the very next allocation. n <= 0 just disarms. */
void fault_alloc_arm(long n)
{
    fault_countdown = (n > 0) ? n : 0;
}

void fault_alloc_disarm(void)
{
    fault_countdown = 0;
}

long fault_alloc_total(void)
{
    return fault_total;
}

/* Not in fault_alloc.h on purpose: the HW1 tests declare it themselves. */
size_t rb_debug_live_allocs(void)
{
    return rb_live_allocs;
}
