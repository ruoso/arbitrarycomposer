# kinds.raster_tilewise_load — Stream level-0 blobs into pool tiles without a dense `DecodedImage` buffer

## TaskJuggler entry

[`tasks/55-kinds.tji:66-71`](../../55-kinds.tji):

> task raster_tilewise_load "Stream level-0 blobs into pool tiles without a
> dense DecodedImage buffer" — effort 2d, `depends !raster_pool_backing,
> serialize.raster_tile_store`.
>
> "Add `RasterStore::build_from_tiles` so a load streams level-0 blobs
> straight into pool blobs instead of materializing a dense `w*h` working
> buffer through `DecodedImage` (the load route `serialize.raster_tile_store`
> ships with, Decision 6). Bounds load peak RSS to O(tile) rather than
> O(image) — 384 MB transient per 24 MP layer today. Source-of-debt:
> `tasks/refinements/serialize/raster_tile_store.md` (Deferred follow-ups),
> commit `serialize.raster_tile_store`. Docs 08/14/15."

Milestone: `m9_release` (inherited from its source-of-debt task).

## Effort estimate

**2d**, apportioned:

| Slice | Days |
| --- | --- |
| Split `build_levels` into a level-0 fill and a shared higher-level pyramid loop; add `RasterStore::build_from_tiles` + `RasterContent::from_tiles` | 0.5 |
| Rewrite `deserialize_raster`'s decode loop onto the new seam (delete the dense buffer and the scatter) | 0.5 |
| Tests: the tile-bounded-peak proof, the fill-liveness and partial-build-teardown unit tests, the padding-fidelity case, and the regression sweep across the existing raster/tile-store suites | 1.0 |

The estimate is dominated by the **proof**, not the code. Deleting the dense
buffer is a small, mostly-subtractive change; showing that the peak is
genuinely tile-bounded — deterministically, without a wall-clock or an RSS
sample — is the work.

## Inherited dependencies

### Settled

- **`kinds.raster_pool_backing`** ([refinement](raster_pool_backing.md), done
  2026-07-10) — tile pixels are pool blobs. It handed forward exactly the
  primitives a streaming build needs: `new_blob(pool, edge)`
  ([`raster_content.cpp:81-85`](../../../src/kind_raster/raster_content.cpp)) allocates
  and zeroes one `edge·edge·4·sizeof(float)` blob and returns an owning
  `BlockRef`; `build_levels`' `keep` vector holds those `BlockRef`s until the
  `TileTable` ctor retains every `BlockSlotRef`
  ([`raster_content.hpp:81`](../../../src/kind_raster/arbc/kind_raster/raster_content.hpp)),
  after which the table holds the sole count. That RAII shape is what makes an
  **abandoned** partial build safe (see Constraint 5) — a fill that declines at
  tile *k* drops *k* `BlockRef`s and the pool reclaims them, with no `TileTable`
  ever constructed. It also handed forward the pool inspection counters
  (`blobs_allocated()` at `raster_content.hpp:189`, `RasterStore::pool()` at
  `:196-197`) that the doc-15 proofs are written against.
- **`serialize.raster_tile_store`** ([refinement](../serialize/raster_tile_store.md),
  done 2026-07-14) — the persisted tile store and the `org.arbc.raster` codec.
  It handed forward: `TileGeometry` + `validate_tile_geometry`
  ([`tile_blob.hpp:68-92`](../../../src/serialize/arbc/serialize/tile_blob.hpp)),
  `decode_tile_blob(frame, expected_hash, storage, samples) -> expected<std::vector<float>, TileBlobError>`
  (`tile_blob.hpp:149`) which returns **exactly one tile's** working-RGBA32F
  samples, the `RasterTileStore` hash memo (`raster_tile_store.hpp:56-96`), and
  the load route this task rewrites
  ([`codec_raster.cpp:179-323`](../../../src/runtime/codec_raster.cpp)). It also
  handed forward the debt, knowingly, in its Decision 6 — quoted under *Why*
  below — and Decision 7, **"tile blobs load synchronously; there is no pending
  state"**, which bounds this task's solution space (see Decision 4).
- **`kinds.raster`** ([refinement](raster.md), done 2026-07-06) — the tile
  table, `k_default_tile_edge = 256`, the working-linear premultiplied RGBA32F
  blob format, and the byte-exact mip goldens
  (`src/kind_raster/t/raster_goldens.t.cpp`) this task must not move.

### Pending

None. Every seam this task needs is already in the tree.

### Downstream (this task unblocks)

Nothing in the WBS `depends` on it. It is a debt-repayment leaf: it makes the
load route of a real-scale painting (a 24 MP layer) affordable, which is a
precondition for shipping `m9_release` credibly, not for compiling anything.

## What this task is

`serialize.raster_tile_store` gave `org.arbc.raster` a content-addressed,
per-tile on-disk form — and then read it back through the one construction path
`RasterContent` had: a dense `DecodedImage`. The loader decodes each level-0
blob, scatters it into a `w × h × 4 × float` working buffer, and hands that
buffer to `RasterContent(DecodedImage, edge)`, which immediately re-tiles it
back into pool blobs. The dense buffer exists for the length of one function
and is 384 MB for a 24 MP layer.

This task cuts the seam that makes it unnecessary. `RasterStore` grows a
`build_from_tiles` overload whose level-0 tiles are filled by a caller-supplied
callback, one tile at a time, directly into the pool blob's own memory. The
codec's decode loop becomes that callback: fetch blob *t*, decompress and verify
it, write it into the span it was handed, drop the frame, move to tile *t+1*.
The dense buffer and the scatter loop are deleted. The pyramid above level 0 is
built by the **same** decimation code as before, unmoved, so every mip rung of a
loaded document stays byte-identical.

Nothing else about the load changes: same geometry validation, same
`ReaderError` kinds, same hash verification, same synchronous semantics, same
memo seeding, same tile ordering.

## Why it needs to be done

The tile store's own reason for existing is that flattening a sparse,
shared, copy-on-write tile table into a dense image throws away exactly the
sparsity that makes it small (doc 08 Principle 8, quoted in Constraint 1). The
load route as shipped does precisely that, transiently, on the way in. From
`serialize.raster_tile_store`'s Decision 6:

> The honest cost: a transient dense `w × h × 16` buffer — 384 MB for a 24 MP
> layer — at load. That is a real regression against the sparsity this task is
> otherwise defending, and it is why `kinds.raster_tilewise_load` is a named
> follow-up rather than a shrug. It is deferred, not dismissed: correctness
> through the proven path first, then the streaming seam.

The cost is not merely a number. A dense buffer sized by the *image* means the
peak footprint of opening a document scales with the largest layer in it,
independently of how much of that layer is actually distinct — a document whose
24 MP layer is 90% one flat color still pays 384 MB to open, on top of the tile
table it is legitimately building. On a machine that can hold the document it
cannot necessarily hold the document *plus* a spare copy of its biggest layer,
and the failure mode is a `std::bad_alloc` while opening a file the user has
already successfully saved.

There is a second, quieter reason. Today's route zeroes each tile's
right/bottom padding on the way through the dense buffer
([`codec_raster.cpp:275`](../../../src/runtime/codec_raster.cpp), "dropping the
tile's right/bottom padding") and then **seeds the hash memo with the original
blob's name** (`:313-321`). For a blob whose padding bytes are not zero — a
foreign writer's, a future writer's — the memo would then assert that a pool
slot hashes to a name it no longer hashes to, and the next save would write that
stale name over different bytes. Our own saves always produce zero padding, so
the bug is latent rather than live; the tilewise route retires it structurally
by making the pool blob a faithful copy of the persisted blob (Decision 3).

## Inputs / context

### Design docs (normative, doc 16)

- **doc 08 § Principles → 8** ([`08-serialization.md:350-456`](../../../docs/design/08-serialization.md)) —
  the tile store. "The design rule is: **persist the tile table, not the
  image.** … Flattening that to a dense pixel buffer on save throws away
  exactly the sparsity and sharing that make it small, and then pays full price
  for both." Also: "**Mip levels are not persisted.** They are derived …
  **Rebuild on load.**" and "A blob whose bytes do not hash to the name they
  were fetched under is a load error, never silent wrong pixels."
- **doc 15 § What this asks of doc 14 and the kinds**
  ([`15-memory-model.md:355-370`](../../../docs/design/15-memory-model.md)) —
  "bulk pixel payloads go to the big-block pool with the tile *table* in
  slabs", plus **the delta this task lands** (see Decision 1): "**Reconstructing
  a tiled payload is tilewise.** … a load's *transient* peak is O(tile) … while
  its *resident* cost stays the O(image) tile table it is genuinely reading."
- **doc 15 § Memory populations** (`15-memory-model.md:16-24`) — painted raster
  tiles are "Bulk media data" (irreplaceable, refcounted from the owning nodes),
  **not** "Decoded source assets" (budgeted, LRU, re-derivable). No byte budget
  governs a raster load, and none should: the tile table *is* the document.
  `org.arbc.image`'s `PyramidCache` budget (`kinds.image_master_budget`) is the
  other population and is not a model for this task.
- **doc 17 § The component graph** (`17-internal-components.md:32-86`) and
  **§ The codec line** (`:252-262`) — `arbc::serialize` and `arbc::kind-raster`
  are **both L4** with **no same-level edges**, and the raster codec lives in
  `arbc::runtime` (L5), "beside the built-in codecs", precisely because L5 may
  name both. This is why `build_from_tiles` may be called from
  `src/runtime/codec_raster.cpp` with no levelization change — and why its
  signature may not name a single `serialize` type (Constraint 6).

### Source seams

- [`src/kind_raster/arbc/kind_raster/raster_content.hpp`](../../../src/kind_raster/arbc/kind_raster/raster_content.hpp)
  — `k_default_tile_edge` `:33`, `k_tile_channels` `:39`, `DecodedImage`
  `:45-50`, `Level` (`std::vector<BlockSlotRef> tiles`, row-major) `:64-70`,
  `TileTable` ctor `:81` / dtor `:89-97`, **`RasterStore::build(const DecodedImage&, int edge)` `:160`**
  (the sibling `build_from_tiles` joins), `intern(TileTablePtr)` `:207`,
  `RasterContent(DecodedImage, int tile_edge)` `:227` (the only construction
  path today), `store()` `:260-261`.
- [`src/kind_raster/raster_content.cpp`](../../../src/kind_raster/raster_content.cpp)
  — `blob_bytes(edge)` `:54-57`, `new_blob` `:81-85`, `put` `:87-95`,
  `decimate_parent` `:105-111` ("the ONE decimation kernel of the pyramid"),
  **`build_levels` `:117-182`** — level-0 loop `:121-150` (the part
  `build_from_tiles` replaces), higher-level pyramid loop **`:152-180`** (the
  part it must reuse *verbatim*), `RasterStore::build` `:270-284`,
  `RasterContent::RasterContent` `:478-481`.
- [`src/runtime/codec_raster.cpp`](../../../src/runtime/codec_raster.cpp) —
  `deserialize_raster` `:179-323`: geometry validation and `blobs`-length check
  `:205-220`, **the dense buffer `:225-237`** (whose comment already names this
  task), the per-tile fetch/verify/decode `:239-273`, **the scatter `:275-300`**
  (deleted), `RasterContent` construction `:303`, **memo seeding `:313-321`**
  (unchanged, and load-bearing on tile ordering). The save side
  (`serialize_raster` `:79-160`, level-0-only walk `:108-150`, `pool.peek(ref)`
  `:135`) is already tilewise and is the shape to mirror.
- [`src/serialize/arbc/serialize/tile_blob.hpp`](../../../src/serialize/arbc/serialize/tile_blob.hpp)
  — `TileGeometry` `:68-83` (`tile_count()` `:76`, `tile_samples()` `:80`),
  `validate_tile_geometry` `:92`, `is_tile_hash` `:101`, `tile_blob_uri` `:109`,
  `decode_tile_blob` `:149`.
- [`src/runtime/arbc/runtime/raster_tile_store.hpp`](../../../src/runtime/arbc/runtime/raster_tile_store.hpp)
  — the hash memo: `seed` `:84`, `tiles_hashed` `:91`, `memoized` `:96`.

### Predecessor / sibling refinements

- [`tasks/refinements/serialize/raster_tile_store.md`](../serialize/raster_tile_store.md)
  — Decision 6 (the debt), Decision 7 (synchronous, no pending state),
  Decision 8 (`params` shape: flat row-major `blobs`), Decision 10 (no
  `check_levels.py` change; "if an implementer finds themselves editing
  `ALLOWED`, the split in Decision 2 has been violated").
- [`tasks/refinements/kinds/raster_pool_backing.md`](raster_pool_backing.md) —
  the `BlockSlotRef`-in-the-table / `BlockRef`-transient discipline, and the
  parking-lot note (2026-07-10) about `ALLOWED["kind_raster"] = {contract}`.
- [`tasks/refinements/kinds/raster.md`](raster.md) — the tile geometry, the
  conformance-suite fixture, and the mip-pyramid goldens.

## Constraints / requirements

1. **The pyramid above level 0 is built by the existing code, unmoved.**
   `build_levels`' higher-level loop
   ([`raster_content.cpp:152-180`](../../../src/kind_raster/raster_content.cpp)) is
   extracted to a helper and called by *both* `build` and `build_from_tiles`.
   Copy-pasting it is forbidden — the file's own comment (`:97-101`) records
   that "two divergent copies of a byte-exact filter is the defect this must not
   recreate", and the claims
   `14-data-model-and-editing#paint-mip-recompute-matches-full-rebuild` and
   `08-serialization#raster-mips-are-not-persisted` both rest on there being one
   decimation path.
2. **Level-0 tile ordering is row-major and identical to the `blobs` array.**
   `Level::tiles[ty * tiles_x + tx]` must correspond to `params.blobs[t]` for
   the same `t`, because the memo seeding at
   [`codec_raster.cpp:313-321`](../../../src/runtime/codec_raster.cpp) pairs
   `level0.tiles[t]` with `(*bit)[t]` positionally. The fill callback is indexed
   by that same flat `t`.
3. **Every observable byte stays put.** `tests/raster_tile_store_golden.t.cpp`
   (byte-exact round-trip, hash-name golden), `src/kind_raster/t/raster_goldens.t.cpp`
   (mip pyramid + render goldens), and `tests/pull_multitile_golden.t.cpp` pass
   **with their frozen tables unedited**. If a golden moves, the change is
   wrong.
4. **Every load failure stays the failure it was.** The `ReaderError` kinds and
   pointers `tests/raster_tile_store_hostile.t.cpp` pins do not change:
   `MalformedField` at `/params/blobs` for a bad geometry, a wrong-length
   `blobs` array, a non-string or non-hash entry, or a blob that fails its hash;
   `UnresolvableReference` at `/params/blobs` for a blob that does not arrive.
   Geometry is still validated **before** any allocation
   (`08-serialization#raster-tile-geometry-is-validated-before-allocation`) —
   `validate_tile_geometry` runs before `build_from_tiles` is entered, so an
   absurd `edge` still cannot provoke a large allocation.
5. **An abandoned build leaks nothing.** When a fill declines at tile *k*,
   `build_from_tiles` must return without constructing a `TileTable`, dropping
   the *k* owning `BlockRef`s it holds — the pool reclaims every slot. The
   `RasterStore` survives the failure and is reusable.
6. **`build_from_tiles` names no `serialize` type.** `kind_raster` is L4 and may
   depend only on `contract` (+ below); `serialize` is its *sibling*
   (doc 17:32-86, "no same-level edges"). The seam takes plain
   `std::span<float>` / `std::size_t` and a `std::function`; the decode
   (`decode_tile_blob`), the hash, and the `LoadContext` stay in
   `serialize`/`runtime` and hand `kind_raster` nothing but floats. No
   `scripts/check_levels.py` edit, no new `DEPENDS` edge.
7. **The load stays synchronous and single-threaded.** Streaming here buys
   *memory*, not *progressiveness*. `serialize.raster_tile_store`'s Decision 7
   stands: a tile blob that does not arrive inline is a `ReaderError`, and
   `RasterContent` grows no `install_asset` override. No pending state, no
   partial render, no worker dispatch (see the deferred follow-up).
8. **`RasterContent(DecodedImage, int)` stays.** `build_from_tiles` is additive.
   Every existing producer of a `DecodedImage` — the tests, `ci_plugins`, and
   any future decode-then-tile importer — keeps working unchanged.

## Acceptance criteria

- **A load's transient peak is bounded by one tile, and does not grow with the
  image.** New claim **`15-memory-model#raster-load-is-tilewise`** (registry
  entry + an `enforces:`-tagged test at `tests/raster_tilewise_load.t.cpp`),
  against the doc-15 delta this task lands. *(behavioral counter — never a
  wall-clock or an RSS sample.)* The test TU defines a counting global
  `operator new` / `operator delete` (it gets its own executable target, as
  every `tests/*.t.cpp` does) tracking live heap bytes and a high-water mark,
  and asserts across a load of the *same* document geometry at two tile counts —
  4×4 tiles and 16×16 tiles, a 16× image-area increase:
  - the **largest single heap allocation** on the load path does not exceed one
    tile blob (`blob_bytes(edge)`) plus a fixed slack, at either size — this is
    the assertion the dense buffer fails outright; and
  - the **transient high-water** (peak live bytes minus the bytes the pool has
    reserved through its `ChunkSource`, and minus the resident `TileTable`) is
    within the same fixed bound at both sizes — i.e. it is O(tile), invariant in
    tile count, not O(image).

  Implementation note: subtract the pool's reserved bytes explicitly rather than
  assuming `AnonymousChunkSource` bypasses `operator new` — the assertion must
  measure *transient* bytes, and the tile table itself is legitimately O(image).
- **The fill sees exactly one tile at a time.** A `kind_raster` unit test
  (`src/kind_raster/t/raster_tilewise_load.t.cpp`) drives `build_from_tiles`
  with an instrumented fill that increments a live-span counter on entry and
  decrements on exit, and asserts: max concurrent live spans == 1; the fill is
  invoked exactly `tiles_x * tiles_y` times; each `dst` span is exactly
  `edge * edge * 4` floats; and the flat index arrives in ascending row-major
  order. *(behavioral counter.)*
- **A tilewise build is byte-identical to a dense build.** Same unit test: build
  a `TileTable` from a `DecodedImage` via `build(...)`, build a second from the
  *same* pixels via `build_from_tiles(...)` with a fill that serves the
  equivalent tiles, and assert every rung of both pyramids — level 0 through the
  1×1 apex — compares equal pixel-for-pixel. *(golden-backed, by construction:
  it pins the two routes against each other, and `raster_goldens.t.cpp` pins the
  dense route against frozen tables.)*
- **An abandoned build releases every blob it allocated.** Same unit test: on
  one `RasterStore`, run a `build_from_tiles` whose fill declines at tile *k*
  (asserting the returned handle is absent), then run a *successful*
  `build_from_tiles` of the same geometry on the same store, and assert the
  pool's reserved bytes did not grow beyond a single image's worth — the
  abandoned build's slots were recycled, not stranded. *(behavioral counter;
  ASan/LSan-clean under the existing sanitizer lanes.)*
- **A blob's padding bytes survive the load.** A codec-level test: hand-craft a
  tile blob whose right/bottom padding samples are non-zero, load it, and assert
  (a) the rendered pixels are identical to the zero-padded blob's — padding is
  outside `width`/`height` and every read clamps, so it is unobservable in
  output — and (b) an immediate re-save reproduces **the same blob name**, which
  today's dense route would not (Decision 3, and the latent memo bug it
  retires). *(golden-backed: the blob name is the golden.)*
- **The regression envelope is green with no golden edited**:
  `tests/raster_tile_store_golden.t.cpp`, `tests/raster_tile_store_hostile.t.cpp`,
  `tests/raster_tile_store_concurrency.t.cpp`, `src/kind_raster/t/raster_goldens.t.cpp`,
  `src/kind_raster/t/raster_paint.t.cpp`, `src/kind_raster/t/raster_pool_backing.t.cpp`,
  `tests/raster_conformance.t.cpp` (the contract conformance suite — mandatory
  for a content kind, doc 16), `tests/raster_concurrency_stress.t.cpp`,
  `tests/pull_multitile_golden.t.cpp`, and the five
  `tests/fuzz/corpus/load_document/raster_*.arbc` seeds.
- **The `08-serialization#raster-mips-are-not-persisted` registry description is
  corrected.** It currently states that "a load rebuilds the pyramid through
  `RasterContent`'s existing `DecodedImage` constructor"; after this task the
  route is `build_from_tiles`, sharing the one decimation chain. Same claim,
  same enforcing test, same guarantee — the description text must not go stale
  (`tests/claims/registry.tsv:314`).
- **Diff coverage ≥ 90%** on changed lines (CI gate). The deleted scatter loop
  helps; the new fill path is covered by the tests above.
- **Deferred — one named follow-up** (closer registers it in the WBS, milestone
  `m9_release`):
  - **`serialize.tile_store_parallel_load`** (2d, area `serialize`) — dispatch
    the per-tile *decode* pipeline (fetch → decompress → unshuffle → verify
    hash) across the pool workers, mirroring the already-registered
    `serialize.tile_store_parallel_save`. The pool's `allocate` is
    writer-only, so the parallel form is decode-on-workers /
    write-into-pool-on-the-loading-thread, with a bounded queue that preserves
    the O(tile)-per-in-flight-tile bound this task establishes (peak becomes
    O(workers · tile), still independent of image size). This task ships the
    load single-threaded and correct first, exactly as the tile store shipped
    the save. Behavioral-counter and TSan coverage; no wall-clock assertion.

## Decisions

### 1. Land the memory rule in doc 15, and hang the claim on it. *(doc 15 delta)*

The `.tji` note promises "load peak RSS O(tile) rather than O(image)" — and no
design doc said so. Doc 08 Principle 8's anti-dense argument is about *save
size*; doc 15's populations table governs the *resident* form and says nothing
about the route that fills it. A claim needs a normative statement to enforce,
so this task adds the missing one to doc 15's "What this asks of doc 14 and the
kinds" list ([`15-memory-model.md:361-369`](../../../docs/design/15-memory-model.md)):
reconstructing a tiled payload is tilewise; transient peak is O(tile), resident
cost is the O(image) table it is genuinely reading.

It is an extension of doc 15's existing bulk-payload discipline to the *filling*
of the pool, not a new axis, so it takes no doc 00 decision-record bullet.

*Rejected — register the claim against doc 08 Principle 8.* The tile store's
sparsity argument is about what hits the disk. Load-time footprint is a memory
question and belongs in the memory doc, next to the pool that receives the
bytes.

*Rejected — ship the code with no claim, on the grounds that the peak is
"structurally obvious" once the buffer is deleted.* Structural obviousness is
what the dense buffer had too, in the other direction — it was a `std::vector`
whose size nobody asserted. A memory promise with no enforcing test is a
comment, and the next refactor that reaches for a scratch buffer will not
notice it.

### 2. The seam is a per-tile fill callback, not a materialized tile sequence.

```cpp
// arbc::kind_raster — no serialize type named; plain floats (Constraint 6).
// Fill level-0 tile `index` (flat, row-major) into `dst`, exactly
// edge*edge*4 working-linear premultiplied RGBA floats, pre-zeroed.
// Return false to abandon the build.
using TileFill = std::function<bool(std::size_t index, std::span<float> dst)>;

std::optional<StateHandle> RasterStore::build_from_tiles(
    int width, int height, int edge, const TileFill& fill);

// RasterContent — the construction path for a loaded raster.
static std::unique_ptr<RasterContent> RasterContent::from_tiles(
    int width, int height, int tile_edge, const TileFill& fill);  // nullptr if a fill declined
```

The callback is what makes the bound *structural* rather than merely intended:
`build_from_tiles` hands out one blob's memory at a time and the codec's frame
and decoded vector are scoped to a single invocation, so the O(tile) peak is a
consequence of the control flow, not of anyone's discipline. It also inverts the
dependency the right way — the *kind* owns the pool and the geometry, the
*codec* owns the bytes, and neither has to know the other's types.

`from_tiles` is a static factory because it is fallible and a constructor is
not. It must never return a partially-built `RasterContent`: on a declined fill
the object under construction is destroyed and `nullptr` is returned. (How the
failure is threaded out of the private constructor — an `ok` out-param, a
pre-checked handle — is the implementer's call; the invariant is that no
half-built content escapes.)

*Rejected — `build_from_tiles(std::span<const std::span<const float>> tiles)`,
i.e. hand the store all the tiles at once.* That is the dense buffer again with
extra steps: the caller must hold every decoded tile simultaneously, and the
peak is O(image) exactly as before.

*Rejected — a pull-style `TileSource` abstract base class instead of a
`std::function`.* One call site, one implementation, no polymorphism needed
across a stable boundary. The simpler abstraction wins (and a `std::function`
costs the kind nothing it isn't already paying — the fill runs once per tile,
against a megabyte of memcpy).

*Rejected — build the `RasterStore` standalone in the codec and move it into
`RasterContent`.* `RasterStore` is not movable and must not become so: it owns a
`BigBlockPool`, a mutex, and an `AnonymousChunkSource`, and every live
`TileTable` holds a raw `BigBlockPool*` (`raster_content.hpp:81`). Moving the
store would dangle every table it minted.

### 3. The blob's bytes land in the pool blob verbatim — padding included.

A level-0 tile blob is `edge · edge · 4` samples, padding and all; the save side
`peek()`s the whole pool blob (`codec_raster.cpp:135`), so the padding is
*inside* the hash. The fill therefore writes the decoded blob into `dst`
wholesale, and the pool blob becomes a faithful copy of the persisted one.

Today's route instead drops the padding on the way through the dense buffer and
re-zeroes it at re-tile time. For blobs our own writer produced this is a
distinction without a difference — `new_blob` zeroes, and `paint` never writes
outside `width`/`height`, so our padding is always zero. For a blob some *other*
writer produced it is not: the load would silently rewrite the tile while the
memo (`codec_raster.cpp:313-321`) went on asserting the *original* name for it,
and the next save would publish that name over different bytes. Copying verbatim
retires that latent inconsistency by construction.

The padding remains unobservable in output: every read clamps to
`[0, width-1] × [0, height-1]` (`level_pixel` / `decimate_parent`,
`raster_content.cpp:105-111`), so no render and no mip rung can see it. Hence no
golden moves — which the padding-fidelity acceptance test asserts directly
rather than assuming.

*Rejected — zero the padding after each fill, byte-matching today's route for
foreign blobs.* It preserves a behavior whose only effect is to make the memo
lie, and it costs a memset per tile to do so. If a future writer's padding is
ever meaningful, the verbatim copy is also the only route that keeps it.

### 4. Streaming buys memory, not progressiveness.

No pending state, no async fill, no partial-render-while-loading. This is
`serialize.raster_tile_store`'s Decision 7, unamended: painted pixels are
document state and the asset directory is a sibling of the `.arbc` by
construction, so "a raster layer whose tiles have not arrived is not a layer in
a pending state, it is a **broken document**". The `org.arbc.image` async /
`install_asset` machinery (`kinds.image_async_pending`) is for *imported* assets
with a plausible "not yet"; a raster's own tiles have none.

*Rejected — reuse `org.arbc.image`'s pending/`install_asset` path to fill tiles
as they arrive.* It would trade a bounded, decided failure (`ReaderError`) for a
document that renders holes and never explains why, and it would make the load
concurrent for no memory benefit — the peak is already one tile.

### 5. Single-threaded now; the parallel decode is a named follow-up.

The per-tile decode (decompress → unshuffle → verify) is a pure function of the
fetched frame, so parallelizing it is sound by construction — but the pool's
`allocate` is writer-only, so the parallel form needs a decode-on-workers /
write-on-the-loading-thread split with a bounded in-flight queue. That is a
different task with its own TSan surface, and it mirrors exactly the choice the
save side already made (`serialize.tile_store_parallel_save`, deferred from the
tile store for the same reason). Ship correct and sequential; the memory bound
this task establishes is what makes the bounded queue's peak analyzable later.

*Rejected — parallelize the decode here, since the fill callback is already a
per-tile unit.* It would put a TSan/stress obligation and a queue-depth policy
inside a 2d debt-repayment task, and the in-flight queue would relax the very
bound (`one tile live`) this task is trying to pin.

## Open questions

(none — all decided.)

## Status

**Done** — 2026-07-14.

- Added `RasterStore::build_from_tiles` and `RasterContent::from_tiles` static factory to `src/kind_raster/arbc/kind_raster/raster_content.hpp` and `src/kind_raster/raster_content.cpp`; extracted the shared pyramid-decimation helper so both `build` and `build_from_tiles` share the one decimation path verbatim.
- Rewrote `deserialize_raster` in `src/runtime/codec_raster.cpp` to use the `TileFill` callback; deleted the dense `w×h` `DecodedImage` buffer and the scatter loop; blob bytes (padding included) now land directly in pool tile memory.
- Added claim `15-memory-model#raster-load-is-tilewise` to `tests/claims/registry.tsv`; behavioural-counter enforcement test at `tests/raster_tilewise_load.t.cpp` (counting global `operator new`, 4×4 vs 16×16 grids, largest-single-allocation and transient-high-water assertions).
- Added unit tests at `src/kind_raster/t/raster_tilewise_load.t.cpp`: fill-liveness counter (max concurrent live spans == 1), tilewise-vs-dense byte-identity across every pyramid rung, abandoned-build slot reclaim, `from_tiles` nullptr-on-decline.
- Added padding-fidelity codec test in `tests/raster_tile_store_golden.t.cpp` (golden is the blob name); verifies non-zero padding survives the load verbatim and re-save reproduces the original hash, retiring the latent memo bug (Decision 3).
- Corrected the stale description of `08-serialization#raster-mips-are-not-persisted` in `tests/claims/registry.tsv` to reflect the new `build_from_tiles` load route.
- Updated doc-15 (`docs/design/15-memory-model.md`) with the normative "Reconstructing a tiled payload is tilewise" rule that the new claim enforces.
- Wired the new test target to `src/kind_raster/CMakeLists.txt` and `tests/CMakeLists.txt`; all 27 raster/tile-store tests green with no golden edited.
