# serialize.asset_gc — mark-and-sweep GC for unreferenced tile blobs

## TaskJuggler entry

[`tasks/60-serialize.tji:74-79`](../../60-serialize.tji) — `task asset_gc
"Mark-and-sweep GC for unreferenced tile blobs"`, `effort 2d`, `depends
!raster_tile_store`.

> Walk the saved document's referenced hashes and delete unreferenced `tiles/**`
> blobs. An explicit user action (never implicit on save) because an incremental
> save cannot know what another document version or a concurrent editor still
> references. Without it the asset directory grows monotonically across a long
> editing session. Source-of-debt: `tasks/refinements/serialize/raster_tile_store.md`
> (Deferred follow-ups), commit `serialize.raster_tile_store`. Doc 08.

Milestone: `m9_release` ([`tasks/99-milestones.tji:72`](../../99-milestones.tji)).

## Effort estimate

**2d**, apportioned:

| piece | days |
| --- | --- |
| `serialize` `AssetReaper` seam + `unreferenced_tiles` set-diff + unit test | 0.25 |
| `runtime` `FilesystemAssetReaper` (`recursive_directory_iterator`, remove, empty-fan-out prune, counters) + test | 0.5 |
| `runtime` `collect_referenced_tiles` (JSON mark walk, compositions recursion, fail-safe) + `sweep_tile_store` driver + directory-scan entry | 0.5 |
| Claims, golden round-trip-after-GC, multi-document / idempotence / non-tile-untouched / fail-safe tests, ≥90% diff coverage, doc deltas | 0.75 |

The work is small and self-contained: the predecessor already landed every hard
primitive (the store, the fan-out URI derivation, the hash, `is_tile_hash`). This
task is the *reverse* traversal — enumerate what is on disk, subtract what is
referenced, delete the rest — plus the safety contract that makes deleting sound.

## Inherited dependencies

### Settled

- **`serialize.raster_tile_store`** ([refinement](raster_tile_store.md), done
  2026-07-14) — the source-of-debt. It handed forward everything this task
  reverses:
  - The on-disk layout: tile blobs live under `assets/tiles/<hh>/<hash>`, a 2-hex
    fan-out with no suffix, derived by `tile_blob_uri(base, hash)`
    ([`src/serialize/arbc/serialize/tile_blob.hpp:103-109`](../../../src/serialize/arbc/serialize/tile_blob.hpp));
    the name is a 32-char lowercase-hex SHA-256/128 digest validated by
    `is_tile_hash`. `k_default_tiles_base = "assets/tiles/"`
    ([`src/runtime/codec_raster.cpp:68`](../../../src/runtime/codec_raster.cpp)).
  - The reference shape: a raster layer's `params` carry a flat, row-major
    `blobs` array of level-0 hashes plus `tiles`/`edge`/`width`/`height`
    ([`codec_raster.cpp:163-169`](../../../src/runtime/codec_raster.cpp)). **This
    `blobs` array is the only place a document names a tile blob** — the mark
    walk reads exactly it.
  - The never-delete rule this task exists to reconcile: **a save never deletes a
    blob** (`raster_tile_store.md` Constraint 6), because *"another document
    version, another `.arbc`, or a concurrent editor may reference it"*. The
    honest cost is stated in the same refinement: *"the asset directory grows
    monotonically across a long editing session — the honest cost of this task's
    never-delete rule."* `raster_tile_store.md:453-461` registers **this task** by
    id as the reclamation follow-up.
  - The write-side seam whose read/reap counterpart this task completes:
    `AssetSink` is deliberately write-only and says so —
    [`src/serialize/arbc/serialize/save_context.hpp:72-73`](../../../src/serialize/arbc/serialize/save_context.hpp):
    *"A sink NEVER DELETES … Reclaiming unreferenced blobs is an explicit
    user-driven sweep (`serialize.asset_gc`), never a side effect of saving."*
    `FilesystemAssetSink`
    ([`src/runtime/arbc/runtime/filesystem_asset_sink.hpp:39-57`](../../../src/runtime/arbc/runtime/filesystem_asset_sink.hpp))
    is the concrete-store idiom to mirror: `strip_file_scheme` +
    `std::filesystem` point operations, counters, and — critically — the
    temp+rename write-if-absent that guarantees **every on-disk blob under a valid
    hash name is complete**, so this task may treat presence-by-name as truth.

### Pending

(none — `raster_tile_store` is `complete 100`; it is the sole predecessor.)

### Downstream (this task unblocks)

Nothing in the WBS `depends` on `asset_gc`; it is a leaf of `m9_release`. It is the
back half of the asset-directory data-safety story — without it the never-delete
rule is a monotonic leak, so a long editing session's `.assets/` is unbounded.

## What this task is

Give the core an **explicit, user-driven mark-and-sweep** over the on-disk tile
store: enumerate every blob under `assets/tiles/**`, subtract the set of hashes
referenced by the document(s) the caller wants to keep, and delete the
difference. It is never on the save path (Decision 1) — a save cannot see the
references an in-memory undo history or another editor holds, so deleting there
would corrupt a document the save never opened. An explicit sweep is the user
asserting *"clean up; I have accounted for what must survive."*

The mark is a plain JSON walk: for each `.arbc` document to preserve, harvest
every `params.blobs` hash, recursing into non-root compositions and operator
input children so no reachable raster content is missed. The sweep enumerates the
filesystem, computes the delete set **in full before deleting anything**
(Decision 3), and unlinks each unreferenced blob. It touches `tiles/**` only —
imported images and any other asset are out of scope (Decision 4). It reports
counters, not wall-clock (`scanned`, `referenced`, `deleted`,
`bytes_reclaimed`), and it offers a `dry_run` that reports what it *would*
reclaim without deleting.

It does **not** coordinate cross-process concurrent editors (that is a host
policy, not agent work — Decision 5), it does **not** run on the housekeeping
thread or any implicit schedule, and it does **not** touch the in-memory pool or
model — it is filesystem-and-JSON only.

## Why it needs to be done

`raster_tile_store` made a painting saveable by making the store append-only:
write-if-absent, never delete. That is correct for a save — a save cannot prove a
blob is dead — but it means the asset directory only ever grows. Every dab that
touches a tile writes a new blob under a new hash; the pre-dab blob stays on disk
even after the layer no longer names it, kept only in case some *other* reference
(undo, another `.arbc`, another editor) still points at it. Over a long session,
`.assets/tiles/` accumulates the entire history of every stroke. Doc 08 names the
only legitimate remedy — [`08-serialization.md:50-53`](../../../docs/design/08-serialization.md):
*"reclaiming unreferenced blobs is an explicit sweep, never a side effect of
saving."* This task is that sweep. It is the difference between an editing session
whose scratch grows without bound and one a user can reclaim.

## Inputs / context

### Design docs (normative, doc 16)

- **[`docs/design/08-serialization.md:21-63`](../../../docs/design/08-serialization.md)
  — §The asset directory.** The container model: a `.arbc` plus a sibling
  `project.assets/`, tiles at `assets/tiles/<hh>/<hash>` (L26-32). **L48-53 is the
  governing sentence for this task**: the sink is *"write-if-absent"*, a save
  *"never deletes a blob — another document version, another `.arbc`, or a
  concurrent editor may reference it — so reclaiming unreferenced blobs is an
  explicit sweep, never a side effect of saving."* The mechanism of that sweep is
  intentionally left open here; this task fills it.
- **[`docs/design/08-serialization.md:350-456`](../../../docs/design/08-serialization.md)
  — Principle 8.** Content-addressed store: blobs keyed by hash of uncompressed
  storage bytes, dedup *"across layers AND across undo versions for free"*
  (L367-369), incremental save as a consequence. The dedup that makes the store
  small is exactly what makes GC non-trivial: a blob may be shared by many
  references, so it is dead only when **no** reference names it.
- **[`docs/design/00-overview.md:460-463`](../../../docs/design/00-overview.md)** —
  the decision-record bullet the predecessor landed: the sink is write-if-absent
  and *"a save never deletes, because another version, another document, or a
  concurrent editor may reference a blob. Decided in `serialize.raster_tile_store`."*
  This task appends the completing half (Decision 1's doc-00 delta).
- **[`docs/design/17-internal-components.md:69`](../../../docs/design/17-internal-components.md)** —
  `arbc::serialize` is **L4**, edges `{contract, model}` (+ JSON, zstd), and
  already owns *"`LoadContext`/`AssetSource` + `SaveContext`/`AssetSink` … 
  content-addressed tile-blob store (hash, byte-shuffle, compress/decompress)."*
  The abstract reaper seam joins this cell.
  **[`17:71`](../../../docs/design/17-internal-components.md)** — `arbc::runtime`
  is **L5**, *"everything below"*; it already owns `FilesystemAssetSink` /
  `FilesystemAssetSource`, so the concrete `FilesystemAssetReaper` and the
  Document-shaped mark walk belong there (Decision 2). **No `check_levels.py`
  change**: `ALLOWED["runtime"]` already reaches both `serialize` and the JSON
  library ([`scripts/check_levels.py:51-54`](../../../scripts/check_levels.py)).
- **[`docs/design/16-sdlc-and-quality.md`](../../../docs/design/16-sdlc-and-quality.md)** —
  tier 3 byte-exact goldens (the round-trip-after-GC golden), tier 4 behavioral
  counters (*"Wall-clock tests lie in CI; counters don't"* — the sweep's
  `deleted`/`bytes_reclaimed` land here, never a timing assertion). Diff coverage
  is a hard ≥90% gate.

### Source seams

- **[`src/serialize/arbc/serialize/save_context.hpp:72-98`](../../../src/serialize/arbc/serialize/save_context.hpp)** —
  `AssetSink` (`put` write-if-absent, `contains`, `blobs_written`) with its
  explicit *"NEVER DELETES"* comment naming this task. The reaper is the third
  role beside `AssetSource` (read) and `AssetSink` (write); it may **not** be
  folded into `AssetSink` without contradicting that comment (Decision 2).
- **[`src/serialize/arbc/serialize/load_context.hpp:33-64`](../../../src/serialize/arbc/serialize/load_context.hpp)** —
  `AssetSource::request(resolved_uri, on_ready)` is **fetch-by-URI only, no
  enumeration**; `LoadContext` has no directory listing either. The free
  `resolve_uri(base_uri, reference)` (L64) and `normalize_uri` (L54) are the
  path-resolution helpers to reuse — the reaper must resolve the tiles base the
  same way a save/load does.
- **[`src/serialize/arbc/serialize/tile_blob.hpp:103-109`](../../../src/serialize/arbc/serialize/tile_blob.hpp)** —
  `tile_blob_uri(base, hash)` (the fan-out derivation) and `is_tile_hash` (the
  32-lowercase-hex validator). The reaper enumerates on-disk names and validates
  each with `is_tile_hash`; anything that is not a tile-hash name is not swept
  (Constraint 4).
- **[`src/runtime/arbc/runtime/filesystem_asset_sink.hpp:39-57`](../../../src/runtime/arbc/runtime/filesystem_asset_sink.hpp)**
  and **[`src/runtime/filesystem_asset_sink.cpp:22-119`](../../../src/runtime/filesystem_asset_sink.cpp)** —
  the concrete-store idiom to mirror: `strip_file_scheme` (L22-25), no configured
  base directory (the resolved URI *is* the path), point `std::filesystem`
  operations, and the temp+rename that guarantees on-disk completeness. The
  reaper reuses `strip_file_scheme` and adds the one operation the sink refuses:
  `std::filesystem::remove`.
- **[`src/runtime/codec_raster.cpp:89-170`](../../../src/runtime/codec_raster.cpp)** —
  the ground-truth *writer* of `params.blobs` (L124-132, L163-169). The mark walk
  reads the mirror: for a content body, `params.blobs` is an array of hash
  strings, one per level-0 tile. **Level 0 only** — mips are never persisted
  (`raster_tile_store.md` Constraint 8), so no reachable blob is ever a mip.
- **[`src/runtime/document_serialize.cpp:202-397`](../../../src/runtime/document_serialize.cpp)** —
  `capture_snapshot` (L202-342) already does the breadth-first walk over the
  composition graph (root composition + `composition_ref()` recursion +
  `for_each_layer_in` + `inputs()` transitively) that the JSON mark walk must
  mirror in data form; `save_document` (L378-397) is where the never-delete
  save path terminates — the mark walk deliberately does **not** run here.
- **[`src/runtime/t/filesystem_asset_sink.t.cpp:95`](../../../src/runtime/t/filesystem_asset_sink.t.cpp)** —
  the tree's only existing `recursive_directory_iterator` (in a test); it walks
  the tile tree and is the usable reference for the reaper's enumeration. **No
  production code iterates a directory today** — the reaper's walk is the first,
  so it carries its own error-as-value handling (`std::error_code` overloads,
  never throwing `directory_iterator`).

### Predecessor / sibling refinements

- [`raster_tile_store.md`](raster_tile_store.md) — Constraint 6 (never delete,
  the reasons), Decision 1 (the `AssetSink`/`SaveContext` seam this mirrors),
  Decision 5 (the in-memory `RasterTileStore` memo is a *last-saved-version* pin,
  **not** a directory index — it is not a source of "all referenced hashes on
  disk" and this task does not touch it), and the `serialize.asset_gc` follow-up
  registration at L453-461.
- [`compositions_table.md`](compositions_table.md) — the reader/writer recurse
  into non-root compositions via a document-level `compositions` table; the mark
  walk must recurse the same way so a raster layer inside a nested composition is
  not missed and its blobs wrongly swept.
- [`reader.md`](reader.md) / [`format_tests.md`](format_tests.md) — the loader is
  an untrusted, fuzzed surface; errors are values, allocations are bounded. The
  mark walk parses `.arbc` JSON and must therefore be fail-safe as a value
  (Constraint 6), never throwing across the boundary.

## Constraints / requirements

1. **Never implicit on save.** No save path — `save_document`,
   `serialize_snapshot`, autosave, the housekeeping thread — may delete a blob.
   The sweep is only ever reached through the explicit GC entry point a host calls
   in response to a user "clean up" action. This is doc 08 L50-53 verbatim and the
   whole reason the task exists as a separate operation (Decision 1).

2. **Mark before sweep; plan in full before deleting.** GC collects the *complete*
   referenced set and the *complete* on-disk set, computes the delete set as a
   pure subtraction, and only then unlinks. No enumerate-and-delete interleaving.
   A mark bug can then never race a delete, and a partial run deletes only a prefix
   of genuine orphans — the store stays consistent (every retained blob still
   present) at every intermediate point (Decision 3).

3. **Fail-safe: a mark that cannot be fully computed deletes nothing.** If any
   root document fails to parse, or any `params.blobs` entry is not a valid
   `is_tile_hash` string, or the directory enumeration fails, GC returns an error
   value and performs **zero** deletions. Over-preservation is always safe; a
   partial mark that then deleted would be data loss. Errors are values, never
   exceptions across the boundary (mirrors the loader's discipline).

4. **Scope is `tiles/**` only.** GC enumerates and may delete only blobs under the
   resolved `assets/tiles/` subtree whose basename passes `is_tile_hash`. Imported
   images (`org.arbc.image` assets, e.g. `assets/bg.png`) and anything else in the
   asset directory are never touched — they are referenced by URI, not content
   hash, and are outside this task's reference model.

5. **The referenced set is the union across every document the caller preserves.**
   The GC API takes a set of root documents (or their referenced-hash sets); the
   mark is the union. This is the direct answer to doc 08's cross-`.arbc` hazard: a
   blob referenced by *any* preserved document survives. The convenience entry
   scans a project directory for `*.arbc` and unions them all. **The safety
   contract is the caller's:** the root set must include every document (and every
   in-memory-open document's current serialized state) that must survive; a
   document GC is not told about can have its unique blobs reclaimed. This is why
   GC is explicit and never inferred.

6. **The mark walk is a generic JSON traversal keyed on the reference *shape*, not
   on a kind type.** It harvests `params.blobs` hashes from *every* content body
   it reaches — root composition layers, non-root compositions (the `compositions`
   table), and operator input children — recursing exactly as `capture_snapshot`
   does. Keying on the presence of a `blobs` array (rather than on
   `kind == "org.arbc.raster"`) means a placeholder content that round-tripped an
   unknown kind's tile references is also preserved: conservative by construction.

7. **Levelization (doc 17, CI-enforced).** The abstract `AssetReaper` seam and the
   pure `unreferenced_tiles` subtraction live in `arbc::serialize` (L4), naming no
   raster type and no filesystem — a byte/format API beside `AssetSource`/
   `AssetSink`. The concrete `FilesystemAssetReaper`, the JSON mark walk, and the
   top-level GC entry live in `arbc::runtime` (L5), the one place that already sees
   the filesystem, the JSON library, and the raster params shape.
   `scripts/check_levels.py` already permits every edge — **do not widen any
   `ALLOWED` entry** (an edit there means the L4/L5 split was violated).

8. **No pool or model interaction; no concurrency surface.** GC is pure filesystem
   + JSON. It never `peek()`s the pool, never touches a `RasterTileStore` memo,
   never pins a version. It holds no shared mutable state, so it introduces no
   in-process data race — the concurrent-editor hazard doc 08 names is *cross
   process*, handled by the caller-contract (Constraint 5) and the write-if-absent
   repair property (Decision 6), not by locking. No TSan lane is scoped, and the
   refinement states why rather than adding a vacuous one (doc 16: scope
   concurrency coverage only where there is shared mutable state).

## Acceptance criteria

- **The sweep deletes exactly the unreferenced set** *(behavioral counter +
  golden)*. New claim
  **`08-serialization#asset-gc-deletes-only-unreferenced-tiles`**: over a store
  with referenced-hash set *R* and on-disk set *D*, GC deletes exactly *D \ R* and
  retains every blob in *R* — asserted on the `GcReport` counters (`deleted ==
  |D \ R|`, and every hash in *R* still `contains()` after the run), never on
  directory size or wall-clock. Backed by a round-trip: `load` of the swept
  document is byte-identical to before GC (every referenced blob still decodes and
  verifies against its name).

- **A save leaks; only an explicit GC reclaims** *(behavioral counter)*. New claim
  **`08-serialization#asset-gc-is-explicit-never-on-save`**: save a painted
  document; apply one dab touching tile set *T*; re-save. The pre-dab blobs of the
  overwritten tiles are still on disk (the save deleted nothing — this is the
  never-delete rule). Then run GC with the current document as sole root: exactly
  the now-orphaned pre-dab blobs are deleted, the current tiles retained. Pins that
  reclamation is a distinct, explicit operation and that the save path itself
  never deletes.

- **References union across documents** *(behavioral)*. New claim
  **`08-serialization#asset-gc-unions-references-across-documents`**: two `.arbc`
  files share one asset directory; a blob referenced only by the *second* document
  is **not** deleted when GC is given *both* as roots, and a blob referenced by
  neither *is* deleted. This is the cross-`.arbc` safety property that is the whole
  reason the sweep is explicit and caller-rooted rather than implicit on a single
  save (doc 08 L50-52).

- **Non-tile assets are untouched** *(behavioral)*. New claim
  **`08-serialization#asset-gc-leaves-non-tile-assets`**: an imported
  `assets/bg.png` (and any name that is not `is_tile_hash` under `tiles/`) survives
  a GC that reclaims orphaned tiles beside it. Asserted by presence after the run.

- **Fail-safe: a broken mark deletes nothing** *(behavioral)*. New claim
  **`08-serialization#asset-gc-fails-safe-deletes-nothing`**: a root document with
  a malformed `params.blobs` (non-array, or a non-hex entry), or an unreadable root
  file, makes GC return a `ReaderError`/`AssetReaperError` value with **zero**
  deletions — the on-disk set is bit-for-bit unchanged. Over-preservation on any
  doubt; never a partial delete.

- **GC is idempotent** *(behavioral counter)*. New claim
  **`08-serialization#asset-gc-is-idempotent`**: a second GC immediately after a
  first, same roots, deletes **zero** blobs (`deleted == 0`). The store reaches a
  fixed point in one pass; re-running is a no-op, which is what makes a `dry_run`
  followed by a real run predictable.

- **`dry_run` reports without mutating** *(behavioral counter)*. The `dry_run`
  mode returns a `GcReport` whose `deleted`/`bytes_reclaimed` equal what a real run
  *would* reclaim, while the on-disk set is unchanged — the user previews the
  reclamation before committing to it. Asserted by comparing the dry-run report to
  a subsequent real run's report and to the pre/post on-disk sets.

- **Concurrency**: **no TSan lane, stated deliberately.** GC holds no shared
  mutable state and touches neither pool nor model (Constraint 8); the
  concurrent-editor hazard is cross-process and handled by contract, not locks.
  The refinement records this rather than adding a lane with nothing to catch.

- **Design-doc delta (same commit)**. Doc 08 §The asset directory — a normative
  paragraph naming the `AssetReaper` seam symmetric to `AssetSource`/`AssetSink`,
  stating the sweep's fail-safe (a mark that cannot be fully computed deletes
  nothing), its `tiles/**`-only scope, and the caller-completeness safety contract
  (the root set must name every document that must survive; over-deletion of a
  tile still in memory is repaired by write-if-absent on the next save). Doc 17 —
  extend the `arbc::serialize` cell (L69) to name the reaper seam. Doc 00 — one
  decision-record bullet completing the write-if-absent story: reclamation is an
  explicit, caller-rooted, fail-safe sweep (Decision 1). Enumerated under
  Decisions 1–2.

- **Coverage / build / WBS gate**. ≥90% diff coverage on changed lines;
  `-Werror -Wpedantic` clean; `scripts/check_levels.py` green with **no `ALLOWED`
  edit**; `scripts/check_claims.py` green (register rows and `enforces:`-tagged
  tests land together); `tj3 project.tjp 2>&1 | grep -iE "error|warning"` silent.

- **Test locations** (area convention, `raster_tile_store.md`): `AssetReaper` /
  `unreferenced_tiles` unit test at `src/serialize/t/asset_reaper.t.cpp`;
  `FilesystemAssetReaper` unit test at `src/runtime/t/filesystem_asset_reaper.t.cpp`
  (mirroring `filesystem_asset_sink.t.cpp`, using a temp dir); the mark-walk,
  multi-document, idempotence, fail-safe, non-tile, and round-trip-after-GC claims
  at a top-level `tests/asset_gc.t.cpp`.

- **No deferred WBS follow-ups.** The task is self-contained: every hard primitive
  exists, and the two items one might defer are not agent-implementable —
  cross-process editor coordination is a host-policy judgment call, and a
  user-facing "clean up" UI/CLI affordance is host/packaging surface (the library
  entry point is the deliverable; the host merely calls it). Both are surfaced in
  the return summary for the parking lot rather than encoded as WBS leaves.

## Decisions

### 1. GC is explicit, caller-rooted, and fail-safe — never on the save path. *(doc 08 + doc 00 delta)*

Doc 08 L50-53 already forecloses implicit deletion: a save cannot prove a blob is
dead, because an in-memory undo history, another `.arbc`, or a concurrent editor
may reference it. So the sweep takes its roots from the *caller*: the set of
documents to preserve, whose `params.blobs` unions form the live set. The delete
set is `on_disk \ live`. The caller owns the completeness of that root set — GC's
contract is "I will keep every blob any root you named references, and reclaim the
rest." That contract is safe to honor precisely because the caller, not the save
path, is the one place that knows which documents are open.

*Rejected — implicit GC on save (delete a version's orphans as the next save
lands).* Exactly what doc 08 forbids, and for the stated reason: the save sees one
version of one document and would delete blobs another reference still needs. It
would also make every save O(store) instead of O(dab).

*Rejected — reference-count the on-disk blobs (a sidecar count file per blob,
incremented on save, decremented on release).* A persistent refcount across
processes and undo histories is a distributed-consensus problem — a crash between
write and count-bump corrupts the count, and a concurrent editor's decrement races
another's increment. Content-addressing plus an explicit mark-and-sweep sidesteps
all of it: the truth is recomputed from the documents each sweep, never
incrementally maintained. This is the same reason `git` uses mark-and-sweep GC
over refs rather than per-object refcounts.

**Doc 08 delta**: the §The asset directory paragraph named under Acceptance.
**Doc 00 bullet**: reclamation completes the write-if-absent story — an explicit,
caller-rooted, fail-safe sweep; decided in `serialize.asset_gc`.

### 2. Split the reaper seam (L4) from the concrete store and mark walk (L5). *(doc 17 delta)*

Mirroring `raster_tile_store` Decision 1/2 exactly. `arbc::serialize` (L4) gains a
byte/format-oriented, filesystem-free abstract seam beside `AssetSource`/
`AssetSink`:

```cpp
struct AssetReaperError { enum class Kind { EnumerateFailed, RemoveFailed }; Kind kind; };

class AssetReaper {                                  // the third role: reap
public:
  virtual ~AssetReaper() = default;
  // Every on-disk tile blob's 32-hex name, under the tiles subtree.
  virtual expected<std::vector<std::string>, AssetReaperError> list_tile_hashes() const = 0;
  // Delete the blob for `hash`; returns whether a file was removed.
  virtual expected<bool, AssetReaperError> remove_tile(std::string_view hash) = 0;
};

// Pure set subtraction: present-on-disk minus referenced. No I/O, no raster type.
std::vector<std::string> unreferenced_tiles(const std::unordered_set<std::string>& referenced,
                                            std::span<const std::string> present);
```

`arbc::runtime` (L5) owns `FilesystemAssetReaper` (the
`recursive_directory_iterator` over the resolved `assets/tiles/` tree, `is_tile_hash`
filtering, `std::filesystem::remove`, empty-fan-out pruning, and the `scanned`/
`deleted`/`bytes_reclaimed` counters), the JSON mark walk
`collect_referenced_tiles(const nlohmann::json&) -> expected<...>`, the driver
`sweep_tile_store(referenced, AssetReaper&, bool dry_run) -> GcReport`, and the
convenience `gc_project_directory(path, dry_run)` that scans `*.arbc` and unions
their marks.

*Rejected — put the whole GC in `serialize` (L4) with a direct filesystem
dependency.* `serialize`'s doc-17 cell has no filesystem edge, and the mark walk
needs to know the raster `params.blobs` shape, which is a codec concern
(`codec_raster.cpp`, L5). The write side already resolved this: `AssetSink` is L4,
`FilesystemAssetSink` is L5. GC follows the same line.

*Rejected — fold delete into `AssetSink`.* `save_context.hpp:72` states the sink
*"NEVER DELETES."* A third role (`AssetReaper`) keeps that invariant legible and
lets a host that stores blobs somewhere other than a POSIX directory implement
reap independently of write.

**Doc 17 delta**: extend the `arbc::serialize` responsibility cell (L69) to name
the reaper seam beside `AssetSource`/`AssetSink`.

### 3. Mark completely, then sweep — plan the full delete set before unlinking.

GC computes the union of referenced hashes and the full on-disk list, subtracts,
and only then deletes. Two payoffs: a mark bug cannot delete a referenced blob
mid-enumeration (the plan is validated against the complete referenced set first),
and a crash or `ENOSPC` partway through the delete loop leaves a store that is
still consistent — a strict subset of true orphans removed, every referenced blob
intact. Delete is a bare `std::filesystem::remove` per blob; unlike a write it
needs no temp+rename, because removing a content-addressed blob is already atomic
and idempotent (a re-run simply finds it gone).

*Rejected — stream: enumerate and delete in one pass, checking each name against
the referenced set as it is visited.* Marginally less memory, but it interleaves
the mark and the mutation, so a bug in the reference check deletes live data
before the run can be aborted. The whole-plan-first order makes over-deletion
structurally impossible from a mark error.

### 4. Scope to `tiles/**`; other assets are out of the reference model.

Only blobs under the resolved `assets/tiles/` subtree whose basename is a valid
`is_tile_hash` are enumerated or deleted. Imported images and any other asset are
referenced by *URI*, not by content hash, so they are not in this task's reference
graph and GC has no basis to judge them dead. Sweeping them would need a different
mark (URI references, `external_asset_ref()`), which no design doc scopes here.

*Rejected — a general asset GC over the whole `assets/` directory.* It would need
to model every kind's URI references (images, imageseq frames, nested `.arbc`), a
much larger reference graph than the task note ("`tiles/**` blobs") describes, and
would risk deleting an imported image whose reference the mark missed. Tile blobs
are the only asset the never-delete rule created a leak for; they are the whole
scope.

### 5. Cross-process concurrent-editor safety is a caller contract, not a lock. *(surfaced for parking lot)*

Doc 08 names a concurrent editor as a reason a save cannot delete. GC does not
solve concurrent editing with a lock file or a store-wide mutex — that is a host
policy (which processes may edit a shared store, and when a "clean up" is
permitted) and a design judgment, not agent-implementable work. GC's contract is
Constraint 5: it reclaims blobs no *named root* references, and the caller must
name every document that must survive. In practice this is safe under an
in-process editor because of Decision 6; the cross-process case is the host's to
gate. Recorded here and surfaced in the return summary for the parking lot rather
than encoded as an "audit" WBS task.

### 6. Over-deletion of a still-resident tile is repaired by write-if-absent.

The honest safety net that makes an explicit GC low-risk: if GC reclaims a blob
that an in-memory editor still holds (its tile is pinned in the pool), the tile is
not lost — a subsequent save re-encodes and re-writes it through the sink's
write-if-absent path, because the source of truth is the resident pool tile, not
the blob. So the cost of an over-aggressive GC against a *live* document is
re-compression on the next save, never data loss. This is *not* true for a blob
whose only reference is a *closed* document the caller failed to name — hence
Constraint 5's completeness requirement is about closed/unnamed documents
specifically. The distinction is stated in the doc 08 delta so a user understands
what an explicit "clean up" can and cannot cost them.

## Open questions

(none — all decided.)

## Status

**Done** — 2026-07-14.

- `src/serialize/arbc/serialize/asset_reaper.hpp`, `src/serialize/asset_reaper.cpp` — L4 `AssetReaper` seam + `AssetReaperError` + pure `unreferenced_tiles` set-diff
- `src/runtime/arbc/runtime/asset_gc.hpp`, `src/runtime/asset_gc.cpp` — L5 `FilesystemAssetReaper`, JSON mark walk `collect_referenced_tiles`, `sweep_tile_store`, `gc_project_directory`, `GcReport`/`GcError`
- `src/serialize/t/asset_reaper.t.cpp`, `src/runtime/t/filesystem_asset_reaper.t.cpp`, `tests/asset_gc.t.cpp` — unit and integration tests; 6 new claims (`08-serialization#asset-gc-{deletes-only-unreferenced-tiles,is-explicit-never-on-save,unions-references-across-documents,leaves-non-tile-assets,fails-safe-deletes-nothing,is-idempotent}`)
- `src/serialize/CMakeLists.txt`, `src/runtime/CMakeLists.txt`, `tests/CMakeLists.txt` — build wiring
- `tests/claims/registry.tsv` — 6 claim rows registered
- `docs/design/00-overview.md`, `docs/design/08-serialization.md`, `docs/design/17-internal-components.md` — design-doc deltas (Decision 1 doc-00 bullet, `AssetReaper` seam in doc 08 and doc 17)
- Two parking-lot entries appended: cross-process concurrent-editor coordination (host policy) and user-facing "clean up" UI/CLI affordance (host/packaging)
