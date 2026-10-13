# Reflection: Slab Pool Allocator & Red-Black Tree Integration (HW2)

## 1. Slab Pool vs. Kernel `kmem_cache`
In this project, we built a custom slab pool (`struct rb_pool`) using an in-band header model: each allocated slab block begins with a `struct slab` metadata header followed by our node slots, with an intrusive free list threading through empty slots.

* **The Slab-Header Trade-off:** We chose in-band headers because requiring only a single `malloc` per slab minimizes allocation growth failure points, paying a small cost in header bytes (8 bytes on macOS, 16 bytes on Linux).
* **Kernel Scale Comparison:** Compared to the Linux kernel’s `kmem_cache` or SLUB allocators, our implementation is simple and single-threaded. Kernel allocators require advanced features for real-world scaling: **per-CPU caches** to eliminate core-to-core lock contention during concurrent allocations, **NUMA node awareness** to keep memory physically close to the CPU accessing it, and **slab coloring** to offset object start addresses and prevent L1 cache line conflict misses. Our allocator targets a fixed-size node payload without the overhead of generalized memory pressure handling.

---

## 2. Geometry, Alignment, and O(1) Statistics
* **Alignment Guarantees:** Because `pool_create` only receives an object size and never a concrete type, it must assume the strictest platform alignment. Both our slab header and slot stride are rounded up to multiples of `alignof(max_align_t)`, and slab memory comes directly from `malloc` (guaranteeing `max_align_t` alignment).
* **Statistics & Invariant:** The pool tracks `slabs` (total page-level allocations), `live` (currently active objects handed out and not freed), and `free_objs` (free-list slots plus never-used slots in the newest slab). 
* **The Invariant:** `live + free_objs == slabs * objs_per_slab`. Because allocations pop directly from the intrusive free list (or bump-allocate from the newest slab) and deallocations push back onto the head of the free list, all tracking operations execute in strict $O(1)$ time.

---

## 3. Debug Poisoning (`0xDD`) & ASan Blind Spots
Under debug builds (`#ifndef NDEBUG`), freed slots are `memset` with `0xDD`. 
* **Why Poisoning Matters:** AddressSanitizer (ASan) treats an entire slab block as a single live heap allocation from `malloc`. ASan is inherently blind to sub-slot boundaries inside custom pools. 
* **Evidence from Sabotage:** During our M3 sabotage testing (commit f3ad7fa), pooled ASan missed use-after-free corruption on sub-slots because the page remained "live" to the allocator. Inserting manual `0xDD` poisoning instantly turned subtle sub-slot reuse bugs into visible corruption patterns (triggering UBSan errors and segmentation faults), proving its necessity.

---

## 4. Testing Rigor & Agent Reliability
* **Validation Scale:** Our test suite runs robustly across sanitizers, with Valgrind memcheck executing deep stack tests at 20,000 nodes, while standard tests and ASan validate up to $10^6$ nodes.
* **Agent Collaboration & Bug Catching:** The AI assistant was exceptionally helpful for drafting test scaffolding and resolving compiler warnings. However, human oversight was critical in catching subtle bugs—such as the M1/M2 sweep leak-check bug where allocations were mistakenly compared against absolute zero rather than per-run deltas, or ensuring strict linkage rules for fault injector counters.