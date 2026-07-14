# serialize.raster_tile_store — content-addressed raster tile store + `org.arbc.raster` codec

## TaskJuggler entry

[`tasks/60-serialize.tji:67-72`](../../60-serialize.tji) — `task raster_tile_store
"Content-addressed raster tile store + org.arbc.raster codec"`, `effort 5d`,
`depends !kind_params, !zstd_dep, kinds.raster_pool_backing`.

> Make a PAINTING SAVEABLE. Today `org.arbc.raster` has NO codec at all — the
> default table registers solid/tone/fade/crossfade/nested only
> (`document_serialize.cpp:159-163`) — so a painted layer does not round-trip
> and a user cannot save their work. Build the codec and the on-disk store per
> doc 08 Principle 8 and the new doc 08 "The asset directory" section. THE
> CENTRAL RULE, which must not be quietly violated by a simpler implementation:
> PERSIST THE TILE TABLE, NOT THE IMAGE. […] Deliverables: (a) tile blobs keyed
> by CONTENT HASH under `<doc>.assets/tiles/`; (b) INCREMENTAL SAVE as a
> consequence; (c) mip levels NOT persisted, rebuilt on load; (d) a
> document-carried STORAGE FORMAT distinct from the working format (default
> `rgba16f`); (e) per-blob zstd with a BYTE-SHUFFLE. Acceptance: byte-exact
> round-trip of a painted document; a re-save after one dab writes exactly the
> touched tiles' blobs and no others (a behavioral counter, not a wall-clock
> assertion); an all-empty layer stores ONE blob; a load rebuilds mips
> byte-identically. Docs 08/10/14.

Milestone: `m9_release` ([`tasks/99-milestones.tji:72`](../../99-milestones.tji)).

## Effort estimate

**5d**, apportioned:

| piece | days |
| --- | --- |
| SHA-256 content hash in `arbc::base` + NIST test vectors | 0.5 |
| Byte-shuffle + storage-format encode/decode + goldens | 0.5 |
| `SaveContext` / `AssetSink` write-side seam; widen `SerializeFn`; update the six existing codecs | 1.0 |
| `FilesystemAssetSink` (write-if-absent, temp+rename, counters) | 0.5 |
| `serialize` tile-blob encode/decode + untrusted-geometry validation | 0.5 |
| `runtime` `RasterTileStore` hash memo + `raster_codec` (serialize/deserialize) + mip-rebuild wiring | 1.0 |
| Claims, goldens, behavioral counters, loader-fuzz corpus, ≥90% diff coverage | 1.0 |

The day-count is dominated by the seam work, not the codec: the codec body is
small once the store exists, but the store needs a hash the tree does not have
and a write path the core does not have.

## Inherited dependencies

### Settled

- **`serialize.kind_params`** ([refinement](kind_params.md), done 2026-07-09) —
  handed forward the codec seam this task registers into: `SerializeFn` /
  `DeserializeFn` / `Codec` / `CodecTable` in
  [`src/serialize/arbc/serialize/codec.hpp:40-70`](../../../src/serialize/arbc/serialize/codec.hpp),
  the `content_body_to_json` / `content_body_from_json` routing
  ([`codec.hpp:109-126`](../../../src/serialize/arbc/serialize/codec.hpp)), and
  the `PlaceholderContent` miss path. Its Decision 3 is the levelization rule
  this task obeys: **a concrete kind codec is registered from L5 (`runtime`) or
  a plugin TU, never from L4** — `serialize` and `kind_raster` are both L4, so
  neither may include the other.
- **`serialize.zstd_dep`** ([refinement](zstd_dep.md), done 2026-07-14) — handed
  forward
  [`src/serialize/arbc/serialize/blob_compress.hpp:73,90-91`](../../../src/serialize/arbc/serialize/blob_compress.hpp):
  `compress_blob(std::span<const std::byte>) -> expected<std::vector<std::byte>,
  BlobCompressError>` and `decompress_blob(frame, expected_size) ->
  expected<std::vector<std::byte>, BlobCompressError>`, stateless (one-shot zstd
  API, safe from any thread), with the decompression bound taken from the
  **caller's** declared size and never from the frame header. It explicitly
  deferred to *this* task: the byte-shuffle, the content-addressed store, the
  hashing, incremental save, mip rebuild, the storage-format document field, and
  the `org.arbc.raster` codec ([`zstd_dep.md` Constraint 10](zstd_dep.md)).
  Its header comment names this task by id and prescribes the composition:
  *"shuffle -> compress_blob -> hash-named file"*.
- **`kinds.raster_pool_backing`** ([refinement](../kinds/raster_pool_backing.md),
  done 2026-07-10) — handed forward the tile store this task reads:
  `Level::tiles` is `std::vector<BlockSlotRef>`
  ([`src/kind_raster/arbc/kind_raster/raster_content.hpp:64-70`](../../../src/kind_raster/arbc/kind_raster/raster_content.hpp)),
  `TileTable` retains every slot it names for its whole lifetime
  ([`raster_content.hpp:79-127`](../../../src/kind_raster/arbc/kind_raster/raster_content.hpp)),
  and reads go through `BigBlockPool::peek(BlockSlotRef) -> std::span<const
  std::byte>` — zero-refcount, any-thread. **Untouched tiles keep their
  `BlockSlotRef` identity across a CoW paint**, which is the property the
  incremental-save hash memo is built on.

### Pending

(none — every predecessor is `complete 100`.)

### Downstream (this task unblocks)

Nothing in the WBS `depends` on `raster_tile_store` today; it is a leaf of
`m9_release`. But it is the task that makes a painting saveable at all, so the
whole `kind_raster` line (`kinds.raster_brush_dab`,
`kinds.raster_workspace_backing`) is only useful once this lands. Note the
deliberate split, stated at [`tasks/55-kinds.tji:69`](../../55-kinds.tji): the
`.arbc` tile store is **save** (interchange, archival, an explicit user action);
`kinds.raster_workspace_backing` is **recovery** (a same-machine session
artifact, doc 08 §Shape). They are different artifacts and this task does not
touch the workspace file.

## What this task is

Give `org.arbc.raster` a serialize codec and give the core the on-disk store that
codec writes into. A painted layer's pixels are document state with no source
file (doc 08 Principle 8), so they cannot serialize as a URI the way an imported
`org.arbc.image` does — they have to *be* in the document's asset directory. The
store persists **the tile table, not the image**: each distinct level-0 tile is
converted to the document's storage format, hashed, byte-shuffled, zstd-framed,
and written once under its content hash at `assets/tiles/<hh>/<hash>`; the
layer's `params` carry the row-major array of hashes. Identical tiles — the empty
ones, the flat ones — collapse to a single blob, dedup falls out across layers
and across undo versions, and **incremental save falls out as a consequence**: a
save writes only the blobs not already on disk under that name. Mip levels are
not persisted; a load rebuilds them through `RasterContent`'s existing
constructor, which is already claim-proven byte-identical to the incremental
recompute.

It does **not** build the workspace file (that is
`kinds.raster_workspace_backing`), it does **not** garbage-collect orphaned
blobs, and it does **not** parallelize the per-tile encode pipeline — all three
are named, deferred follow-ups below.

## Why it needs to be done

`builtin_codecs()`
([`src/runtime/document_serialize.cpp:158-168`](../../../src/runtime/document_serialize.cpp))
registers solid, tone, fade, crossfade and nested. There is no raster row. A
document containing a painted layer therefore routes through the codec-miss path
and comes back as `PlaceholderContent` — the `kind` / `kind_version` / `params` /
`inputs` frame round-trips verbatim and the layer renders as the diagnostic
half-coverage magenta, but **the pixels are gone**. That is the whole of the
user's work. Every other deliverable in the raster line — brush dabs, resampling
quality, workspace recovery — is building on a layer that cannot be saved.

The predecessors have deliberately staged the pieces: `zstd_dep` landed the
compressor and stopped exactly at the seam, `raster_pool_backing` landed the pool
backing whose `peek()` span is the byte source, and `kind_params` landed the
codec table with a raster-shaped hole in it. This task fills the hole.

## Inputs / context

### Design docs (normative, doc 16)

- **[`docs/design/08-serialization.md:21-46`](../../../docs/design/08-serialization.md)
  — §The asset directory.** *"A document is a `.arbc` file plus a sibling asset
  directory, not a single container"* (L23-24), laid out as `project.arbc` +
  `project.assets/tiles/…` (L26-32). *"Everything inside is addressed by relative
  URI and resolved through the same `LoadContext` asset hook that external nested
  projects use (Principle 3), so one resolution seam serves both and a project
  directory stays relocatable"* (L34-36). *"A directory rather than a single file
  is the deliberate choice. It is what makes content-addressed blobs work as
  files — an incremental save writes only the new tiles and touches nothing else,
  which a monolithic container cannot do without rewriting itself… nothing in the
  format may come to depend on single-file-ness"* (L38-46).
- **[`docs/design/08-serialization.md:331-408`](../../../docs/design/08-serialization.md)
  — Principle 8.** *"Persist the tile table, not the image"* (L338-343). Blobs
  keyed by content hash, each distinct tile written once, dedup across layers and
  undo versions, saves incremental (L345-351). **The hash is over the tile's
  uncompressed bytes in the storage format, not over the compressed blob**
  (L353-361) — *"Compression is a storage encoding, never content identity"*,
  which is what keeps the store decoupled from the compressor version and makes a
  blob self-verifying. Mips not persisted, rebuilt on load (L362-365). Storage
  format document-carried, `rgba16f` default, *"authored, not inferred"*
  (L366-371). Per-blob zstd with a byte-shuffle, *"the shuffle is ours, not the
  library's"* (L372-376). The size-lever table (L378-400): 16.11 GB → 32 MB, with
  compression the **weakest** lever at 2.9x, below dedup's 4.3x.
- **[`docs/design/08-serialization.md:176-191`](../../../docs/design/08-serialization.md)
  — Principle 3.** *"The core fetches asset bytes; the kind only decodes them…
  An asset-referencing kind never performs file I/O of its own."* This is the
  rule that forbids the raster codec from opening files itself, and therefore the
  rule that forces the new write-side seam (Decision 1).
- **[`docs/design/08-serialization.md:425-455`](../../../docs/design/08-serialization.md)
  — §Dependency note.** *"bounded decompression of untrusted input — the output
  size comes from the tile geometry the document declares, never from the frame
  header, which on a hostile file is attacker-controlled"* (L440-442). Note the
  consequence this task must close: once the geometry itself is
  attacker-controlled, **the geometry must be validated before it is used as an
  allocation bound** (Constraint 7).
- **[`docs/design/10-tooling-and-packaging.md:19-35`](../../../docs/design/10-tooling-and-packaging.md)
  — §Dependency policy.** The dep table is a closed list of two core
  dependencies (JSON, zstd). The zstd row (L27) closes with *"it is worth exactly
  one small, well-vetted dependency and no more"*. This is the constraint that
  rules out taking a hash library (Decision 3).
- **[`docs/design/14-data-model-and-editing.md:197-211`](../../../docs/design/14-data-model-and-editing.md).**
  *"`org.arbc.raster`'s state is a persistent tile table — a paint stroke copies
  only touched tiles"*; and the mip paragraph behind the claim
  `14-data-model-and-editing#paint-mip-recompute-matches-full-rebuild`, already
  enforced at
  [`src/kind_raster/t/raster_paint.t.cpp:246`](../../../src/kind_raster/t/raster_paint.t.cpp).
  Rebuilding mips on load is *sound because that claim already holds*.
- **[`docs/design/17-internal-components.md:69-70`](../../../docs/design/17-internal-components.md).**
  `arbc::serialize` is L4 with edges `{contract, model}` (+ JSON, zstd);
  `arbc::kind-*` are L4 with edge `{contract}`. **No same-level edges** (L52-56).
  `arbc::runtime` is L5 and depends on *"everything below"* (L71) — it is the one
  place that can see both a concrete kind and the JSON library, which is why the
  codec TU lives there ([`17:142-149`](../../../docs/design/17-internal-components.md)).
- **[`docs/design/16-sdlc-and-quality.md:46-62`](../../../docs/design/16-sdlc-and-quality.md).**
  Tier 3 byte-exact goldens (*"tolerances are the exception"*); tier 4 behavioral
  counters (*"Wall-clock tests lie in CI; counters don't"* — where the
  incremental-save claim lands). Tier 8 loader fuzzing (L79-81). Diff coverage is
  a hard ≥90% gate (L112-118).

### Source seams

- **[`src/kind_raster/arbc/kind_raster/raster_content.hpp`](../../../src/kind_raster/arbc/kind_raster/raster_content.hpp)** —
  `k_default_tile_edge = 256` (L33), `k_tile_channels = 4` (L39); `DecodedImage`
  (L45-50) is the kind's only construction input; `Level{width, height, tiles_x,
  tiles_y, std::vector<BlockSlotRef> tiles}` (L64-70); `TileTable` with
  `level(l)`, `levels()`, `level_pixels(l)`, `byte_cost()` (L79-127);
  `RasterStore::build(const DecodedImage&, int edge) -> StateHandle` (L149-220);
  `RasterContent(DecodedImage, int tile_edge = k_default_tile_edge)` (L225),
  `kind_id = "org.arbc.raster"`, `capture()`/`restore()` (the `Editable` facet),
  `base_handle()`, `store()`.
  `blob_bytes(edge) = edge*edge*4*sizeof(float)`
  ([`raster_content.cpp:54-56`](../../../src/kind_raster/raster_content.cpp)) —
  exactly **1 MiB** in-memory `rgba32f` at edge 256, **512 KiB** at the `rgba16f`
  storage default. `RasterContent` overrides **neither** `external_asset_ref()`
  nor `install_asset()` — painted pixels are not an imported asset, and this task
  must not make them one.
- **[`src/pool/arbc/pool/big_block_pool.hpp:190-196`](../../../src/pool/arbc/pool/big_block_pool.hpp)** —
  `std::span<const std::byte> peek(BlockSlotRef) const noexcept`, *"Zero-refcount-traffic
  read… Valid only while the target is kept alive by a count the caller holds.
  Any thread."* This is the byte source the encode pipeline consumes.
- **[`src/serialize/arbc/serialize/codec.hpp:40-70`](../../../src/serialize/arbc/serialize/codec.hpp)** —
  `using SerializeFn = std::function<expected<nlohmann::json, SerializeError>(const Content&)>;`
  **Note the signature: no context, no output directory, no asset sink.** This is
  the seam Decision 1 widens.
- **[`src/serialize/arbc/serialize/load_context.hpp:31-45,62-90`](../../../src/serialize/arbc/serialize/load_context.hpp)** —
  `class AssetSource { virtual void request(std::string_view resolved_uri,
  std::function<void(std::string_view bytes)> on_ready) = 0; }`, and `LoadContext`
  with `resolve()`, `resolved_uri()`, `base_uri()`, `set_asset_source()`,
  `load_asset()`. The read half exists in full. **There is no write half** —
  a tree-wide grep for `AssetSink|SaveContext|write_asset|save_asset` returns
  nothing, and `serialize_document(...)` returns
  `expected<std::string, SerializeError>` having touched no filesystem.
- **[`src/runtime/document_serialize.cpp:158-179`](../../../src/runtime/document_serialize.cpp)** —
  `builtin_codecs()` and `builtin_codecs(const Registry&)`; the raster row is
  absent. `capture_snapshot()` (L183-323, writer-thread only) and
  `serialize_snapshot()` (L325-351) are the save path; `save_document()`
  (L353-361) the entry point.
- **[`src/runtime/codec_nested.cpp:74,103,137-139`](../../../src/runtime/codec_nested.cpp)** and
  **[`src/runtime/codec_image.cpp:87-94,134-181`](../../../src/runtime/codec_image.cpp)** —
  the two codec idioms to copy: params read via `params.find(key)` + `is_string()`
  (a present-but-mistyped key is treated **leniently as absent** and rides the
  `params_residual` diff back out verbatim); errors as values via
  `read_fail(ReaderError::Kind::…, "/params/…")` with a JSON-pointer path;
  collaborators bound **by closure** (`nested_codec(loader)`,
  `image_codec(registry, loader)`), never by widening `DeserializeFn`.
- **[`src/media/arbc/media/pixel_traits.hpp:62-68,174-190`](../../../src/media/arbc/media/pixel_traits.hpp)** —
  the storage-format converter already exists and was built for exactly this:
  *"Storage is `std::uint16_t`; conversion is branch-light bit manipulation with
  round-to-nearest-even, no reliance on `_Float16`/`std::float16_t` so the three
  target compilers agree bit-for-bit (doc 16 byte-exact discipline)."*
  `PixelTraits<PixelFormat::Rgba16fLinearPremul>::encode/decode` is the encode
  primitive; `bytes_per_pixel()` / `channels_per_pixel()` are in
  [`pixel_format.hpp:22-36`](../../../src/media/arbc/media/pixel_format.hpp).
  `media` is L1 and sits in `serialize`'s transitive closure via `contract`.
- **[`src/base/arbc/base/hash_mix.hpp:29-39`](../../../src/base/arbc/base/hash_mix.hpp)** —
  `mix64` is a splitmix64 finalizer and says so: *"It is NOT a cryptographic hash
  and makes no claim to be."* A grep for `sha|blake|xxhash|md5|crc32` across
  `src/` returns **nothing**. There is no content-hash-grade function in the
  tree; introducing one is this task's job (Decision 3).
- **[`scripts/check_levels.py:44,47,51-54`](../../../scripts/check_levels.py)** —
  `serialize: {contract, model}`, `kind_raster: {contract}`, and `runtime`
  already lists **both** `kind_raster` and `serialize`. **No `check_levels.py`
  change is needed**; the codec TU's placement is legal as-is.

### Predecessor / sibling refinements

- [`kind_params.md`](kind_params.md) Decision 3 — codecs register from L5.
- [`zstd_dep.md`](zstd_dep.md) Decision 1 (hash over uncompressed bytes),
  Decision 6 (`decompress_blob` allocates the *caller's* bound, never the frame
  header's), Constraint 6 (one-shot zstd, no shared `ZSTD_CCtx` — a shared
  context would be a data race).
- [`../kinds/raster_pool_backing.md`](../kinds/raster_pool_backing.md) Decision 4
  — retain/release run on the writer/drain thread, never on an RT render worker.
- [`unknown_field_preservation.md`](unknown_field_preservation.md) Constraint 7 —
  *"`serialize.compositions_table` extends the key sets, not the mechanism."* The
  same applies here: the raster `params` keys join the known-key set; the
  `params_residual` stash mechanism is untouched.

## Constraints / requirements

1. **Persist the tile table, not the image.** The save path must never
   materialize a dense pixel buffer of the layer and write that. It walks
   `TileTable::level(0)`'s `BlockSlotRef` grid, `peek()`s each blob, and encodes
   each tile independently. A dense flatten is the failure mode doc 08 Principle 8
   exists to forbid, and it would silently pass a naive round-trip test — so the
   content-addressing counters (Acceptance) are the guard, not the round-trip.

2. **The blob name is the hash of the *uncompressed* bytes in the *storage*
   format.** Not the working bytes, not the compressed frame. Compression and the
   byte-shuffle are a storage encoding *inside* the blob; they are never content
   identity (doc 08 L353-361). Consequences the implementation must preserve: a
   zstd version or level change must not rename a single blob, and a blob is
   self-verifying — decompress, unshuffle, hash, compare against the name it was
   fetched under.

3. **Levelization (doc 17, CI-enforced).** `serialize` (L4) may **not** include
   `kind_raster` (L4); `kind_raster` may **not** include `serialize`. So the split
   is: `arbc::serialize` owns the **format** (hash, shuffle, storage-format
   conversion, blob encode/decode, the `AssetSink` seam) over a byte-oriented API
   — `std::span<const std::byte>` in, `std::span<const std::byte>` out, naming no
   raster type; `arbc::runtime` (L5) owns the **codec** (`codec_raster.cpp`),
   which is the only place that sees both `RasterContent` and `nlohmann::json`.
   `scripts/check_levels.py` already permits both edges; **do not widen any
   `ALLOWED` entry.** `src/runtime/CMakeLists.txt` gains `kind_raster` in
   `DEPENDS` (runtime's doc-17 row is already "everything below").

4. **The core writes asset bytes; the codec only encodes them.** Doc 08
   Principle 3's read-side rule (*"An asset-referencing kind never performs file
   I/O of its own"*) applies symmetrically to the write side. `codec_raster.cpp`
   must not open, create, or rename a file. It hands finished blob frames to an
   `AssetSink` supplied by the caller (Decision 1).

5. **A save that would drop pixels is an error, never a silent success.** If a
   document contains an `org.arbc.raster` layer and no `AssetSink` is installed on
   the `SaveContext`, `save_document` fails with a new
   `SerializeError::Kind::AssetSinkMissing`. Existing sink-less call sites keep
   working because a document with no raster layer never consults the sink.

6. **Blob writes are write-if-absent and crash-atomic.** `AssetSink::put` skips a
   name already present (content-addressing makes presence-by-name sufficient) and
   reports whether it wrote. `FilesystemAssetSink` writes to a temporary name in
   the target directory and `rename()`s into place, so a crashed or
   out-of-disk save can never leave a truncated blob under a valid hash name —
   which would otherwise poison every future save's write-if-absent check.
   **A save never deletes a blob** (see the deferred `serialize.asset_gc`): another
   document version, another `.arbc`, or a concurrent editor may reference it.

7. **Untrusted geometry is validated before it is used as an allocation bound.**
   `zstd_dep` bounded decompression by the caller's `expected_size`; that
   `expected_size` is now derived from `edge`, `width` and `height` read out of a
   possibly-hostile document. The reader must therefore reject, as a value and
   before any allocation sized by them: a non-positive or absurd `edge` (bound it
   to a sane maximum and require `edge` to be a power of two matching the kind's
   tiling); `width`/`height` whose implied `tiles_x * tiles_y` overflows or
   exceeds a sane cap; a `blobs` array whose length is not exactly
   `tiles_x * tiles_y`; and a malformed hash string. This is the extension of doc
   08 L440-442's promise from the frame header to the tile table.

8. **Mip levels are neither written nor read.** The `params` carry level 0 only.
   Load reconstructs the pyramid through the *existing* `RasterContent` /
   `RasterStore::build` path, which is what makes the rebuild byte-identical for
   free (`14-data-model-and-editing#paint-mip-recompute-matches-full-rebuild`).
   No mip blob may appear in the asset directory — asserted, not assumed.

9. **The hash memo must be sound against pool slot reuse.** `BlockSlotRef` is
   `{uint32 index, uint32 size}` in release builds — the generation tag is
   `#ifndef NDEBUG`-gated
   ([`big_block_pool.hpp:49-77,377-379`](../../../src/pool/arbc/pool/big_block_pool.hpp)) —
   and `SlotStore::allocate` reuses the most-recently-released slot first
   ([`src/pool/slot_store.cpp:190-206`](../../../src/pool/slot_store.cpp)). So a
   released-and-recycled slot produces a `BlockSlotRef` **bit-identical** to the
   stale one, and neither `peek()` nor `count()` can tell them apart. A memo keyed
   on `BlockSlotRef` alone would hand back a stale hash for entirely different
   pixels. The refcount is the only validity token (Decision 5).

10. **Concurrency.** The save runs off the editing thread on a pinned snapshot
    (doc 08: autosave never pauses editing). Every tile the save touches is
    already kept alive by the pinned `TileTablePtr`, which holds a retain on
    every slot it names — so the memo's own `retain` is always "add a count to
    something already ≥ 1" and can never resurrect a dead slot. `peek()` is
    any-thread; `compress_blob` is stateless. `BigBlockPool::allocate` is
    writer-only and the save never allocates from the pool. TSan coverage is
    scoped in Acceptance.

11. **Canonical form and unknown fields are untouched.** The raster `params` keys
    join the known-key set; the `params_residual` preservation mechanism is not
    modified (`unknown_field_preservation` Constraint 7). Output stays canonical
    (sorted keys, shortest round-trip decimals).

## Acceptance criteria

- **Content-addressing is real, not incidental** *(behavioral counter)*. New claim
  **`08-serialization#raster-tiles-persist-as-a-content-addressed-store`**: saving
  a layer whose tiles are all identical (an untouched, all-zero raster) writes
  **exactly one** blob regardless of tile count; two layers sharing a tile's exact
  pixels write that blob **once**. Asserted on `AssetSink::blobs_written()`, not on
  directory size or wall-clock. This is the counter that a dense-flatten
  implementation cannot pass.

- **Incremental save** *(behavioral counter, never wall-clock)*. New claim
  **`08-serialization#raster-save-is-incremental`**: save a painted document; apply
  one dab touching tile set *T*; re-save into the same asset directory. Exactly
  `|T|` blobs are written and exactly `|T|` tiles are hashed — asserted on
  `blobs_written()` and `RasterTileStore::tiles_hashed()`. The hash counter is the
  stronger of the two: write-if-absent alone would give the blob count while still
  re-hashing the whole document every save, which is the difference between
  gesture-cadence autosave and a lie.

- **Byte-exact round-trip** *(golden-backed)*. New claim
  **`08-serialization#raster-tile-store-round-trips-byte-exactly`**: with
  `storage_format: rgba32f`, `load(save(D))` yields a `TileTable` whose every
  level-0 tile is bit-identical to `D`'s, and re-serializing yields byte-identical
  JSON. `tests/raster_tile_store_golden.t.cpp` pins, as inline literals: the
  canonical `.arbc` bytes of a small painted document, and the **exact set of blob
  hash names** it emits. It must **not** golden the compressed frame bytes — doc 08
  L353-361 explicitly reserves the right of a different zstd version or level to
  emit different bytes for the same tile, so a frame-byte golden would encode a
  promise the format does not make. The byte-shuffle *is* ours and version-stable,
  so `shuffle`/`unshuffle` get their own byte-exact golden as a pure function.

- **The `rgba16f` default quantizes once and is then a fixed point** *(golden-backed)*.
  New claim **`08-serialization#raster-storage-quantization-is-idempotent`**: at the
  default `storage_format: rgba16f`, the first save quantizes f32→f16 (lossy, by
  the user's authored choice); thereafter `save → load → save` produces an
  **identical** hash set and writes **zero** new blobs, because `f16_to_float`
  followed by `float_to_f16` is exact. Stating this honestly matters — "byte-exact
  round-trip" is unqualified only at `rgba32f`.

- **Mips are not persisted and rebuild byte-identically** *(golden-backed)*. New
  claim **`08-serialization#raster-mips-are-not-persisted`**: no blob is written for
  any level above 0 (asserted by count, and by the emitted name set); and a loaded
  document's every mip rung is byte-identical to the pre-save document's. This
  leans on the already-enforced
  `14-data-model-and-editing#paint-mip-recompute-matches-full-rebuild`
  ([`src/kind_raster/t/raster_paint.t.cpp:246`](../../../src/kind_raster/t/raster_paint.t.cpp)) —
  the new test pins that the *save/load* path actually routes through it.

- **A blob verifies against its name; a corrupt one fails as a value**
  *(behavioral)*. New claim **`08-serialization#tile-blob-verifies-against-its-name`**:
  decoding hashes the decompressed storage bytes and compares against the name the
  blob was fetched under; a mismatch — a truncated file, a bit-flipped frame, a
  substituted blob — is a `ReaderError`, never a crash, never silent wrong pixels.

- **Hostile geometry is rejected before allocation** *(behavioral + fuzz)*. New claim
  **`08-serialization#raster-tile-geometry-is-validated-before-allocation`**: a
  document declaring an absurd `edge`, an overflowing `width`×`height`, or a `blobs`
  array whose length ≠ `tiles_x * tiles_y` is rejected as a `ReaderError` before any
  allocation is sized by those numbers. The `serialize.format_tests` libFuzzer
  harness over the loader gains raster-layer seeds in its checked-in corpus.

- **SHA-256 correctness** *(golden-backed)*. The in-tree SHA-256 is pinned against
  the published NIST FIPS 180-4 test vectors (empty input, `"abc"`, the 448-bit
  message, the 1 000 000 × `'a'` case). A passing implementation of a fixed spec
  with no key and no secret is a *correct* one; the vectors are what make the
  hand-roll defensible rather than a risk (Decision 3).

- **Concurrency** *(TSan)*. A TSan test drives a save on a non-writer thread against
  a pinned snapshot while the writer thread paints and drops versions concurrently —
  exercising the memo's `retain` racing the writer's `release`, which is exactly the
  case doc 14 L168-181 requires the state store to admit. Clean under TSan.

- **Design-doc delta (same commit)**. Doc 08 (§The asset directory, Principle 8, the
  JSON example, §Dependency note), doc 17 (`arbc::base` and `arbc::serialize`
  responsibility cells), and two decision-record bullets in doc 00 — enumerated
  under Decisions 1 and 3.

- **Coverage / build / WBS gate**. ≥90% diff coverage on changed lines;
  `-Werror -Wpedantic` clean; `scripts/check_levels.py` green with **no `ALLOWED`
  edit**; `scripts/check_claims.py` green (register and tests land together);
  `tj3 project.tjp 2>&1 | grep -iE "error|warning"` silent.

- **Deferred — three named follow-ups (closer registers each in the WBS, milestone
  `m9_release`)**:
  - **`serialize.asset_gc`** (2d, area `serialize`) — mark-and-sweep over the asset
    directory: walk the saved document's referenced hashes and delete unreferenced
    `tiles/**` blobs. An explicit user action ("clean up"), **never implicit on
    save**, because an incremental save cannot know what another document version
    or a concurrent editor still references. Without it the asset directory grows
    monotonically across a long editing session — the honest cost of this task's
    never-delete rule (Constraint 6).
  - **`serialize.tile_store_parallel_save`** (2d, area `serialize`) — dispatch the
    per-tile encode pipeline (storage-convert → hash → shuffle → compress) across
    the pool workers. It is already a pure function of immutable `peek()`ed bytes
    with a stateless compressor, so the parallelism is sound by construction; this
    task ships it single-threaded and correct first. Behavioral-counter and TSan
    coverage, no wall-clock assertion.
  - **`kinds.raster_tilewise_load`** (2d, area `kinds`) — a
    `RasterStore::build_from_tiles` seam so a load streams level-0 blobs straight
    into pool blobs instead of materializing a dense `w × h` working buffer through
    `DecodedImage` (Decision 6). Bounds load peak RSS to O(tile) rather than
    O(image) — 384 MB transient per 24 MP layer today.

## Decisions

### 1. Introduce the write-side asset seam: `SaveContext` + `AssetSink`, mirroring `LoadContext` + `AssetSource`. *(doc 08 + doc 17 delta; doc 00 bullet)*

The read side is complete — `AssetSource::request(resolved_uri, on_ready)` and
`LoadContext` with `resolve`/`load_asset`
([`load_context.hpp:31-90`](../../../src/serialize/arbc/serialize/load_context.hpp)).
The write side **does not exist**: `SerializeFn` is `(const Content&) ->
expected<json, SerializeError>` with no context at all, and `serialize_document`
returns a `std::string` having touched no filesystem. A content-addressed tile
store has nowhere to put its blobs.

So `arbc::serialize` gains the symmetric half:

```cpp
class AssetSink {                                    // mirrors AssetSource
public:
  virtual ~AssetSink() = default;
  // Write-if-absent. Returns true if bytes were written, false if `resolved_uri`
  // was already present (content-addressed: same name => same content).
  virtual expected<bool, AssetSinkError> put(std::string_view resolved_uri,
                                             std::span<const std::byte> bytes) = 0;
  virtual std::uint64_t blobs_written() const noexcept = 0;
};

class SaveContext {                                  // mirrors LoadContext
  // base_uri(), asset_sink(), storage_format(), store_asset(relative_uri, bytes)
};
```

`SerializeFn` widens to `(const Content&, SaveContext&)` — six existing codecs
take an unused parameter, a mechanical edit — and `content_body_to_json` /
`serialize_document` / `save_document` thread it through.
`FilesystemAssetSink` lands in `runtime`, mirroring the existing
`FilesystemAssetSource`.

*Rejected — the raster codec writes its own files.* Doc 08 Principle 3 is
explicit: *"An asset-referencing kind never performs file I/O of its own"*, and
*"The core fetches asset bytes; the kind only decodes them."* Nothing in that
sentence is read-only by accident. A codec that opens files also makes the
format untestable without a real filesystem and unusable from a host that stores
documents somewhere other than a POSIX directory.

*Rejected — `SerializeFn` returns `(json, std::vector<Blob>)` and the writer
collects the blobs.* This avoids the context parameter but buffers **every blob
of the document in memory** before a single byte reaches disk — 1.4 GB for the
doc-08 reference composition — which is precisely the dense-flatten cost
Principle 8 exists to avoid, just relocated. The sink lets each blob be written
and dropped as it is encoded.

*Rejected — reuse `Content::external_asset_ref()` as the write channel.* Its own
doc comment forecloses it: *"Read-only discovery, never a write channel"*
([`content.hpp:643`](../../../src/contract/arbc/contract/content.hpp)). And it
would conflate the two things doc 08 keeps apart: an imported image *is* a URI
(Principle 3); painted pixels have no file to point at (Principle 8).

**Doc 08 delta**: a new normative paragraph in §The asset directory stating the
write-side symmetry — *the core writes asset bytes; the kind only encodes them* —
and naming `AssetSink`/`SaveContext` alongside `AssetSource`/`LoadContext`.
**Doc 17 delta**: extend the `arbc::serialize` responsibility cell (L69) to name
the content-addressed tile store, `SaveContext`/`AssetSink`, and the byte-shuffle.
**Doc 00 bullet**: the core's asset story is symmetric — the core owns asset I/O
in both directions and kinds only encode/decode bytes.

### 2. Split format from codec across the L4/L5 line.

`arbc::serialize` (L4) owns a **byte-oriented** tile-blob format API naming no
raster type: `hash_tile(span<const byte>) -> TileHash`, `shuffle`/`unshuffle`,
`encode_tile_blob(working_pixels, PixelFormat storage) -> {TileHash, frame}`,
`decode_tile_blob(frame, expected_hash, PixelFormat storage, int edge) ->
expected<vector<float>, …>`, plus the blob-URI derivation. `arbc::runtime` (L5)
owns `codec_raster.cpp` — the only TU that may name both `RasterContent` and
`nlohmann::json` — plus the `RasterTileStore` memo and `FilesystemAssetSink`.

*Rejected — put the store in `kind_raster`.* `kind_raster` is L4 and may not
include `serialize` (no same-level edges); it would also drag zstd into a kind,
which doc 17 L241-250 keeps on the serialize side of the codec line.

*Rejected — add a direct `serialize → pool` CMake edge so `serialize` can name
`BlockSlotRef`.* Unnecessary: the codec (in `runtime`) does the `peek()` and hands
`serialize` a plain byte span. Doc 17 L77-86 says not to widen an entry for an
include that already resolves transitively — and here it need not resolve at all.

### 3. The content hash is in-tree SHA-256, truncated to 128 bits. *(doc 08 + doc 17 delta; doc 00 bullet)*

Doc 08 says "keyed by content hash" and never names one. Nothing in the tree can
serve: `mix64` is a splitmix64 finalizer whose own comment disclaims it, and a
grep for `sha|blake|xxhash|md5|crc32` finds nothing. So this task introduces
`arbc::base`'s `sha256(span<const byte>) -> Sha256Digest`; the blob name is its
leading 16 bytes, hex-encoded (32 chars), fanned out as `tiles/<first-2-hex>/<full-hex>`.

Rationale. Doc 10's dependency table is a closed list and its zstd row closes with
*"it is worth exactly one small, well-vetted dependency and no more"* — so a hash
library is not available for the asking. Hand-rolling is nonetheless safe **here
specifically**, and it is worth being precise about why, because "hand-rolled
crypto" is normally a smell: SHA-256 is a fixed public spec (FIPS 180-4) of about
150 lines; there is no key and no secret, so there is no side-channel surface; and
correctness is *completely* pinned by the published NIST test vectors. The risk
being hedged against in the usual case — a subtly wrong construction that still
looks right — does not exist when the reference output for every input is
published. This is unlike hand-rolling a cipher or a MAC.

128 bits gives a 2^64 birthday bound, far beyond any document's tile count across
its entire undo history and any plausible fleet of collaborators. The failure mode
of a collision is *silent pixel corruption*, which is why the margin is set here
rather than at the cheap end.

*Rejected — a 64-bit non-cryptographic hash (xxHash-64).* Fast and small, but its
birthday bound is ~2^32 tiles. A large painting across a long undo history is
within a few orders of magnitude of that, the population is shared across
collaborators, and the consequence of a hit is one tile silently showing another
tile's pixels. It also cannot honestly support doc 08's "self-verifying blob"
promise (L357-359) against anything but random corruption.

*Rejected — BLAKE3 as a third dependency.* Faster, but it breaks doc 10's stated
two-dependency bound for a *speed* win that the hash memo (Decision 5) already
neutralizes — a steady-state save hashes only the tiles the user actually touched.
Buying a dependency to accelerate work we have arranged not to do is a bad trade.

*Rejected — hand-rolled BLAKE3.* The portable reference is an order of magnitude
more code than SHA-256 with a tree-hashing structure whose edge cases the
published vectors cover far less completely.

**Doc 08 delta**: Principle 8's blob bullet names the hash (SHA-256/128, hex, 2-hex
fan-out) and §Dependency note records that content hashing is deliberately in-tree
rather than a third dependency. **Doc 17 delta**: extend the `arbc::base` row to
name the content hash. **Doc 00 bullet**: content hashing is in-tree SHA-256/128,
not a third dependency — the dependency bound in doc 10 holds at two.

### 4. The storage format is document-scoped, not layer-scoped. *(doc 08 delta)*

Doc 08's JSON example (L66-71) shows `"format": "rgba16f"` inside the raster
layer's `params`, while its prose (L366-371) says *"the storage format is
document-carried"*. The two readings are not equivalent, and dedup forces the
choice: **the hash is over storage-format bytes**, so two layers with different
storage formats hash the same pixels to different names and the dedup that
Principle 8 is built on — *"across layers AND across undo versions"* — quietly
stops working. One storage format per asset directory is what makes the store a
store.

So it lands in the document's `arbc` meta block, `{"arbc": {"format": 1,
"storage_format": "rgba16f"}}`, defaulting to `rgba16f` when absent, and rides the
`SaveContext` / `LoadContext` into the codec. Permitted values are `rgba16f`
(default; lossy from an `rgba32f` working space, ample for 8-bit-origin content)
and `rgba32f` (lossless); any other value is a clean `ReaderError`. It stays
**authored, never inferred** — the lossy/lossless call is the user's.

Note the name must not collide with the composition-level `"format"` field, which
already means the *working* space
([`src/serialize/t/writer.t.cpp:57-71`](../../../src/serialize/t/writer.t.cpp)
asserts `"format": "rgba16f-linear-premul"` there). Different concept, different
key.

*Rejected — per-layer storage format.* More flexible, and the flexibility buys
nothing a user wants while breaking cross-layer dedup, which is the single largest
size lever in doc 08's table (4.3x, above compression's 2.9x).

**Doc 08 delta**: move `storage_format` out of the params example into the `arbc`
meta block; amend Principle 8's storage-format bullet to say document-scoped
explicitly and to say *why* (dedup requires one format per store).

### 5. The hash memo is keyed by `BlockSlotRef` **and pinned by a refcount**; it lives in `runtime`, bound into the codec by closure.

Without a memo, every save hashes every tile — O(document) per save, ~1.4 GB of
SHA-256 for the doc-08 reference composition — and "incremental save" is a claim
about blob *writes* while the CPU cost stays linear in document size. That is not
the gesture-cadence autosave doc 08 describes. With a memo, a re-save after one dab
hashes only the touched tiles, because `RasterStore::paint` leaves every untouched
tile's `BlockSlotRef` **identical** (`raster_pool_backing`'s structural-sharing
property).

But a `BlockSlotRef`-keyed memo is **unsound on its own**: the ref is
`{uint32 index, uint32 size}` in release builds (the generation tag is
`#ifndef NDEBUG`-gated,
[`big_block_pool.hpp:49-77`](../../../src/pool/arbc/pool/big_block_pool.hpp)), and
`SlotStore::allocate` reuses the most-recently-released slot first
([`slot_store.cpp:190-206`](../../../src/pool/slot_store.cpp)). A freed slot
re-allocated at the same size class yields a **bit-identical** ref for entirely
different pixels, and neither `peek()` nor `count()` can distinguish them.

**The refcount is the validity token.** `RasterTileStore` holds an owning `BlockRef`
for every memoized entry, so a memoized slot can never be recycled while the entry
lives. Each save rebuilds the memo against the version it just saved — hit ⇒ carry
the hash and the pin forward; miss ⇒ hash and take a pin — then swaps and drops the
old memo, releasing pins for tiles no longer in the saved version. So the memo pins
exactly the tiles of the **last-saved** document version: bounded by one document,
self-trimming, and arguably the working set you want resident anyway.

Ownership follows the established closure-binding pattern
(`nested_codec(loader)`, `image_codec(registry, loader)`): the host owns one
`RasterTileStore` per `Document` and binds it via
`builtin_codecs(registry, &tile_store)`. The zero-argument `builtin_codecs()`
registers `raster_codec(nullptr)` — **correct but non-memoizing**, hashing every
tile each save. That degradation is deliberate: every existing call site keeps
working and still saves correct pixels; it just does not get the incremental
CPU win.

*Rejected — memoize inside `TileTable` as a parallel hash column.* Structurally
elegant (invalidation becomes free: a CoW paint makes a new slot with an empty
memo). But it puts a serialization concept into `kind_raster` (L4), it needs
atomics for a `mutable` lazily-filled column on a `TileTable` shared with a
concurrent save, and it must additionally carry the storage-format tag because the
hash is format-dependent. Real cost, for an invalidation problem the refcount pin
already solves.

*Rejected — add a release-build generation column to `BigBlockPool`.* A change to
L1 with a permanent release-build memory cost across every pool user, to serve one
consumer. Out of proportion.

### 6. Load reconstructs through the existing `DecodedImage` constructor.

`RasterContent`'s only constructor takes a dense `DecodedImage`
([`raster_content.hpp:225`](../../../src/kind_raster/arbc/kind_raster/raster_content.hpp)),
and `RasterStore::build` runs the mip chain from it. So the loader decodes the
level-0 blobs, assembles the dense working buffer, and hands it to the existing
constructor — **zero new code in `kind_raster`**, and the mip rebuild routes
through exactly the path the existing byte-identical claim already covers.

The honest cost: a transient dense `w × h × 16` buffer — 384 MB for a 24 MP
layer — at load. That is a real regression against the sparsity this task is
otherwise defending, and it is why `kinds.raster_tilewise_load` is a named
follow-up rather than a shrug. It is deferred, not dismissed: correctness through
the proven path first, then the streaming seam.

### 7. Tile blobs load synchronously; there is no pending state.

`org.arbc.image` has a three-outcome fetch with a pending placeholder
([`external_asset_loader.hpp:47-108`](../../../src/runtime/arbc/runtime/external_asset_loader.hpp))
because an imported image has a source file that may be slow, missing, or remote —
and a sensible "not yet" rendering. Painted pixels have none of that: they are
document state, the asset directory is a sibling of the `.arbc` by construction,
and a raster layer whose tiles have not arrived is not a layer in a pending state,
it is a **broken document**. So a tile blob that does not arrive inline through
`LoadContext::load_asset` is a `ReaderError`, and `RasterContent` grows no
`install_asset` override.

*Rejected — reuse the pending-asset machinery.* It would add a partially-painted
raster state that no design doc describes, that the render path would have to
define pixels for, and that exists to serve a latency problem local files do not
have.

### 8. The `params` shape.

```jsonc
"params": {
  "tiles":  "assets/tiles/",         // base URI, resolved via LoadContext (doc 08 L34-36)
  "edge":   256,
  "width":  4096,
  "height": 3072,
  "blobs":  ["3fa91c…", "0091ab…", …]  // level 0, row-major, len == tiles_x * tiles_y
}
```

`tiles` stays the authored base URI, as doc 08's example has it — it goes through
the same `LoadContext::resolve` seam nested projects use, which is what keeps a
project directory relocatable (L34-36). The doc's `"levels": [[…]]` becomes a flat
`"blobs"` array: `levels` is an array-of-arrays that implies persisted mips, which
directly contradicts Principle 8's L362 bullet. A flat, mip-free, self-describing
name removes the contradiction. The 2-hex fan-out is derived inside the store, so
the JSON stays fan-out-agnostic and the layout can change without a format break.

**Doc 08 delta**: reconcile the two spellings of the blob path — the tree at L26-32
shows `tiles/3f/3fa91c….tile` (fan-out + suffix) while Principle 8's prose at L347
says `assets/tiles/<hash>` (flat, no suffix). Adopt the fan-out (it is what every
content-addressed store does, and a flat directory of 10⁵ blobs is hostile to
ordinary filesystem tooling — the very thing L44 says the directory exists to
preserve), drop the `.tile` suffix (a content-addressed blob's name is its hash;
an extension invites the idea that it means something), and amend both sites plus
the `levels` → `blobs` example.

### 9. The byte-shuffle groups by **sample**, stride = `bytes_per_sample`.

Stride 2 for `rgba16f`, 4 for `rgba32f`: all byte-0s of every sample, then all
byte-1s, and so on. This is doc 08 L372-376 read literally — *"separating a
**float's** noisy low mantissa bytes from its structured exponent and sign
planes"* — and it is the transform behind the calibrated 2.1x figure. The
shuffle is inside the blob (the hash is over the *unshuffled* storage bytes, per
Constraint 2), so it must round-trip exactly; `unshuffle ∘ shuffle == identity`
is pinned as a byte-exact golden on a pure function.

A wider stride (`bytes_per_pixel`, 8 or 16) would additionally deinterleave the
R/G/B/A planes and might compress better still. It is not what the doc specifies
and not what its number was measured against, so it is not what ships. Not
deferred to a task — a measurement-and-decide item is not agent-implementable
work; noted here and surfaced for the parking lot.

### 10. No `check_levels.py` change.

`ALLOWED["runtime"]` already contains both `kind_raster` and `serialize`
([`scripts/check_levels.py:51-54`](../../../scripts/check_levels.py)), and
`ALLOWED["serialize"] = {contract, model}` reaches `media` and `pool`
transitively, which is all the byte-oriented format API needs. Only
`src/runtime/CMakeLists.txt` gains `kind_raster` in `DEPENDS`, which the doc-17
`arbc::runtime` row ("everything below") already sanctions. If an implementer finds
themselves editing `ALLOWED`, the split in Decision 2 has been violated.

## Open questions

(none — all decided.)

## Status

**Done** — 2026-07-14.

- Created `src/base/arbc/base/sha256.hpp`, `src/base/sha256.cpp`, `src/base/t/sha256.t.cpp` — in-tree SHA-256/128 (FIPS 180-4) with NIST FIPS vectors; Decision 3.
- Created `src/serialize/arbc/serialize/save_context.hpp`, `src/serialize/save_context.cpp` — write-side `SaveContext` + `AssetSink` seam mirroring `LoadContext`/`AssetSource`; widened `SerializeFn` to `(Content&, SaveContext&)` (Decision 1). Added `AssetSink::contains()` / `SaveContext::has_asset()` beyond the refinement sketch (prevents re-compressing already-on-disk tiles before learning they exist).
- Created `src/serialize/arbc/serialize/tile_blob.hpp`, `src/serialize/tile_blob.cpp`, `src/serialize/t/tile_blob.t.cpp` — tile-blob encode/decode (storage-format convert → hash → byte-shuffle → zstd), byte-shuffle goldens, hostile-geometry validation (Constraint 7); Decisions 2/9.
- Created `src/runtime/arbc/runtime/filesystem_asset_sink.hpp`, `src/runtime/filesystem_asset_sink.cpp`, `src/runtime/t/filesystem_asset_sink.t.cpp` — write-if-absent, temp+rename crash-atomic `FilesystemAssetSink` (Constraint 6); `blobs_written()` counter.
- Created `src/runtime/arbc/runtime/raster_tile_store.hpp`, `src/runtime/raster_tile_store.cpp`, `src/runtime/t/raster_tile_store.t.cpp` — `RasterTileStore` with `BlockSlotRef`-keyed, refcount-pinned hash memo (Decision 5); `tiles_hashed()` counter.
- Created `src/runtime/codec_raster.cpp` — `org.arbc.raster` serialize/deserialize codec registered from L5 `runtime`; level-0-only (no mips persisted); `Document::storage_format` document-scoped (Decision 4).
- Created `tests/raster_tile_store_golden.t.cpp`, `tests/raster_tile_store_hostile.t.cpp`, `tests/raster_tile_store_concurrency.t.cpp` — 7 new claims covering content-addressed store, incremental save, byte-exact round-trip, quantization idempotence, mips-not-persisted, blob self-verification, geometry validation; TSan save-races-writer test.
- Added 5 raster fuzz-corpus seeds under `tests/fuzz/corpus/load_document/`.
- Edited `src/serialize/{codec,writer,reader,load_context}.{hpp,cpp}`, six existing codec TUs, `src/runtime/document_serialize.{hpp,cpp}`, `document.hpp`, `builtin_codecs.hpp`, three CMakeLists, four existing test files, `tests/claims/registry.tsv` (+7 claims), `docs/design/{00,08,17}*.md`.
- All 7 acceptance claims exercised; no scoped test deferred; no `ALLOWED` edit.
