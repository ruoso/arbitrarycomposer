# pool.header_writeback_ordering — Generation-stamp the store-table snapshot

## TaskJuggler entry

`tasks/05-pool.tji:113-118` → `pool.header_writeback_ordering`, in milestone
`m9_release` (`tasks/99-milestones.tji:72`).

## Effort estimate

2d.

## Inherited dependencies

- `pool.workspace_store_directory` — **settled**. Refinement:
  `tasks/refinements/pool/workspace_store_directory.md` (Done 2026-07-11). It
  landed format 2: the A/B store table (`WorkspaceStoreEntry`,
  `src/pool/arbc/pool/workspace_file.hpp:116-121`), the per-root snapshot
  discipline, `adopt_snapshot`, and the behavioral-counter test this task must
  not regress.
- `pool.crash_tests` — **settled**. Refinement:
  `tasks/refinements/pool/crash_tests.md` (Done 2026-07-04). It landed the
  `SyscallInjector` seam (`workspace_file.hpp:203-244`), the in-process
  `SnapshotInjector` durable-image model, the fork-and-kill sweep, and the
  file-surgery primitives (`copy_file` / `write_at` / `truncate_file`,
  `src/pool/t/crash_tests.t.cpp:166-230`) that this task's new test builds on.
- `pool.checkpoints` — settled transitively; owns the commit protocol
  (`src/pool/arbc/pool/checkpoint.hpp:158-234`) and the `WorkspaceRoot`
  `(generation, root_index)` encoding (`checkpoint.hpp:47-59`).

## What this task is

The commit path writes each registered store's high-water into the *inactive*
store-table snapshot as plain stores (`checkpoint.hpp:185-188`), flips the
8-byte root slot for that same slot (`:190-191`), and makes **both** durable
with one `sync_header()` msync over the whole header region
(`:192-196`; `workspace_file.cpp:1090-1096`). The root slots sit at header
offsets 40/48 — page 0 — while the store table sits at offset 393328, page 96
of the same mapping. Those are different pages, and the kernel orders their
writeback not at all: a crash during the header msync can persist the **new
root** paired with the **stale snapshot**. `adopt_snapshot`'s coverage check
(`workspace_file.cpp:811-815`) only refuses a high-water that is too *high* for
the chunks present; a stale-*low* high-water sails through, and the recovery
then treats every chunk above it as post-checkpoint garbage and hole-punches it
(`workspace_file.cpp:825-835`) — under the very root that references those
slots.

This task makes that pairing **checkable**: the store-table snapshot gains a
generation stamp, written by the commit with the same generation it puts in the
root slot; open accepts a root only when its snapshot's stamp matches its own
generation, and otherwise falls back to the older root. The snapshots move to
page-aligned, one-page-each placement so a snapshot's stamp and rows share a
single unit of kernel writeback. Format major goes 2 → 3. No new syscall is
added anywhere.

## Why it needs to be done

`m9_release` ships a workspace file that is a durable document store; the
project's whole crash story rests on claim
`15-memory-model#store-high-water-durable-with-root` ("a kill anywhere between
the data msync and the header msync recovers the old root paired with the old
store-table snapshot, never a mismatched pair"). That claim is *witnessed* today
only against whole-syscall kills, which is all the crash sweep can produce — and
it is **false** against a torn header page. The failure is not a refusal to open;
it is a silent, catastrophic under-restore: the file opens on the new root whose
records live in chunks the recovery just punched.

`model.workspace_backing` and every downstream consumer of a reopened document
inherit this. It has to close before release.

## Inputs / context

### Design docs (normative)

- `docs/design/15-memory-model.md:254-274` — the store table and what reopen may
  assume (amended by this task, below).
- `docs/design/15-memory-model.md:276-308` — the checkpoint commit protocol,
  the durability epoch, and the root/snapshot pairing promise (amended by this
  task, below).
- `docs/design/15-memory-model.md:208-216` — "map the file, read the last valid
  root, resume". Before this task the doc never said what makes a root *valid*.
- `docs/design/16-sdlc-and-quality.md:14-25` — claims register + the same-commit
  amendment rule.
- `docs/design/17-internal-components.md` — levelization: everything here is
  inside `arbc::pool`; no new edge.

### Design-doc delta landed by this task (doc 16 same-commit rule)

`docs/design/15-memory-model.md` — two amendments, both inside `## File-backed
arenas`:

1. The store-table paragraph now states that each per-root snapshot is
   **page-resident** (page-aligned, rows + stamp inside one page), i.e. a single
   unit of kernel writeback.
2. The checkpointing paragraph now says the root/snapshot pairing is **verified
   on open, not assumed** — it names the unordered-writeback hazard explicitly,
   states the generation stamp and the newest-eligible-root selection rule, and
   records that the check costs no extra syscall.

No doc 00 decision-record bullet: this repairs a promise doc 15 already makes,
it does not reshape the project.

### Code (the seams this task extends)

- `src/pool/arbc/pool/workspace_file.hpp:63-85` — `WorkspaceHeader` (112 bytes,
  `static_assert` at :140). `root_slot_a` at offset 40, `root_slot_b` at 48,
  `store_table_offset` at 56, `max_stores` at 64, `reserved[5]` at 72.
- `src/pool/arbc/pool/workspace_file.hpp:116-121` — `WorkspaceStoreEntry`
  `{slot_stride, slot_align, chunk_slots, high_water}`, 16 bytes
  (`static_assert` at :143).
- `src/pool/arbc/pool/workspace_file.hpp:126-143` — the format constants;
  `k_workspace_format_major == 2` at :135.
- `src/pool/workspace_file.cpp:489-500` — the create-time layout math
  (`store_table_offset = sizeof(WorkspaceHeader) + max_chunks*24`;
  `header_bytes = align_up(store_table_offset + 2*max_stores*16, page)`).
- `src/pool/workspace_file.cpp:68-72` — `store_table(ab)`, the row-addressing
  accessor (`base + ab * max_stores`).
- `src/pool/workspace_file.cpp:100-105` — `publish_store_high_water`, the plain
  non-atomic row store.
- `src/pool/workspace_file.cpp:1098-1117` — `root_slot(ab)` (acquire load) and
  `publish_root_slot` (release store through `std::atomic_ref`, routed through
  the injector as `WorkspaceSyscall::RootFlip`).
- `src/pool/workspace_file.cpp:1090-1096` — `sync_header()`: **one** `io_msync`
  over `[0, d_header_bytes)`, i.e. header + directory + both snapshots.
- `src/pool/workspace_file.cpp:776-839` — `adopt_snapshot`: the row sanity
  checks, the coverage check at :811-815 (shortfall only), and the discard/punch
  of chunks above the high-water at :825-835.
- `src/pool/workspace_file.cpp:926-955` — header structural validation on open
  (magic, `format_major`, store-table bounds, `chunk_count <= max_chunks`).
- `src/pool/arbc/pool/checkpoint.hpp:47-59` — `WorkspaceRoot{generation,
  root_index}`, packed into the 8-byte root slot. **The generation counter this
  task stamps already exists**; `generation == 0` means "never written".
- `src/pool/arbc/pool/checkpoint.hpp:158-203` — the commit sequence.
- `src/pool/arbc/pool/checkpoint.hpp:105-116` and `:248-275` — the A/B selection
  rule, `a.generation >= b.generation`, **duplicated** in the `Checkpointer`
  constructor and in `Checkpointer::open`.
- `src/pool/arbc/pool/checkpoint.hpp:285-310` — `reserve_restored_all`, which
  feeds each row's `high_water` into `SlotStore::reserve_restored`.

### Tests / claims

- `src/pool/t/workspace_store_directory.t.cpp:568-628` — the behavioral-counter
  test the `.tji` note protects (`header_msyncs == 1`, `root_flips == 1`,
  `data_msyncs == live_chunks`).
- `src/pool/t/workspace_store_directory.t.cpp:541-566` — the "store tables cost
  zero extra pages / `data_offset` identical to format 1" size assertion.
- `src/pool/t/workspace_store_directory.t.cpp:760-867` — the byte-exact golden of
  the header + table region (`k_golden_header_region` at :819) and its regen
  case.
- `src/pool/t/crash_tests.t.cpp:166-230` — `copy_file` / `write_at` /
  `truncate_file`, the file-surgery primitives.
- `src/pool/t/crash_tests.t.cpp:242-360` — `SnapshotInjector`, whose durable
  image is "live page cache, with the two root slots rolled back to their last
  durable values".
- `src/pool/t/crash_tests.t.cpp:893-906` — the existing "torn inactive root slot
  → A/B redundancy still recovers" corruption case; the idiom the new test
  follows.
- `tests/claims/registry.tsv:71` —
  `15-memory-model#store-high-water-durable-with-root`, the claim this task
  makes true.

### Predecessor decisions this task honors

- `workspace_store_directory.md` Decision 3 ("two store tables, A/B, one per
  root slot") — kept; this task supplies the missing half of its argument. That
  decision reasoned only about *table-vs-header-msync* ordering and assumed the
  header msync publishes root and table atomically. It does not.
- `workspace_store_directory.md` Decision 4 ("high-water is written at commit,
  **zero** extra syscalls, pinned by a behavioral counter") — kept, untouched.
  The generation stamp is another plain store into a page the header msync
  already covers.
- `workspace_store_directory.md` Decision 5 ("bump `format_major` and refuse
  older files") — reapplied: 2 → 3, and format-2 files are refused as
  `UnsupportedFormat`. Pre-release; no migration path is owed.
- `crash_tests.md` ("two death models"; the seam is an installable
  `SyscallInjector`, not an interposer) — kept; this task adds **no** new
  production seam.

## Constraints / requirements

**Zero new syscalls.** The commit must still issue N data msyncs (N = live
chunks, 0 for a still scene) + exactly 1 header msync + exactly 1 root flip. The
generation stamp is a plain store into the snapshot page, published by the
header msync that already runs. `workspace_store_directory.t.cpp:568-628` keeps
its counter assertions verbatim.

**One writeback unit per snapshot.** The stamp only proves the pairing if it
cannot land without its rows. Each snapshot is therefore page-aligned and must
fit inside one page: `sizeof(WorkspaceStoreSnapshot) + max_stores *
sizeof(WorkspaceStoreEntry) <= page_size`. `create` rejects a larger
`max_stores` as a value (`MaxStoresExceeded`); `open` re-checks the recorded
geometry before trusting any snapshot.

**Threat model, stated once.** The format already assumes a naturally-aligned
8-byte store is torn-write-free (the root slot) and that a page is the unit of
kernel writeback; what it must not assume is any *ordering between* pages. This
task fixes exactly the ordering gap and adopts the same atomicity model
consistently — it does not introduce a checksum (see Decisions).

**One selection rule, one place.** The `a.generation >= b.generation`
comparison is currently duplicated at `checkpoint.hpp:105-116` and `:248-275`.
The eligibility rule must be factored into a single function used by both, or
the `Checkpointer` constructor will pick a `d_next_slot` that overwrites the
snapshot the open path just recovered from.

**Error surface stays stable.** `adopt_snapshot`'s existing errors
(`StoreLayoutMismatch`, `StoreDirectoryInconsistent`) keep their meanings and
remain hard failures — the generation check is a *root-selection* predicate that
runs before `adopt_snapshot`, not a retry wrapper around it. The existing
reopen-validation tests must pass unchanged.

**Levelization (doc 17).** Everything lands in `src/pool/` (`arbc::pool`) and
`src/pool/t/`. No new component edge, no new dependency (doc 10 policy
untouched).

**Platform.** The header layout is shared by the POSIX and Win32 backings; the
Win32 lane (`pool.checkpoints_win32`, `pool.crash_tests_win32`) must stay green.
The new test's file surgery uses the existing cross-platform `write_at`.

## Acceptance criteria

**Unit tests** (extend `src/pool/t/workspace_store_directory.t.cpp`):

- A commit stamps the snapshot it writes with the same generation it puts in the
  root slot, and leaves the other snapshot's stamp untouched (read back through
  `read_header` + a snapshot-stride-aware `read_store_row` helper).
- Fresh-file open: both roots and both stamps are 0; open succeeds with
  `valid == false`.
- `create` with `max_stores` too large for one page returns `MaxStoresExceeded`
  rather than laying out a straddling snapshot.
- `open` refuses a format-2 file as `UnsupportedFormat`.
- Round-trip: N commits alternating A/B, each reopen lands on the newest root and
  restores that root's high-waters exactly.

**The torn-header test** (new, in `src/pool/t/crash_tests.t.cpp`'s corruption
section, beside the torn-root-slot case at :893-906). Crash sweeps kill only at
whole-syscall boundaries and cannot manufacture a half-flushed header, so the
partial writeback is modelled by **file surgery on a copy**:

1. Commit generation N (root slot A), capture the bytes of snapshot slot B.
2. Allocate more, commit generation N+1 (root slot B, snapshot B).
3. Build the torn image: copy the post-commit file, then `write_at` the captured
   pre-commit snapshot-B bytes back over snapshot B's page — the exact durable
   state of "root page landed, snapshot page did not".
4. Open it: recovery must select **root A**, generation N, restore A's
   high-waters intact, resolve every record the generation-N root reaches, and
   punch nothing that root references. Against the pre-fix code this same image
   opens on root B and under-restores.
5. The mirror image (snapshot page landed, root page did not) recovers on the
   older root too — already safe, now pinned.
6. Both snapshots' stamps mismatched (both pages rolled back, roots kept) →
   `StoreDirectoryInconsistent`, a refusal, never a silent mis-restore.

**Claims register** — one new entry in `tests/claims/registry.tsv`, tagged
`// enforces:` on the torn-header test:

- **`15-memory-model#torn-header-falls-back-to-matching-root`** — A workspace
  whose root-slot page landed durably but whose store-table snapshot page did not
  (an unordered partial writeback of the header) opens on the older root whose
  snapshot generation matches it, restoring that root's high-waters intact,
  rather than pairing the new root with a stale snapshot and silently
  under-restoring.

Existing claims that must stay green (regression surface):
`#store-high-water-durable-with-root` (now true rather than merely unwitnessed),
`#reopen-routes-chunks-to-owning-store`, `#store-layout-mismatch-rejected`,
`#recovery-resumes-last-durable-root`, `#checkpoint-recovers-consistent-root`,
`#checkpoint-of-still-scene-skips-data-msync`,
`#workspace-io-faults-surface-as-values`
(`tests/claims/registry.tsv:62-76`).

**Behavioral counters (doc 16 — never wall-clock).**
`workspace_store_directory.t.cpp:568-628` keeps its assertions verbatim:
`data_msyncs == live_chunks`, `header_msyncs == 1`, `root_flips == 1`, plus
`Checkpointer::data_msyncs()/header_msyncs()` agreement. Only its
`read_store_row` helper is updated for the new snapshot stride. A still-scene
commit still issues zero data msyncs.

**Goldens.** The byte-exact header-region golden
(`workspace_store_directory.t.cpp:760-867`) is re-frozen for format 3 through
its existing regen case — byte comparison, no tolerance. The size assertion at
:541-566 changes from "zero extra pages / `data_offset` byte-identical to
format 1" to "`data_offset` grows by exactly two pages over format 1" (8 KiB on
a 4 KiB-page machine, 0.5% of the 388 KiB default header), with the arithmetic
spelled out.

**Crash / concurrency (doc 16, mandatory for a pool task).** The existing kill
sweep (`crash_tests.t.cpp:569`, `:592`, `:1096`, and the `[.nightly]` exhaustive
sweep at `:1110`) runs against the format-3 file unchanged and must stay green —
`SnapshotInjector`'s durable image (live pages + rolled-back roots) now yields
"stale root + fresh snapshot", the safe direction, and must still recover.
No new cross-thread state is introduced: the stamp is stored on the writer
thread inside `commit`, in the same critical section as the root flip. The pool
TSan lane must stay green; the Win32 lane must stay green. Gate: full suite green
under ASan/UBSan/TSan and **≥90% diff coverage** on changed lines.

**No deferred follow-up.** The Win32 backing shares these format structs and
rides in scope; this task registers no new WBS leaf.

## Decisions

1. **Stamp the snapshot with the root's generation; make it a root-selection
   predicate.** `WorkspaceRoot` already carries a 32-bit `generation`
   (`checkpoint.hpp:47-59`) — the stamp reuses it, so the "does this snapshot
   belong to this root?" question has a bit-exact answer with no new counter, no
   new syscall, and no new seam. Open reads both roots newest-first and takes the
   first whose snapshot stamp equals its generation; if neither qualifies, the
   header is corrupt beyond A/B redundancy and open fails.
   *Rejected*: making it a fallback wrapper around `adopt_snapshot` (retry the
   older root on *any* validation error). It would mask a genuine build mismatch
   or a torn table as a successful open on an older root, and it would break the
   existing reopen-validation tests that expect `StoreLayoutMismatch` /
   `StoreDirectoryInconsistent` to surface. A snapshot that fails *content*
   validation is a corrupt file; a snapshot that fails the *generation* check is
   merely not the one this root owns. Only the second is a fallback condition.

2. **One generation stamp per snapshot, not per row.** A per-row stamp
   (`WorkspaceStoreEntry` → 20 bytes) collides with the format-2 bind path, which
   writes a new store's identity columns into **both** snapshots at bind time,
   outside any commit. A row bound after the last commit would then carry a stamp
   that matches neither root, and the newest-eligible rule would spuriously reject
   the *durable* root. The per-snapshot stamp is untouched by bind, so bind stays a
   pure identity write.
   *Rejected*: per-row stamps. *Rejected*: stamping at bind time as well — it
   would mutate the durable snapshot outside a commit, which is precisely the
   discipline Decision 3 of `workspace_store_directory` exists to preserve.

3. **Page-align each snapshot; cap `max_stores` so a snapshot fits one page.** A
   stamp only proves the pairing if it cannot become durable without its rows. At
   the format-2 offsets the two snapshots share page 96 with the tail of the chunk
   directory, and nothing stops a caller-supplied `max_stores` from straddling a
   page boundary — at which point the stamp could land on one page while stale
   rows sat on the next, and the fix would silently stop working. Page-aligning
   each snapshot (`store_table_offset = align_up(header + directory, page)`,
   snapshot stride = one page) makes the invariant unconditional and checkable,
   and gives each snapshot a page no other writer dirties. Cost: `data_offset`
   grows by two pages. At `max_stores = 16` the used bytes of a snapshot are
   8 + 256 = 264, so it fits inside a single 512-byte sector as well — the
   pairing survives sector-granular tearing at the default geometry, though the
   format does not promise that in general.
   *Rejected*: leaving the table at its format-2 offset and merely asserting the
   default geometry happens to fit — it turns a correctness invariant into a
   coincidence of `max_chunks`.

4. **No checksum on the snapshot.** A per-snapshot CRC would additionally catch
   sub-page (sector-level) tearing, but the rest of the format already rests on
   the aligned-store/whole-page atomicity model (the root slot itself is an
   8-byte store with no checksum), and a checksum can only *refuse* a file, not
   repair it — `workspace_store_directory` Decision 3 rejected exactly that
   trade ("detects the mismatch but cannot repair it, turning a recoverable crash
   into a failed open"). The generation stamp, by contrast, *repairs*: it names
   the older root that is still coherent. Adding a checksum on top would be a
   separate, larger decision about the format's durability model, and it is not
   what this hole requires.

5. **Move the table into page 0 instead of stamping it — rejected.** Header
   (112 B) + both snapshots (528 B) would fit in page 0 alongside the root slots,
   making the torn pair impossible by construction at zero cost in pages
   (`data_offset` would even stay byte-identical to format 1). It is the simpler
   change, and it was seriously considered. It is rejected because it is strictly
   weaker: it *assumes* whole-page writeback atomicity rather than *verifying*
   the pairing, so if the assumption fails at sector granularity it fails
   silently and undetectably — the same silent under-restore, just rarer and
   harder to reproduce. It also still needs a create-time cap on `max_stores`, and
   it wires the store table's correctness to the directory's page-0 slack. The
   `.tji` note prescribes the stamp; the stamp also survives a future
   `max_stores` growth, which the co-residency trick does not.

6. **Model the partial writeback with file surgery, not a new injector mode.**
   The existing `SyscallInjector` contract is "return 0 to run the call, or a
   positive errno to *fail* it" (`workspace_file.cpp:441-454`); making it express
   "let the call appear to succeed but write back only these pages" would need a
   new return convention *and* the ability to rewrite the msync's `addr`/`len` —
   a production-code seam widening to serve one test. Building the torn image by
   `copy_file` + `write_at` over the snapshot page (the idiom already used at
   `crash_tests.t.cpp:893-906`) is deterministic, page-exact, cross-platform, and
   needs no production change at all.
   *Rejected*: extending `SyscallInjector` with a partial-msync mode.
   *Rejected*: per-page `sync_header` msyncs to create real orderable boundaries —
   it would multiply the header msync count and break Decision 4's counter pin,
   which is the whole point of doing this without spending a syscall.

7. **Bump `format_major` 2 → 3; refuse format-2 files.** The snapshot moved and
   grew a field; there is no honest way to read a format-2 table under the new
   layout. Pre-release, no user files exist.
   *Rejected*: a `format_minor` bump with a compat read path — it would let
   exactly the un-stamped files this task exists to distrust open as if they were
   stamped.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-14.

- `src/pool/arbc/pool/workspace_file.hpp`, `src/pool/workspace_file.cpp` — added `WorkspaceStoreSnapshot` stamp struct; page-aligned, one-page-each snapshot placement; `snapshot_generation`/`publish_snapshot_generation`; create-time `MaxStoresExceeded` cap; open re-checks page residency; format major bumped 2 → 3.
- `src/pool/arbc/pool/checkpoint.hpp` — factored `select_root()` as the single eligibility rule (newest root whose snapshot stamp matches its generation), used by both `Checkpointer::open` and the constructor's `d_next_slot` choice; `commit` stamps the snapshot it writes; no eligible root → `StoreDirectoryInconsistent`.
- `src/pool/t/crash_tests.t.cpp` — new torn-header test (claim `15-memory-model#torn-header-falls-back-to-matching-root`): file-surgery torn images for control (gen 2), root-landed/snapshot-stale → falls back to root A gen 1 with high-waters intact, mirror tear, and both-stale → refusal.
- `src/pool/t/workspace_store_directory.t.cpp` — unit tests: commit stamps only its snapshot; fresh-file zero roots+stamps open with `valid == false`; over-page `max_stores` → `MaxStoresExceeded`; format-2 file → `UnsupportedFormat`; alternating-commit round-trip incl. reopened checkpointer publishing into the other slot; golden re-frozen for format 3; size assertion updated to "`data_offset` grows by exactly two pages".
- `tests/claims/registry.tsv` — new claim `15-memory-model#torn-header-falls-back-to-matching-root`.
- `docs/design/15-memory-model.md` — two amendments: per-root snapshot is page-resident (one writeback unit); pairing verified on open via generation stamp (not assumed).
