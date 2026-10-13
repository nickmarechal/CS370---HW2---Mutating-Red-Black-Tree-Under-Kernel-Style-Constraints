# rbtree-lab (HW2): project rules

Carried forward from HW1 and amended. The HW1 tree is the starting point; HW2
mutates it three times (fault sweep, slab pool, O(1)-space teardown).

## Commands
- Build & unit tests: `make test`
- Sanitizers: `make asan`   Valgrind: `make memcheck` (Linux only, see Environment)
- A change is DONE only when all three pass. Always run them; show output.
- `make test` runs each binary twice: plain trees, then `--pooled`.
- Fuzzer by hand: `./build/fuzz <ops> [seed] [--pooled]`. A failure prints
  its seed and a reproduce command; rerun with that, then shrink the op count.
- Unit tests by hand: `./build/test_rbtree [--pooled] [--nodes N]` (N = size of
  the 64 KiB-stack test, default 1000000; memcheck uses 20000).

## Hard constraints
- NEVER modify include/rbtree.h. It is the graded contract (HW2 version,
  Appendix A: adds `rb_create_pooled` and `rb_snapshot`).
- The whole HW1 suite (unit tests, delete table, fuzzer) is a regression
  suite. Every milestone must leave all of it green.
- NEVER weaken, skip, or delete a test to make the suite pass. If a test
  looks wrong, stop and explain why instead.
- All heap allocation in src/ goes through `rb_malloc`/`rb_free`, declared in
  tests/fault_alloc.h. Direct malloc/free in src/ is a defect.
- ANY allocation may fail. Every failure path must unwind completely: no
  leaks, tree left exactly as before the call, caller's value not consumed,
  documented error code. Prefer moving allocations before the first tree
  change (the commit point) over writing cleanup code.
- `rb_delete` and the overwrite path of `rb_insert` must never allocate.
- Teardown/traversal (M3): no recursion, no heap-allocated stack, O(1) space.

## Style
- C23. -Wall -Wextra -Werror must stay clean. No VLAs.
- No goto except cleanup-label unwinding. Every allocation checked.
- Prefer the smallest diff that passes. Do not refactor unrelated code.
- Every non-obvious loop gets a one-line invariant comment.

## Workflow
- For any multi-file or algorithmic change: propose a plan and wait for
  approval before editing.
- Tests first, then the smallest slice. One slice per prompt.
- Commit only from a green state; message format "M<n>: <what>".
- Nick runs git commit/push himself unless he says otherwise for that case.

## Code map
- `src/rbtree.c` — the tree. No <stdlib.h>, so a direct malloc/free fails to
  compile. Node has a `parent` pointer; missing children are NULL.
  `struct rbtree` has `pool` (NULL = plain tree). `node_alloc`/`node_release`
  are the only place a node's two allocations happen; `node_storage_free`
  sends the node back to the pool or `rb_free`. `rb_create_pooled` = tree
  struct + pool, unwinds the struct if the pool fails. `rotate_left/right`
  own the `t->root` update. `insert_fixup`, `delete_fixup`, `transplant`
  allocate nothing. M3: `rb_foreach` walks with `successor()` over parent
  pointers (one node pointer of state, tree never modified); `rb_destroy`
  rotates the tree into a right spine (one pointer of state; measure = total
  nodes + nodes off the spine). Neither recurses or allocates.
  `validate_subtree` is still recursive (allowed; depth <= 2 log2(n+1)).
- `src/pool.{c,h}` — slab pool (M2). 4096-byte slabs from `rb_malloc`,
  in-band `struct slab` header, stride rounded to `alignof(max_align_t)`
  (8 on the Mac, 16 on Linux CI: compute geometry, never hardcode 85).
  `pool_alloc`: free list, else bump, else new slab. `pool_free` poisons
  the slot with 0xDD, then writes the link over the front. `pool_check`
  walks the free list for tests. `pool.h` is private, not the frozen header.
- `tests/fault_alloc.{c,h}` — `rb_malloc`/`rb_free` seam + fault injector
  (`arm(n)` fails the n-th call once; `total` counts every call). Live-block
  counter is `static`; read it via `rb_debug_live_allocs()`.
- `tests/sweep.{c,h}` — fault sweep: 200 insert / 100 delete / 50 overwrite,
  n = 1.. until a clean run; five `assert_unchanged` checks; leaks measured
  per run. Takes the create function, so it runs plain and pooled.
- `tests/test_pool.{c,h}` — pool unit tests through the pool API only.
- `tests/test_rbtree.c` — HW1 unit tests + delete table; `create_tree`
  pointer is `rb_create` or `rb_create_pooled` (`--pooled`); main also
  calls the pool tests, injector self-test and sweep.
- `tests/test_bigstack.{c,h}` — M3: n-node tree (default 10^6), then
  rb_foreach + rb_destroy inside a pthread with a 64 KiB stack.
- `tests/fuzz.c` — random ops vs. a flag-array model, xorshift PRNG, seeded.
- CI: `.github/workflows/battery.yml` (test, memcheck, asan on Ubuntu,
  gcc-14) ran on every push through 3749549, all green. Removed afterwards
  because Canvas rejects `.yml` files in the submission; it is still in git
  history (added in 859dac0) if it needs to come back.

## Ownership
Three separately-owned things per node: the node, the tree's private key
copy, and the caller's `void *value`.
- rb_insert returns 0 -> tree owns value. Returns -1 -> caller still owns it.
- Overwrite frees the OLD value via value_free first -- unless the caller
  passed back the very pointer the tree already holds; then nothing is freed.
- rb_delete frees the key copy and (if owned) the value, via node_release.
  The two-children hoist SWAPS key/value pointers, never copies.
- Rotations change topology only, never ownership. Delete never allocates.
- Pooled trees (M2): nodes go back to the pool; key copies still go to
  `rb_free`.

## Environment
`make memcheck` cannot run on this Mac (no valgrind on Apple Silicon), and
macOS ASan has no LeakSanitizer. Valgrind and LSan ran in GitHub Actions
(workflow now removed for Canvas, see Code map); "green under memcheck" means
those runs. Never report
memcheck as passing locally. gcc-14 there is stricter than the Mac's clang
(e.g. -Wformat-truncation under UBSan), so a local green is not the last word.

## Status
M0, M1, M2, M3 code done and green on Actions (test, valgrind memcheck,
ASan+UBSan+LSan), plain and pooled. Last code commit fdd3370 (review fixes:
foreach/destroy proven to allocate nothing; stale comment removed).
Reach (M4, COW snapshots) not attempted; rb_snapshot left undefined.
Open: Nick's wording for the DRAFT invariant comment in rb_destroy; DEVLOG
is still the empty template; spec wants a fresh-context review session.
