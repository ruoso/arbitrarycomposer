# serialize.zstd_dep — Compressor dependency decision (zstd)

## TaskJuggler entry

Back-link: [`tasks/60-serialize.tji:61-65`](../../60-serialize.tji) — `task zstd_dep`
inside `task serialize`.

> note "Take zstd as libarbc's SECOND core dependency, for the raster tile-blob
> compression of doc 08 Principle 8. The decision is already made (doc 10
> dependency table, 2026-07-12) — this task is the wiring, not the evaluation:
> consume find-first with a version-pinned FetchContent fallback and pin it in CI,
> exactly as serialize.json_dep did for nlohmann/json (never an in-tree vendored
> copy; doc 08 Dependency note's 'unproblematic vendoring'). Assert the honest
> bound in the refinement so it is not quietly widened later: compression is the
> WEAKEST of the size levers (2.9x, below content-addressed dedup's 4.3x), it buys
> one small well-vetted dependency and no more, and it is explicitly NOT a licence
> to bring an image codec into core — decoding stays behind doc 17's codec line in
> arbc-plugin-image / arbc-plugin-imageseq. Docs 08/10."

## Effort estimate

**0.5d** (`tasks/60-serialize.tji:62`). Deliberately the same size as its sibling
gate `serialize.json_dep` (0.5d), and for the same reason: the deliverable is a
**wired, buildable, proven dependency seam**, not a feature. The library choice is
*already made* — doc 10's dependency table records `zstd` as chosen
(`10:27`) — so unlike `json_dep` there is no trade study to run. What is left is
the wiring (CMake find-first + pinned `FetchContent` fallback), the two design-doc
deltas that make the second core dependency honest in the constitution, a minimal
internal compress/decompress seam, and the smoke test that proves the load-bearing
properties. The 5d of real work — the content-addressed tile store, the
byte-shuffle, incremental save, the `org.arbc.raster` codec — is
`serialize.raster_tile_store`, which `depends !zstd_dep`.

## Inherited dependencies

`zstd_dep` declares no `depends` of its own (`tasks/60-serialize.tji:61-65`), so —
exactly like `json_dep` — it inherits the parent `task serialize` edges
(`tasks/60-serialize.tji:4`): `depends contract.registry, model.journal`. Both are
long since DONE. It is a *peer* of `json_dep`, not a successor: a second
dependency-ratification gate opened by a second core dependency.

**Settled (formal `depends`, inherited):**

- **`contract.registry`** — DONE (2026-07-09,
  [`contract/registry.md`](../contract/registry.md)). Bears on this task only
  distantly: it fixes the kind id (`org.arbc.raster`) that
  `serialize.raster_tile_store` will register a codec under.
- **`model.journal`** — DONE (2026-07-05, [`model/journal.md`](../model/journal.md)).
  Not a constraint on the compressor choice.

**Settled (informal, but the real context — this task exists because they landed):**

- **`serialize.json_dep`** — DONE (2026-07-09,
  [`json_dep.md`](json_dep.md)). The template this task mirrors *and deviates from
  in one place*. It established: find-first + pinned `FetchContent` +
  `FIND_PACKAGE_ARGS` + SYSTEM-marked includes (`CMakeLists.txt:123-147`), the
  never-vendored-in-tree reading of doc 08's "unproblematic vendoring"
  (Decision 3), errors-as-values at the boundary (Decision 2), and the rule that
  the dependency is linked **PRIVATE** so it stays off `libarbc`'s public interface
  (Decision 3 / Constraint 5). Its Constraint 5 — *"Header-only is load-bearing,
  not incidental"* — is precisely the argument that **does not transfer to zstd**,
  and Decision 2 below is where this task re-earns it.
- **`serialize.writer` / `reader` / `kind_params` / `sharing`** — all DONE. They
  stood up `src/serialize/` (`src/serialize/CMakeLists.txt`), the `arbc_serialize`
  OBJECT component, the `PRIVATE` external-dep link idiom
  (`src/serialize/CMakeLists.txt`, the `target_link_libraries(arbc_serialize PRIVATE
  nlohmann_json::nlohmann_json)` line), and the serialize-owned **codec table**
  keyed by kind id that `raster_tile_store` will register into.

**Pending:** none.

**Downstream (this task unblocks):**

- **`serialize.raster_tile_store`** (`tasks/60-serialize.tji:66-71`, `depends
  !kind_params, !zstd_dep, kinds.raster_pool_backing`) — the 5d task that makes a
  painting saveable. It is the *only* consumer of this dependency, and every
  scoping decision below is made in its service.

## What this task is

Take **zstd** as `libarbc`'s **second core dependency** and wire it, so that
`serialize.raster_tile_store` starts on a settled, buildable, *proven* seam instead
of re-litigating the compressor. Concretely, five things:

1. **Wire the dependency** into `CMakeLists.txt` — find-first
   (`find_package(zstd)`) with a version-pinned `FetchContent` fallback, SYSTEM
   includes, never an in-tree vendored copy — mirroring the shipped nlohmann block
   (`CMakeLists.txt:123-147`), with the three wrinkles a *compiled* library brings
   that a header-only one did not (Decisions 2 and 3).
2. **Normalize the target name.** A system zstd and a fetched zstd do not present
   the same CMake target. Land one `arbc_zstd` INTERFACE target that resolves to
   whichever materialized, so `src/serialize/CMakeLists.txt` names exactly one
   thing (Decision 3).
3. **Link it `PRIVATE` onto `arbc_serialize`** and land the **minimal internal
   seam** it links for: `compress_blob` / `decompress_blob` in
   `src/serialize/` — raw zstd frame in, raw zstd frame out. Not the byte-shuffle,
   not the tile store, not the hashing (Decision 4).
4. **Land the two design-doc deltas** that make the second dependency honest: doc
   08's §Dependency note still says the core "needs a JSON reader/writer" as though
   that were the only dep (`08:368-374`), and doc 17's `arbc::serialize` row lists
   its external deps as "JSON dep" alone (`17:59`). Plus the Principle 8
   clarification in Decision 1, which is the substantive one.
5. **Prove it with a smoke test + two claims** — byte-exact round-trip, and bounded
   failure-as-a-value on corrupt input (the loader is a fuzzed, untrusted-input
   surface).

It does **not** implement the byte-shuffle, the content-addressed store, incremental
save, mip rebuild, the storage-format field, or the `org.arbc.raster` codec. Those
are `serialize.raster_tile_store`'s, all five of them.

## Why it needs to be done

`org.arbc.raster` has **no codec at all** today — the default table registers
solid/tone/fade/crossfade/nested only — so **a painted layer does not round-trip
and a user cannot save their work** (`tasks/60-serialize.tji:70`). Fixing that is
`serialize.raster_tile_store`, and it is blocked here: doc 08 Principle 8 specifies
the tile blobs as *"per blob: zstd with a byte-shuffle"* (`08:315-319`), so the
store cannot be built until the compressor is a real target on a real link line.

The decision itself is *already made* — doc 10's dependency table records **"chosen:
`zstd`"** against `serialize.zstd_dep` (`10:27`), with the rationale (zstd over LZMA
on speed, over zlib on ratio) and the consumption mechanism already written down.
So this task is not an evaluation. It exists because taking a **second core
dependency** is a threshold event for a project whose dependency policy is *part of
its public promise* (`10:34-35`), and because a *compiled* dependency does not
inherit the argument that made the first one safe.

That last point is the whole substance of this task. `json_dep` could wave the
transitive-burden question away in one clause — nlohmann is header-only, so it
imposes *no* link or system requirement on an embedder of `libarbc`
(`json_dep.md:169-173`, Constraint 5: *"Header-only is load-bearing, not
incidental"*). **zstd is a compiled C library.** It has an archive, symbols, and a
possible runtime `.so`. Doc 10's promise — *"embedding the core must never
transitively impose codecs, GPU SDKs, or a GUI toolkit"* (`10:34-35`) — is not
self-evidently intact once a compiled library is inside `libarbc`. Somebody has to
decide, on the record, what "PRIVATE" actually buys here and what escapes anyway.
That is Decision 2, and doing it now — as a 0.5d gate with a smoke test — is far
cheaper than discovering it inside a 5d tile-store task.

## Inputs / context

### Design docs (normative, doc 16)

- **`docs/design/08-serialization.md:284-351`** — **Principle 8**, the governing
  section. Its compression bullet (`08:315-319`) is the requirement this task
  serves: *"**Compression is per blob: zstd with a byte-shuffle.** The shuffle
  (group all byte-0s, then all byte-1s, …) separates a float's noisy low mantissa
  bytes from its structured exponent and sign planes, which is what lifts
  photographic tiles from 1.5x to 2.1x. zstd rather than LZMA: an interactive editor
  saves incrementally and cannot pay LZMA's speed for the ratio."* Its blob bullet
  (`08:298-304`) — *"**Blobs are keyed by content hash.** Each *distinct* tile is
  written once, as `assets/tiles/<hash>`"* — is **ambiguous about what is hashed**,
  and Decision 1 resolves it. Its lever table (`08:325-343`) is the honest bound the
  `.tji` note demands be asserted: compression is **2.9x, the weakest lever**, below
  content-addressed dedup's 4.3x.
- **`08:368-374`** — **§Dependency note**, verbatim: *"This is the first place the
  'minimal vetted deps' policy (doc 10) bites: the core needs a JSON reader/writer.
  Candidates evaluated in doc 10; the requirement here is: order-preserving-optional,
  exact round-trip of unknown content, no exceptions across the plugin boundary
  (errors as values at the API), and unproblematic vendoring."* **JSON-only — it has
  never been extended to the compressor.** Amended by this task (delta below).
- **`08:21-46`** — **§The asset directory**: `project.assets/tiles/3f/3fa91c….tile`.
  The blobs this compressor produces. *"A directory rather than a single file is the
  deliberate choice. It is what makes content-addressed blobs work as *files* — an
  incremental save writes only the new tiles and touches nothing else"* (`08:38-41`).
- **`docs/design/10-tooling-and-packaging.md:27`** — **the zstd row** of §Dependency
  policy. The decision, already recorded: *"**chosen: `zstd`** (`serialize.zstd_dep`).
  The second core dependency, and taken deliberately… `zstd` over LZMA on speed: an
  interactive editor saves incrementally, on a gesture cadence, and cannot pay LZMA's
  compression time for its extra ratio; over zlib on ratio, at comparable speed.
  Applied with a byte-shuffle (doc 08 Principle 8) — **the shuffle is ours, not the
  library's**. … Consumed find-first with a version-pinned `FetchContent` fallback,
  pinned in CI, mirroring the JSON wiring."* **No doc-10 delta is needed** — this row
  is already correct and complete.
- **`10:32-35`** — the consumption rule and the public promise: *"consume through
  standard find mechanisms (`find_package`), never vendored copies in-tree; lockstep
  versions pinned in CI. The dependency *policy* is part of the public promise:
  embedding the core must never transitively impose codecs, GPU SDKs, or a GUI
  toolkit."*
- **`10:29`** — the **Image codecs** row: *"**not core** … Decoding belongs to the two
  kinds that reference external image files — `org.arbc.image` and `org.arbc.imageseq`
  — which therefore ship *outside* `libarbc` as plugin artifacts carrying their own
  decode dependency."* The line this task must not cross.
- **`10:15-17`** — *"No exceptions across public API and plugin boundaries (doc 03);
  Public errors are values (`expected`-style result — `arbc::expected`)."*
- **`docs/design/17-internal-components.md:59`** — levelization row: `| arbc::serialize
  | 4 | JSON read/write, canonical form, unknown-kind placeholders, LoadContext, $ref
  resolution | 08 | contract, model (+ below); **JSON dep** |`. The external-dep
  column names JSON alone. Amended by this task (delta below).
- **`17:205-229`** — **§The codec line**, the constitutional boundary the `.tji` note
  insists this task not quietly widen: *"codecs must never ride into an embedder's
  link line (doc 10). The resolution: **`libarbc`'s built-in kinds are codec-free**"*
  (`17:208-210`), and *"no compressor closes that gap, because photographic tiles are
  93% of the bytes and compress about 2.1x"* (`17:226-227`).
- **`docs/design/16-sdlc-and-quality.md`** — claims register + testing taxonomy.

### Source seams (build integration)

- **`CMakeLists.txt:123-147`** — the shipped **nlohmann/json** block: the four-part
  template this task mirrors — `include(FetchContent)`; a `set(<DEP>_BuildTests OFF
  CACHE INTERNAL "")` knob; `FetchContent_Declare` with `GIT_REPOSITORY` + pinned
  `GIT_TAG v3.11.3` + `FIND_PACKAGE_ARGS 3`; `FetchContent_MakeAvailable`; then the
  SYSTEM re-marking that copies `INTERFACE_INCLUDE_DIRECTORIES` into
  `INTERFACE_SYSTEM_INCLUDE_DIRECTORIES`. **Unconditional** — not inside
  `if(BUILD_TESTING)` — because it is a dependency of shipped code. zstd goes in the
  same place, for the same reason.
- **`CMakeLists.txt:82-98`** (Catch2, gated on `BUILD_TESTING`) and
  **`CMakeLists.txt:100-121`** (Google Benchmark, gated on `ARBC_BENCHMARKS`) — the
  precedents for conditional fetch and for SYSTEM re-marking. Neither gate applies
  to zstd.
- **`CMakeLists.txt:23-27`** — `arbc_build_flags`: `-Wall -Wextra -Wpedantic -Werror`
  (`/W4 /WX /permissive-` on MSVC). This is why third-party includes must be marked
  SYSTEM.
- **`src/serialize/CMakeLists.txt`** — the **exact idiom to copy**, and the invariant
  to preserve. `arbc_add_component(NAME serialize … DEPENDS contract model)` followed
  by an explicit `target_link_libraries(arbc_serialize PRIVATE
  nlohmann_json::nlohmann_json)`, with the comment recording *why*: *"nlohmann/json is
  header-only, so linking it PRIVATE keeps it off libarbc's public interface — no
  transitive link/system requirement on an embedder (doc 10; serialize.writer
  Decision 3). `arbc_add_component` exposes no extra-link parameter, so the wiring is
  an explicit `target_link_libraries` here."* Note the consequence, already handled
  for nlohmann: because the dep is PRIVATE to the OBJECT library and does **not**
  propagate through the umbrella `arbc`, every test that reaches serialize internals
  must re-name the dep on its own link line (the `if(TARGET arbc_serialize_t)` block).
  zstd inherits this exactly.
- **`cmake/ArbcComponent.cmake:18-40`** — `arbc_add_component`: `add_library(arbc_<name>
  OBJECT …)`; its `DEPENDS` loop hardcodes the `arbc_` prefix
  (`target_link_libraries(${target} PUBLIC "arbc_${dep}")`, `:33-35`), so **there is no
  parameter for an external target** — an external dep is linked by a direct
  `target_link_libraries` call after the `arbc_add_component` call. `:129-141` —
  `arbc_finalize_library`: the umbrella `arbc` links every component OBJECT library
  PRIVATE.
- **`tests/CMakeLists.txt:14-26`** — the `arbc_serialize_json_dep_smoke_t`
  registration: a raw `add_executable` + `target_link_libraries(… PRIVATE
  nlohmann_json::nlohmann_json Catch2::Catch2WithMain arbc_build_flags)` +
  `catch_discover_tests`, deliberately **not** `arbc_component_test()`. This task's
  smoke test deviates (Decision 4): it links **`arbc`**, not zstd standalone, because
  what must be proven is a property of `libarbc`, not of zstd.
- **`tests/serialize_json_dep_smoke.t.cpp`** — the shape to mirror: three
  `TEST_CASE`s, each pinning one capability the format rests on, with doc/line
  citations in the header comment.
- **`scripts/check_levels.py:33`** (`ALLOWED`) — the levelization checker scans
  `#include <arbc/…>` only (`:7`), so an **external** target is invisible to it.
  `zstd` adds **no** component-graph edge and the checker needs **no** change —
  same finding `json_dep.md:140-143` recorded for nlohmann.
- **`src/base/arbc/base/expected.hpp:29`** — `arbc::expected<T, E>`, the
  errors-as-values vehicle the decompress seam returns.
- **`src/kind_raster/arbc/kind_raster/raster_content.hpp:33,39`** and
  **`src/kind_raster/raster_content.cpp:54-56`** — the bytes this compressor will
  eventually see, for sizing the smoke test's realistic case: `k_default_tile_edge =
  256`, `k_tile_channels = 4`, and `blob_bytes(edge) = edge * edge * k_tile_channels
  * sizeof(float)` — **exactly 1 MiB** at edge 256 in the `rgba32f` working format,
  **512 KiB** at the `rgba16f` storage default. Tiles are read through
  `BigBlockPool::peek` as an immutable `std::span<const std::byte>` of exactly that
  length, which is precisely the shape `compress_blob` takes. Note for context (it
  bears on `raster_tile_store`, not on this task): the in-memory store is a **dense
  per-level vector of pool slot refs** with sharing via the pool's refcount — it is
  *not* content-addressed in memory, and **no content-hash utility exists anywhere in
  the tree** (`src/base/arbc/base/hash_mix.hpp:29-31` is splitmix64 and says outright
  it *"is NOT a cryptographic hash"*). Introducing a real content hash is
  `raster_tile_store`'s job, and Decision 1 below is what tells it *what to hash*.
- **`.github/workflows/ci.yml:89-97`** — configure/build/test with **no dependency
  pinning step of any kind**. There is no apt/vcpkg/Conan/CPM/cache line for
  nlohmann anywhere. "Pinned in CI" in doc 10 means, operationally: *the `GIT_TAG` in
  `CMakeLists.txt` is the pin, and CI builds it because no system copy is found.*
  **This is where zstd differs and the difference is not cosmetic** — see Decision 5.
- **zstd is greenfield**: `grep -ri zstd` hits `docs/design/{08,10}`,
  `tasks/{60-serialize,99-milestones}.tji`, and nothing else. No code, no CMake, no CI.

### Predecessor / sibling refinements

[`json_dep.md`](json_dep.md) (the template; its Constraint 5 and Decisions 2–4 are
each either inherited or explicitly re-decided below), [`writer.md`](writer.md)
(PRIVATE external-dep link idiom), [`kind_params.md`](kind_params.md) (the codec
table `raster_tile_store` registers into), [`format_tests.md`](format_tests.md) (the
libFuzzer loader harness — the reason Decision 6's bounded-decompression constraint
is not optional).

## Constraints / requirements

1. **Ratify; do not re-open the trade study.** The compressor is `zstd`. Doc 10:27
   already records the choice and the reasoning (over LZMA on speed, over zlib on
   ratio). LZMA/brotli/zlib are **not** re-evaluated here. Doc 10 needs **no** delta.
2. **The honest bound is asserted, not quietly widened** (the `.tji` note's explicit
   demand). Three things must remain true in the doc record and in this refinement:
   (a) compression is the **weakest** size lever — **2.9x**, below content-addressed
   dedup's **4.3x** (`08:325-343`); (b) it therefore buys **exactly one small,
   well-vetted dependency and no more**; (c) it is **not** a licence for an image
   codec in core — decoding stays behind doc 17's codec line, out of `libarbc`, in
   `arbc-plugin-image` / `arbc-plugin-imageseq` (`17:205-229`, `10:29`). zstd is a
   general-purpose **byte-stream** compressor operating on **our own** blob format;
   it never parses a foreign file format. That is the distinction that keeps the
   codec line intact, and it must be stated where a future reader will find it.
3. **PRIVATE link; zstd's types never appear in a `libarbc` public header.** Exactly
   as the JSON type stays private to `arbc::serialize` (doc 08 Principle 1:
   *"Because the JSON type stays private to `arbc::serialize` (doc 17 levelization:
   the `Content` interface lives in `contract`, which must not name the JSON
   library)"*, `08:95-98`), **no `#include <zstd.h>` may appear in any header
   installed with `libarbc`, and no `zstd.h` type may appear in any signature the
   seam exposes.** The seam trades in `std::span<const std::byte>` /
   `std::vector<std::byte>` / `arbc::expected`. This is what makes the compile/include
   interface unconditionally clean, and it is stronger than a link-level claim.
4. **Consumption: find-first, pinned `FetchContent` fallback, SYSTEM includes, never
   an in-tree vendored copy** (`10:32-33`). The stb-class vendoring carve-out at
   `10:29` / `17:211,218-219` applies **only** to out-of-lib plugin artifacts, never
   to a core dep. Note the two wiring wrinkles a compiled library brings, both of
   which the implementer must handle and neither of which arose for nlohmann:
   - **`SOURCE_SUBDIR`.** The zstd repository has **no CMakeLists.txt at its root** —
     its CMake build lives in `build/cmake/`. `FetchContent_Declare` therefore needs
     `SOURCE_SUBDIR build/cmake`. Omitting it fails the configure with a confusing
     "does not contain a CMakeLists.txt" error.
   - **Target-name normalization.** A system zstd (via `zstdConfig.cmake`) presents
     `zstd::libzstd_shared` / `zstd::libzstd_static`; the fetched build presents
     `libzstd_static`. They are not the same name. See Decision 3.
5. **No new component-graph edge.** `check_levels.py` scans `#include <arbc/…>` only;
   an external target is invisible to it (`json_dep.md:140-143`). `ALLOWED` needs no
   entry and the checker must stay green unchanged.
6. **The seam is stateless and reentrant.** Use zstd's one-shot API (`ZSTD_compress`
   / `ZSTD_decompress` / `ZSTD_compressBound` / `ZSTD_isError`), **not** a shared
   `ZSTD_CCtx`/`ZSTD_DCtx`. A `ZSTD_CCtx` is explicitly **not** safe to share across
   threads, and tile compression will eventually be called from pool workers
   (`raster_tile_store`). The one-shot functions are stateless and thread-safe by
   construction, so the seam is safe to call concurrently with no locking and no new
   TSan surface. Recorded here so a later "optimization" to a shared reused context
   is recognized as the data race it would be. (A per-call context, or a
   thread-local one, is the correct future optimization if profiling ever demands it
   — never a shared one.)
7. **Decompression is bounded and never trusts the blob.** The loader is an
   **untrusted-input, fuzzed** surface (`serialize.format_tests` ships a libFuzzer
   harness over it). `decompress_blob` therefore takes the expected output size
   **from the caller** — the tile geometry the document declares — and **never**
   from the frame header. `ZSTD_getFrameContentSize` is attacker-controlled data: it
   may be `ZSTD_CONTENTSIZE_UNKNOWN`, `ZSTD_CONTENTSIZE_ERROR`, or simply a lie
   ("this frame expands to 64 GB"). Allocating on it is a trivial remote OOM. The
   seam allocates the caller's bound, decompresses into it, and fails as a value if
   the frame does not produce exactly that many bytes.
8. **Errors as values; no exception crosses the boundary** (`10:15-17`). zstd's C API
   returns `size_t` codes checked with `ZSTD_isError` and throws nothing, so this
   falls out naturally — but it must be *pinned by a test*, not assumed:
   `decompress_blob` returns `arbc::expected` and reports corrupt/truncated/oversized
   input as an error value.
9. **Diff coverage ≥ 90%** (doc 16) on changed lines. The changed code is the
   `compress_blob`/`decompress_blob` TU plus the smoke test; the tests exercise both
   the success and every error path. CMake wiring and doc deltas carry no
   instrumented lines.
10. **Nothing in the byte-shuffle, the tile store, the hashing, or the storage-format
    field lands here.** All five are `serialize.raster_tile_store`. Doc 10:27 is
    explicit that **"the shuffle is ours, not the library's"** — it is a transform
    applied *before* `compress_blob` and *after* `decompress_blob`, not a
    responsibility of this seam.

## Acceptance criteria

- **Design-doc deltas land (same commit, doc 16's rule).** Four edits, two files —
  **already written by this refinement**; the closer commits them with the code:
  1. **`docs/design/08-serialization.md`, Principle 8 blob bullet** — resolves the
     "keyed by content hash" ambiguity: the hash is over the **uncompressed** tile
     bytes in the storage format, so compression is a storage encoding and **never**
     content identity (Decision 1). This is the substantive delta; the rest tighten
     the record.
  2. **`docs/design/08-serialization.md`, §Dependency note** — extended from "the
     core needs a JSON reader/writer" (JSON-only) to cover **both** core
     dependencies, stating the compressor's requirements in the same shape as JSON's
     four: exact round-trip, errors as values across the boundary, **bounded
     decompression of untrusted input**, never-in-tree consumption — plus the two
     bounds from Constraint 2 (compressed bytes are not identity; a compressor is not
     a codec and is not a precedent for one). **This is the doc text the two new
     claims anchor to.**
  3. **`docs/design/17-internal-components.md`, the `arbc::serialize` row** — external-dep
     column `JSON dep` → `JSON dep, compressor (zstd)`, and contents gain
     "tile-blob compress/decompress".
  4. **`docs/design/17-internal-components.md`, §The codec line** — one paragraph
     stating that the compressor does not cross the line (the distinction is *what is
     being parsed*, not whether bytes get smaller), so "we already depend on a
     compression library" can never be cited as precedent for an in-core image codec.
     This is the `.tji` note's explicit demand, placed at the codec line's
     constitutional home where a future reader will actually meet it.
  **No doc-10 delta** (`10:27` already records the choice, the rationale, and the
  consumption mechanism — it is complete and correct) and **no doc-00 bullet**
  (Decision 7).
- **Dependency wired and buildable on both paths.** A `FetchContent_Declare(zstd …
  SOURCE_SUBDIR build/cmake … GIT_TAG <pin> FIND_PACKAGE_ARGS)` block lands in
  `CMakeLists.txt` beside the nlohmann one (unconditional, not gated on
  `BUILD_TESTING`), with zstd's programs/tests/shared/legacy knobs off,
  `POSITION_INDEPENDENT_CODE ON` on the fetched static target, and SYSTEM-marked
  includes. **Verify both paths explicitly**: a clean configure with no system zstd
  fetches and builds the pinned tag; a configure on a machine with `libzstd-dev`
  present resolves via `find_package` instead. `-Werror -Wpedantic` stays green on
  both (no warnings out of zstd headers).
- **`arbc_zstd` normalization target exists** and is the single name
  `src/serialize/CMakeLists.txt` links (Decision 3). A configure on either path
  produces a usable `arbc_zstd`.
- **The seam links PRIVATE and does not escape.** `target_link_libraries(arbc_serialize
  PRIVATE arbc_zstd)` in `src/serialize/CMakeLists.txt`, mirroring the nlohmann line
  directly above it. **The escape property is proven by construction, not asserted**:
  the smoke test links **`arbc` only** — it never names `arbc_zstd` or `zstd::*` on
  its own link line — and calls the seam. If zstd had leaked onto `libarbc`'s public
  *include* interface the test would not compile; if the PRIVATE link were wrong for
  the shipped library shape, it would not link. A standalone probe that links zstd
  directly (the `json_dep` shape, `tests/CMakeLists.txt:14-26`) would prove **none**
  of this, which is exactly why this task deviates (Decision 4).
- **Two claims-register entries + `enforces:`-tagged tests** (`tests/claims/registry.tsv`;
  ids anchored to the doc-08 Dependency-note delta above):
  - **`08-serialization#tile-blob-codec-round-trips-byte-exactly`** — `decompress_blob(compress_blob(b),
    b.size())` returns `b` byte-for-byte, for every input class the tile store will
    actually hand it: empty input; an all-zero tile (the empty-tile case, which must
    collapse to a tiny frame); a flat/constant tile; a high-entropy incompressible
    tile (the photographic case, where the compressed frame may legitimately be
    **larger** than the input — `ZSTD_compressBound` exists precisely because of
    this, and a seam that sized its output buffer at `input.size()` would fail here);
    and a tile at the real geometry — `src/kind_raster/raster_content.cpp:54-56`
    fixes the in-memory blob at `edge * edge * 4 * sizeof(float)` = **1 MiB** at the
    default `k_default_tile_edge = 256` (`raster_content.hpp:33`), which at the
    `rgba16f` storage default is **512 KiB** on disk.
  - **`08-serialization#tile-blob-decompress-is-bounded-and-fails-as-a-value`** — a
    corrupt, truncated, or hostile blob yields an `arbc::expected` **error value**:
    no exception escapes, no abort, and **no allocation beyond the caller's declared
    bound**. Covers, at minimum: random bytes that are not a zstd frame; a valid
    frame truncated mid-stream; a valid frame whose content is *shorter* than the
    caller's declared size; a valid frame whose content is *longer*; and — the one
    that matters most — **a frame whose header advertises a huge content size while
    the caller's bound is one tile**, which must fail as a value rather than attempt
    the allocation. (Constraint 7. This is the test that makes the fuzz harness in
    `serialize.format_tests` meaningful rather than decorative once
    `raster_tile_store` puts blob bytes on the loader's untrusted path.)
- **Levelization + build + WBS gate green.** `scripts/check_levels.py` passes
  **unchanged** (no `ALLOWED` edit — Constraint 5); the full build and test suite
  pass; `tj3 project.tjp 2>&1 | grep -iE "error|warning"` is silent after the `.tji`
  `complete 100` + refinement back-link land.
- **Nothing new deferred to the WBS.** The sole consumer,
  `serialize.raster_tile_store`, is already a WBS leaf that `depends !zstd_dep`
  (`tasks/60-serialize.tji:69`) and already owns the byte-shuffle, the store, the
  storage-format field, incremental save, and the `org.arbc.raster` codec in its note
  (`:70`). **No new task, no parking-lot item.** One constraint is *recorded for* an
  existing future leaf rather than deferred to a new one: whenever `libarbc` grows an
  install/export (`packaging.shared_library_build`, already a WBS leaf in M-final),
  its exported CMake config must express the zstd requirement — a `find_dependency(zstd)`
  when zstd was found on the system, nothing when the pinned static build was folded
  in. That is a packaging concern by construction and needs no leaf of its own; it is
  called out in Decision 2 so the packaging task inherits it in writing.

## Decisions

1. **The content hash is over the *uncompressed* tile bytes — compression is a
   storage encoding, never content identity. Design-doc delta to Principle 8.**
   Doc 08 says *"Blobs are keyed by content hash"* (`08:298`) and never says **hash
   of what**. The ambiguity is load-bearing and it is *this* task's to resolve,
   because the answer decides whether zstd's output must be byte-reproducible —
   which in turn decides the whole consumption story below.
   Hashing the **compressed** bytes would make the blob's *name* a function of the
   zstd version, the compression level, and the build's zstd tuning. A distro zstd
   1.5.5 and a fetched 1.5.7 would name the same tile differently; the entire asset
   directory would be invalidated by a compressor upgrade; incremental save would
   rewrite every blob after a version bump; and cross-machine dedup — the lever doing
   **4.3x**, the strongest one there is (`08:325-343`) — would silently break between
   collaborators. It would also drag a *hard* reproducibility requirement onto a
   third-party library that has never promised byte-stable output across versions.
   Hashing the **uncompressed** bytes in the document's storage format decouples the
   store from the compressor completely. zstd's output may vary freely across
   versions and levels; the blob's name, the dedup, and the document's meaning do
   not move. It also makes the store **self-verifying at no cost**: decompress a
   blob, hash it, compare to the filename it was fetched under — a corrupt or
   substituted blob is caught structurally. And it keeps the choice of compression
   *level* a free tuning knob (a future "save fast" vs "save small") rather than a
   format break. Incremental save is unaffected: it tests for a blob's **presence by
   name**, so an already-present blob is never rewritten and never needs to match
   byte-for-byte.
   *Rejected — hash the compressed bytes:* the naive reading, and the one an
   implementer reaching for "hash whatever I'm about to write" would fall into. It
   couples content identity to a library version for no benefit whatsoever, and
   converts every zstd upgrade into a full-document rewrite.
   *Rejected — leave it ambiguous and let `raster_tile_store` decide:* the decision
   determines whether this task must pin a zstd version hard and forbid the system
   path (Decision 5). It cannot be deferred past the wiring it governs.

2. **PRIVATE link — and the honest accounting of what that does and does not buy.**
   `json_dep`'s Constraint 5 (*"Header-only is load-bearing, not incidental"*,
   `json_dep.md:186-190`) is the argument that a compiled zstd **cannot** inherit, so
   it is re-earned explicitly rather than assumed:
   - **The compile/include interface is unconditionally clean.** Constraint 3 —
     no `zstd.h` in any installed header, no zstd type in any exposed signature —
     means **no embedder ever compiles against zstd**, no matter how `libarbc` is
     built. This is the same guarantee nlohmann gets, and it is the one that actually
     protects doc 10's promise, because "transitively impose" is about what an
     embedder must *have and know about*, not about which symbols ended up in an
     archive.
   - **The link interface is clean for a shared `libarbc`, and carries zstd for a
     static one — which is normal and honest.** A shared `libarbc` folds the static
     zstd's symbols in and exposes nothing (hence
     `POSITION_INDEPENDENT_CODE ON`, Constraint 4). A **static** `libarbc` carries
     the zstd requirement to its consumer's final link — exactly as *every* static
     library carries its dependencies, and exactly as it would for any compiled dep.
     That is not a violation of `10:34-35`; that clause forbids imposing **codecs, GPU
     SDKs, or a GUI toolkit** — heavyweight, platform-entangling, format-parsing
     things — and a ~700 KB general-purpose byte-stream compressor with a stable C
     ABI, present in every mainstream distro, is categorically none of them. The
     packaging consequence is real and is `packaging.shared_library_build`'s to
     express (`find_dependency(zstd)` in the exported config when the system copy was
     used, nothing when the pinned static build was folded in) — recorded in
     Acceptance criteria so it is inherited in writing, not rediscovered.
   - **zstd is not a codec, and this is not a crack in the codec line.** The
     distinction doc 17:205-229 actually draws is about **parsing foreign file
     formats**: an image codec means decoders for PNG/JPEG/etc., a large
     attack surface over untrusted third-party formats, platform baggage, and a
     format dependency in core. zstd compresses **bytes we produced ourselves, in a
     container we defined**. It never parses a foreign format. The codec line holds
     unchanged: `org.arbc.raster` stays codec-free and keeps accepting decoded
     buffers; `org.arbc.image`/`imageseq` keep their decoders out of `libarbc`
     (`10:29`). Constraint 2 requires this be stated in the doc-08 delta so a future
     reader cannot cite zstd as precedent for "well, we already took a compression
     library."
   *Rejected — dynamic-link a system zstd unconditionally:* imposes a runtime
   `libzstd.so` requirement on every embedder of a shared `libarbc`, which is the
   transitive burden `10:34-35` is actually about.
   *Rejected — vendor zstd in-tree (the stb-class carve-out):* that carve-out
   (`10:29`, `17:211`) is explicitly and only for **out-of-lib plugin artifacts**. A
   core dep in-tree violates `10:32-33` outright and defeats lockstep version pinning
   — `json_dep` Decision 3 already settled this and it is not reopened.

3. **One `arbc_zstd` INTERFACE target normalizes the two consumption paths.** Unlike
   nlohmann — one target, `nlohmann_json::nlohmann_json`, on both paths — zstd
   presents **different target names** depending on how it materialized: a system
   install via `zstdConfig.cmake` gives `zstd::libzstd_shared` and/or
   `zstd::libzstd_static`; the `FetchContent` build gives `libzstd_static`. Pushing
   that three-way `if(TARGET …)` cascade into `src/serialize/CMakeLists.txt` would
   scatter it, and would scatter again at the next consumer. Instead the top-level
   `CMakeLists.txt` resolves it **once**, immediately after
   `FetchContent_MakeAvailable`, into a single `arbc_zstd` INTERFACE target that
   links whichever real target exists. `src/serialize/CMakeLists.txt` then names
   exactly one thing — `target_link_libraries(arbc_serialize PRIVATE arbc_zstd)` —
   directly beneath the nlohmann line it mirrors. The shim is ~10 lines of CMake with
   one call site today and an obvious second one tomorrow; it is the cheapest thing
   that keeps the component file honest.
   *Rejected — name `zstd::libzstd_static` directly and require the static path:*
   breaks the find-first mandate (`10:32`) on every distro that ships only the shared
   library, and makes a system zstd unusable.
   *Rejected — a full `FindZSTD.cmake` module:* CMake ships no `FindZSTD`, and writing
   one (pkg-config probing, imported-target synthesis, version extraction) is real
   maintenance for a case the `FetchContent` fallback already covers. If no config
   package is found, we build the pinned tag — that *is* the fallback, and it is
   simpler and more deterministic than a hand-rolled finder.

4. **Land the minimal `compress_blob`/`decompress_blob` seam inside
   `arbc::serialize` — deviating, deliberately, from `json_dep`'s "no component code"
   Decision 4.** `json_dep` linked its library into a **standalone** smoke test and
   explicitly declined to create `src/serialize/` (`json_dep.md:280-288`), on the
   grounds that an empty component whose only consumer is a smoke test is abstraction
   ahead of need. That reasoning was right *then* and does not apply *now*, for two
   independent reasons:
   - **`src/serialize/` already exists.** `writer`/`reader`/`kind_params`/`sharing`
     all landed. There is no empty component to avoid creating; there is a real
     component to link onto.
   - **Decisively: a standalone probe cannot prove the thing this task exists to
     decide.** The load-bearing property is *"zstd is PRIVATE to `arbc_serialize` and
     does not escape onto `libarbc`'s interface"* (Decision 2). A test that links zstd
     **directly** exercises zstd — it says nothing whatsoever about `libarbc`'s
     interface. The only construction that actually proves it is a test that links
     **`arbc` alone** and calls a seam that internally uses zstd: that compiles only
     if no zstd type leaked into a public header, and links only if the PRIVATE
     linkage is genuinely right. For a *header-only* dep this distinction was
     academic, which is why `json_dep` could take the cheaper shape. For a *compiled*
     dep it is the entire question.
   So the seam is the test fixture *and* the deliverable. It stays deliberately
   minimal — raw zstd frame in, raw zstd frame out, ~50 lines: no shuffle, no
   hashing, no tile geometry, no I/O. `serialize.raster_tile_store` composes
   shuffle → `compress_blob` → hash-named file on top of it.
   **The levelization forces this shape anyway, which is a good sign it is right.**
   `arbc::serialize` and `arbc::kind-*` are *both* Level 4 (`17:59-60`), and doc 17's
   rule is *"A component may depend only on strictly lower levels. No same-level
   edges"* (`17:42-43`) — so `serialize` **cannot see `RasterContent` at all**. A
   seam here could not be tile-aware even if we wanted it to be: it must trade in
   raw bytes. (This is the same constraint that puts the `org.arbc.raster` *codec* TU
   in `runtime` (L5) — the only layer that legally sees both a concrete kind type and
   the serialize internals, per `src/runtime/arbc/runtime/builtin_codecs.hpp:8-13` and
   `kind_params.md` Decision 3. `raster_tile_store` inherits that placement; it is not
   this task's to arrange, but it confirms the byte-oriented seam is the only shape
   the component graph permits.)
   *Rejected — the `json_dep` standalone-probe shape:* cheaper by an hour, and proves
   nothing about the only property in question.
   *Rejected — also land the byte-shuffle here:* it is a pure, self-contained,
   testable transform with no dependency on zstd at all, and doc 10:27 is explicit
   that *"the shuffle is ours, not the library's."* It belongs with the tile store
   that gives it meaning (and whose `.tji` note already assigns it,
   `tasks/60-serialize.tji:70(e)`). Pulling it forward would blur a dependency-wiring
   gate into a compression-design task and inflate a 0.5d gate.

5. **Find-first stays genuinely find-first: CI may resolve a system zstd, and that is
   safe — *because* of Decision 1.** This is the one place where mirroring the JSON
   wiring produces a materially different outcome, and it deserves to be on the
   record rather than discovered. CI pins nothing today: there is no apt/vcpkg/Conan
   step anywhere (`ci.yml:89-97`), and "pinned in CI" (`10:33`) works for nlohmann
   only *by accident* — no system nlohmann exists on the runners, so
   `FIND_PACKAGE_ARGS` finds nothing and the `GIT_TAG` is what gets built. **zstd is
   different**: `libzstd-dev` is commonly present on `ubuntu-latest` images, so the
   Linux lanes may well resolve the **system** copy while the Windows lane builds the
   **pinned** one. Two zstd versions across the CI matrix.
   That is **fine, and it is fine only because Decision 1 holds.** Since blob bytes
   are not content identity, nothing observable depends on which zstd built: the
   frame format is standardized (RFC 8878) and forward/backward compatible, the four
   functions used (`ZSTD_compress`, `ZSTD_decompress`, `ZSTD_compressBound`,
   `ZSTD_isError`) are ABI-stable across the entire 1.x line, and every assertion this
   task and `raster_tile_store` make is over **decompressed** bytes, which are
   version-invariant by the format's own guarantee. Version drift is genuinely
   unobservable — and letting the distro path actually run in CI is *valuable*, since
   it is the path most downstream packagers will take, and the one most likely to rot
   unnoticed. So: declare a **minimum** version (`find_package(zstd 1.5)`, for the
   config package and stable target names) and a **pinned `GIT_TAG` fallback**; do
   **not** force the fetched copy. Any lane needing determinism can set
   `FETCHCONTENT_TRY_FIND_PACKAGE_MODE=NEVER` — the knob is documented in the CMake
   comment; no new CI lane, no new pinning machinery.
   Had Decision 1 gone the other way — hash over compressed bytes — this would invert
   completely: blob names would depend on the zstd version, CI would have to force one
   exact build on every lane, and a system zstd would become a correctness hazard.
   The two decisions are the same decision, and that is why they are made together.
   *Rejected — force `FETCHCONTENT_TRY_FIND_PACKAGE_MODE=NEVER` globally in CI:* a
   blunt global knob that would also detach Catch2 and nlohmann from any system copy,
   slow every lane with extra clones, and — worst — retire the distro-consumption path
   from CI coverage entirely, which is the path doc 10:32 actually mandates for
   downstreams.
   *Rejected — `apt-get install libzstd-dev` pinned to an exact version in CI:* pins
   one lane's platform, cannot be mirrored on Windows, and buys determinism that
   Decision 1 already made unnecessary.

6. **`decompress_blob` takes its output bound from the caller and never from the
   frame header.** The natural implementation reads `ZSTD_getFrameContentSize` and
   allocates it. On the loader's path that value is **attacker-controlled**: a
   hand-edited `.arbc`'s tile blob can claim to expand to 64 GB, and the reader
   OOMs before a single byte is validated — a one-line denial of service in a
   surface `serialize.format_tests` already fuzzes. The caller always *knows* the
   true size (tile edge × storage format's bytes-per-pixel, both declared in the
   document and validated independently), so it passes it, the seam allocates
   exactly that, and a frame that does not produce exactly that many bytes is an
   error value. `ZSTD_getFrameContentSize`'s sentinel returns
   (`ZSTD_CONTENTSIZE_UNKNOWN`, `ZSTD_CONTENTSIZE_ERROR`) become non-issues because
   the value is never consulted.
   *Rejected — trust the frame header, cap it at some large constant:* a cap still
   admits a blob claiming exactly the cap, still allocates far more than any tile
   needs, and leaves a magic number where a known-exact value was available for free.

7. **No doc-00 decision-record bullet.** Doc 00 already carries both halves: the
   dependency-policy bullet (*"minimal-vetted-deps core (codecs and GPU APIs live in
   plugins/backends)"*, `00:125-127`, decided in doc 10) and the Principle-8 bullet
   (*"Imported images are referenced; painted pixels are stored"*, `00:356-375`),
   which already states the storage decision **and** the honest compression bound
   (*"no compressor closes that gap … Compression is the weakest lever here — dedup
   beats it"*). Filling an already-sanctioned dependency slot with the library doc
   10:27 already names is not a newly project-shaping decision — the same reasoning
   `json_dep` Decision 5 applied. The doc-08 and doc-17 deltas are the right and only
   homes.
   *Rejected — a doc-00 bullet for "the second core dependency":* the *count* of core
   dependencies is not itself a project-shaping fact; the *policy* that governs them
   is, and doc 00 already records it.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-14.

- `CMakeLists.txt`: wired zstd as a `FetchContent_Declare` block (find-first with pinned GIT_TAG fallback, `SOURCE_SUBDIR build/cmake`, SYSTEM-marked includes, programs/tests/shared/legacy knobs off, `POSITION_INDEPENDENT_CODE ON` on the fetched static target) and resolved to a single `arbc_zstd` INTERFACE target normalizing the system vs. fetched name split.
- `src/serialize/arbc/serialize/blob_compress.hpp`: minimal public seam — `compress_blob(span) → vector<byte>` and `decompress_blob(span, size) → expected<vector<byte>, BlobError>` with `BlobError` values (`CorruptFrame`, `SizeMismatch`, `CompressError`); no zstd types in any installed header (Constraint 3 holds by construction).
- `src/serialize/blob_compress.cpp`: implementation using one-shot ZSTD API (stateless, thread-safe); `decompress_blob` allocates the caller's declared bound and never consults the frame header, closing the remote-OOM risk on the untrusted loader path.
- `src/serialize/CMakeLists.txt`: `target_link_libraries(arbc_serialize PRIVATE arbc_zstd)` added beneath the nlohmann line; the link is PRIVATE and zstd does not escape onto libarbc's public interface.
- `tests/serialize_zstd_dep_smoke.t.cpp`: 2 TEST_CASEs, 41 assertions, links `arbc` alone (never `arbc_zstd`/`zstd::*`), proving PRIVATE-link and no-leaked-header by construction.
- `tests/CMakeLists.txt`: `arbc_serialize_zstd_dep_smoke_t` registered (links `arbc` + Catch2).
- `tests/claims/registry.tsv`: 2 new claims — `08-serialization#tile-blob-codec-round-trips-byte-exactly` and `08-serialization#tile-blob-decompress-is-bounded-and-fails-as-a-value`.
- `docs/design/08-serialization.md`: §Dependency note extended to cover both core deps; Principle 8 blob bullet clarified (content hash is over uncompressed bytes — compression is a storage encoding, never content identity).
- `docs/design/17-internal-components.md`: `arbc::serialize` row external-dep column updated to `JSON dep, compressor (zstd)`; §The codec line paragraph added stating zstd does not cross the codec line.
- `src/pool/arbc/pool/refs.hpp`, `src/pool/arbc/pool/typed_store.hpp`: `TypedStore::destroy` split into `destruct` + ordered `reclaim` (destruct → bump generation → release slot) to close a false-positive TSan tripwire window that `model.per_object_revision` HAMT churn had widened.
- Zero warnings under `-Wall -Wextra -Wpedantic -Werror` on all CI lanes; all claims and levelization checks green.
