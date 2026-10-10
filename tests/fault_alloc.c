#include "fault_alloc.h"

#include <stdlib.h>

/* Blocks handed out by rb_malloc that rb_free has not taken back yet.
 * static: no other file can see this variable. They read it through
 * rb_debug_live_allocs() instead, so it is defined exactly once per binary. */
static size_t rb_live_allocs = 0;

void *rb_malloc(size_t n)
{
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

/* Not in fault_alloc.h on purpose: the HW1 tests declare it themselves. */
size_t rb_debug_live_allocs(void)
{
    return rb_live_allocs;
}
