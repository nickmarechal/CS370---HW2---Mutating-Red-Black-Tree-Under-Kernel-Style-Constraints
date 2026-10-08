# reflection

TODO (1-2 pages, my own words, filled in at the end). The spec asks for:

- slab vs. the kernel's kmem_cache: what the kernel adds, and why it matters at kernel scale
- copy-on-write comparison to fork() (only if I attempt the Reach)
- my `pool_stats` definitions
- my slab-header and alignment decisions
- my 0xDD poisoning rationale (what bug class it catches that ASan no longer can)
- where the agent was most and least reliable
- one bug it introduced that I caught, and how
- if the million-node test ran smaller under valgrind, say so here
