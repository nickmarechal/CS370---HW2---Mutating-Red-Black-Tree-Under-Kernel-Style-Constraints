# rbtree-lab (HW2): project rules

Carried forward from HW1 and amended. The HW1 tree is the starting point; HW2
mutates it three times (fault sweep, slab pool, O(1)-space teardown).

## Commands
- Build & unit tests: `make test`
- Sanitizers: `make asan`   Valgrind: `make memcheck` (Linux only, see Environment)
- A change is DONE only when all three pass. Always run them; show output.
- Fuzzer by hand: `./build/fuzz <ops> [seed]`. A failure prints its seed and
  a reproduce command; rerun with that, then shrink the op count.

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
- `src/rbtree.c` — the whole tree (HW1 code, imported unchanged in M0).
  Node struct has a `parent` pointer. Missing children are NULL (no sentinel).
  `node_alloc`/`node_release` are the only place a node's two allocations
  are made and freed. `rotate_left/right` own the `t->root` update.
  `insert_fixup`, `delete_fixup`, `transplant` allocate nothing.
  `validate_subtree`, `destroy_subtree`, `foreach_subtree` are still
  recursive (destroy and foreach must lose that in M3).
- `src/pool.c` — slab pool (M2). Does not exist yet.
- `tests/fault_alloc.{c,h}` — fault injector and the `rb_malloc`/`rb_free`
  seam (M1). Does not exist yet; today the wrappers are `static` in rbtree.c.
- `tests/test_rbtree.c` — unit tests plus the table-driven delete cases.
- `tests/fuzz.c` — random ops vs. a flag-array model, xorshift PRNG, seeded.

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
`make memcheck` cannot run on this Mac (no valgrind on Apple Silicon). Valgrind
runs in GitHub Actions on every push once the workflow is added; "green under
memcheck" means that run is green. Never report memcheck as passing locally.

## Status
M0 in progress: HW1 tree imported unchanged, new frozen header swapped in.
Nothing from M1-M3 exists yet.
