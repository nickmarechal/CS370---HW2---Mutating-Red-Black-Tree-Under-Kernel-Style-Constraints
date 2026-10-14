# Claude Code transcripts (HW2)

Raw `.jsonl` session files, copied unmodified from `~/.claude/projects/`
(spec section 13: not a summary, not an `/export`, not retyped). This README
is only an index; the transcript files themselves are untouched.

## Sessions

| File | Ran (Mountain Time) | What it covers |
|---|---|---|
| `8545f1b6-685c-4092-b3b0-55d5bdd6ab51.jsonl` | 2026-10-08 17:14 → 2026-10-13 20:53 | All of HW2: reading the spec, M0–M3, review, write-up checks, packaging, autograder fix |

This was the only Claude Code session used for HW2. It was started from the
`CS 370` course folder, so the source file lives in
`~/.claude/projects/-Users-nicholasmarechal-Desktop-CS-370/`. The other session
files in that folder are HW1, the term project and midterm prep; each was
searched for HW2 content (`fault_alloc`, `CS370-HW2`, `sweep.c`) and none had
any, so they are not included.

The session was still open when this copy was made, so the file is a snapshot
taken at the time of the commit that added it.

## When each part happened (session timestamps + git history)

| Date | Work | Commits |
|---|---|---|
| 2026-10-08 | Read spec + companion; repo set up; HW1 tree imported; new frozen header; CLAUDE.md amended | `5e3a2f3` `765541a` `e53b9f8` |
| 2026-10-09 | M0 seam: `fault_alloc.h`, `fault_alloc.c`, `rbtree.c` uses the external seam (linkage-rules question raised here) | `807b66e` `1806afe` `c3e4389` |
| 2026-10-10 | CLAUDE.md code map after the seam move | `cf7c1ed` |
| 2026-10-11 | M1: planned in plan mode (sweep file, output, scenario size chosen); injector + fault sweep | `76401dd` |
| 2026-10-13 morning | M1: sabotage check found the per-run leak bug; Actions battery; gcc-14 format-truncation fix | `6602391` `859dac0` `4ce2536` |
| 2026-10-13 late morning | M2: plan split into Part A (pool alone) and Part B (tree + `--pooled`) | `3e068f0` `433df75` `09adb70` |
| 2026-10-13 midday | M3: foreach walk, paper drill → right-spine destroy, 10^6-node 64 KiB stack test | `2560b0a` `f3ad7fa` `74bb3f8` |
| 2026-10-13 afternoon | Review pass; findings 1–2 fixed; PROMPTLOG and REFLECTION | `fdd3370` `c936427` |
| 2026-10-13 late afternoon | Final transcript copy; tar/zip built; Canvas rejected `.gitkeep` and the CI `.yml`, both removed | `3749549` `a2b946f` |
| 2026-10-13 evening | Autograder M2 finding (destructor count); traced to the same-pointer overwrite skip; test first, then fix | `a45586d` |
