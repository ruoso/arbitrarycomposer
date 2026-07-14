# kinds.image_master_budget — `org.arbc.image` byte-budgeted LRU pyramid eviction

## TaskJuggler entry

`tasks/55-kinds.tji:41-46` — `task image_master_budget "org.arbc.image
byte-budgeted LRU pyramid eviction"`, under `task kinds "Reference kinds"`
(55 — Reference kinds, docs 03/05/11/12/17). Milestone: **M9**
(`tasks/99-milestones.tji:72`).

```tji
task image_master_budget "org.arbc.image byte-budgeted LRU pyramid eviction" {
  effort 2d
  allocate team
  depends !image
  note "Byte-budgeted eviction of decoded pyramids from the plugin-side pyramid
  cache. A 24 MP rgba32f master plus mips is ~512 MB resident; a composition
  referencing many distinct photographs will exceed any sane budget. Add an LRU
  over the plugin-side pyramid cache under a configurable byte budget,
  re-decoding on demand, pinned by the existing decodes_issued() counter (an
  evicted-then-re-pulled image issues exactly one further decode; a re-pull
  within budget issues zero). Deliberately NOT in kinds.image: it needs the
  residency counter that task lands as its measuring instrument.
  Source-of-debt: tasks/refinements/kinds/image.md (Decision 4), commit
  kinds.image. Docs 03/17."
}
```

## Effort estimate

**2d.** The LRU itself is small — an intrusive recency list, a byte total, and
an `evict_to_fit` loop over `Pyramid::resident_bytes()` (~150 lines inside
`PyramidCache`). The cost is not the LRU; it is the **ownership inversion** it
forces (Decision 1) and the two enforced claims it re-opens (Decisions 3 and 6).
The estimate holds because eviction turns out to be **model-invisible**
(Decision 5) — no revision bump, no damage route, no `runtime` edge, no model
edge — so the entire diff lands inside `plugins/image/` plus two design-doc
deltas and three claim rows.

*If Decision 2 is reversed — if the encoded bytes are not retained and a
re-decode has to re-fetch through `AssetSource` — this is a 5d task and it stops
being plugin-local.*

## Inherited dependencies

Declared edge (`tasks/55-kinds.tji:44`): `depends !image`.

### Settled (may be relied on)

- **`kinds.image`** (`complete 100`, `tasks/refinements/kinds/image.md`). Shipped
  the kind out-of-lib at `plugins/image/`, with:
  - `class Pyramid` (`plugins/image/arbc/kind_image/image_content.hpp:78-107`) —
    an immutable mip pyramid, level 0 the decoded master in
    `Rgba32fLinearPremul`, each rung a 2:1 Lanczos-3 half-band decimation through
    `media`'s frozen bank (`image_content.cpp:61-126`). Plain heap
    (`std::vector<float> px`, `:101`), no CoW, no `StateHandle`, no pool backing,
    no versioning (image.md **Decision 4**).
  - **`Pyramid::resident_bytes()`** (`image_content.hpp:93-95`, impl
    `image_content.cpp:53-59`) — annotated in-source as "*the measuring
    instrument a later byte-budgeted eviction (`kinds.image_master_budget`)
    needs*". **This is the whole of the promised residency instrument** (see the
    correction under *Inputs*): it is a per-`Pyramid` size, and `PyramidCache`
    has no aggregate.
  - `class PyramidCache` (`image_content.hpp:121-136`) — `resolve(resolved_uri,
    encoded)` (`:126`, impl `image_content.cpp:130-152`), `decodes_issued()`
    (`:130`), a `std::mutex` (`:133`), and
    `std::unordered_map<std::string, std::weak_ptr<const Pyramid>> d_by_uri`
    (`:134`). **The entries are `weak_ptr`.**
  - `default_pyramid_cache()` — the process-wide instance the registered factory
    resolves through (`image_content.hpp:141`, a function-local static at
    `image_content.cpp:159-162`), with **no** constructor argument.
  - `decodes_issued()` — the behavioral counter, incremented at exactly one site
    (`image_content.cpp:149`), and **only on a genuine, successful decode**
    (an undecodable arrival costs zero; image_async_pending.md Status).
  - Claim `03-layer-plugin-interface#image-decodes-once-per-resolved-uri`
    (`tests/claims/registry.tsv:300`), pinned by
    `tests/image_content.t.cpp:109-137`.
- **`kinds.image_async_pending`** (`complete 100`,
  `tasks/refinements/kinds/image_async_pending.md`). Not a declared edge, but it
  ships the seam this task consumes and the invariant this task re-opens:
  - **`Content::install_asset(std::string_view)`**
    (`src/contract/arbc/contract/content.hpp:645-666`, beside
    `external_asset_ref()` at `:643`) — the writer-thread, pixels-arrive-later
    channel. image_async_pending.md:148-151 states outright:
    "**It unblocks `kinds.image_master_budget`.** Eviction and re-decode need a
    channel to push pixels into a live content after construction. This task
    builds that channel and pins its discipline; the budget task **inherits it
    rather than inventing a second one**."
  - `ImageContent::d_pyramid` as `std::atomic<std::shared_ptr<const Pyramid>>`
    (`image_content.hpp:261`), published **monotonically, at most once**
    (image_async_pending.md **Decision 6**) — the invariant Decision 3 below
    re-opens, exactly as that refinement instructed its successor to.

### Pending (must NOT be assumed)

Nothing this task needs is unbuilt. It touches no model, runtime, or serialize
seam (Decision 5), so no `model.*` / `runtime.*` leaf gates it.

## What this task is

Give the plugin-side pyramid cache a **configurable byte budget** and an **LRU
eviction policy** over decoded pyramids, re-decoding on demand when an evicted
image is pulled again.

Doing that honestly requires one structural change the `.tji` note does not name,
and it is the substance of the task: **`PyramidCache` must become the owner of
the decoded pyramids.** Today it holds `weak_ptr`s (`image_content.hpp:134`) and
`ImageContent` holds the strong reference (`:261`), so *evicting a cache entry
frees nothing* — it only de-dupes worse. An LRU over `weak_ptr`s is a no-op with
a counter attached. The cache takes the strong reference; the content resolves
its pixels through a **pin** for the duration of a render.

It is **not**:

- A *tiled* or *level-granular* pyramid store. Eviction granularity is the whole
  pyramid, as the `.tji` note specifies. Per-level eviction (level 0 is ~3/4 of
  the bytes, and a zoomed-out view needs only the small upper rungs) is a real
  and better policy, deferred as a named leaf under *Acceptance criteria*.
- A change to any rendered pixel. A re-decode is **byte-identical** to the
  original decode (Decision 4), and that is the property the goldens pin.
- A model-visible event. Eviction bumps no revision and emits no damage
  (Decision 5).
- A move onto `arbc::cache`'s `KeyedStore`. Doc 17 forbids the edge and
  `KeyedStore` is not thread-safe (Decision 7).

## Why it needs to be done

- **The kind is unusable at scale without it, and that is arithmetic, not
  taste.** A 24 MP master at `rgba32f` is 24e6 × 16 B ≈ **384 MB**; the mip chain
  adds ~1/3, so **~512 MB resident per photograph** (`tests/image_content.t.cpp:100`
  already asserts `resident_bytes() < 2 * master`). `kinds.image`'s own
  justification for existing is a **30-layer 24 MP composition** (`image.md`,
  "~490 MB against ~32 MB"). Thirty resident pyramids is **~15 GB**. The kind
  that was added to make large photographic compositions affordable to *store*
  currently makes them impossible to *open*.
- **It discharges a registered source-of-debt.** `image.md:541-548` registered
  this leaf against its Decision 4, and named the instrument
  (`Pyramid::resident_bytes()`) it would be measured with.
  `image_async_pending.md:585-588` confirmed the one forward tension — pyramid
  eviction re-opening publish-once — "belongs to `kinds.image_master_budget`".
  Both predecessors are `complete 100`; this leaf closes the loop they left open.
- **The policy it implements is already designed — for a different component.**
  Doc 02:255-284 settles budgeted-LRU-with-residency-pin in full, and doc
  15:129-131 states the governing principle: "*cache budgets (docs 02/12) decide
  how much **derived** data stays alive*". A decoded pyramid is derived data —
  re-derivable, byte-for-byte, from the encoded bytes. This task applies a
  settled policy to a population the docs never explicitly covered, which is why
  it carries a doc 15 delta rather than an invention.

## Inputs / context

### Governing design-doc sections (normative — doc 16)

- **doc 02 — Architecture, "Tile cache" (`02:255-284`).** The eviction policy
  this task mirrors, and the reason it needs no new design:
  - `02:260-261`: "Budgeted (bytes), LRU within priority classes".
  - `02:268-277` — **Residency pin vs. payload refcount**: "An entry a frame is
    about to composite from **must not be evicted mid-frame**, so a lookup (or a
    just-completed insert) yields a *pinned* hold on the entry; **eviction skips
    pinned entries**". This is the memory-safety rule of the whole task.
  - `02:278-284` — **"The byte budget is a soft target, not a hard allocator
    cap."** "The **pinned working set is never dropped** to honor the budget —
    correctness (serving the tiles the current frame needs) outranks the budget —
    so **resident bytes may transiently exceed the budget** when the pinned set
    alone does." Inherited verbatim (Constraint 4).
- **doc 15 — Memory Model.** `15:117` — "*Version reclamation: refcounts as the
  GC, **budgets as the policy***". `15:129-131` — "The only tunables are already
  designed: the journal byte budget (doc 14, `state_cost`) decides how much
  *history* stays alive; **cache budgets (docs 02/12) decide how much *derived*
  data stays alive**." `15:21` (the *Cache values* population) — "backend-owned,
  **budgeted, LRU** … budgets are the eviction policy". **The gap this task
  fills:** every budget the docs settle is a *cache* budget (tiles, blocks) or the
  *journal* budget. **No doc covers a budget over a decoded source asset held
  inside a kind.** Hence the doc 15 delta.
- **doc 03 — Layer/Plugin Interface, `install_asset` (`03:143-161`).** The
  invariant this task amends, verbatim:
  > "A kind that DOES override it must publish the decoded result **ATOMICALLY
  > and AT MOST ONCE** — so a worker rendering an earlier pinned revision
  > observes either the pre-arrival state or the final one, never a partial
  > install. That obligation is what lets an overriding kind keep
  > `render_thread_safe() == true` … It is **monotonicity, not immutability**,
  > that buys the flag … `org.arbc.image` keeps the flag on exactly these terms."

  A re-decode after eviction is a **second** publish and a **non-null → null →
  non-null** transition. Doc 03 as written forbids it. Hence the doc 03 delta
  (Decision 3).
- **doc 17 — Internal Components.** Levelization (`17:52-55`, table `:57-72`):
  a kind is **L4** and its **only** granted direct edge is `contract` (`17:70`);
  the legal reach is `contract`'s transitive closure (`17:74-86`) — which
  includes `pool`, `media`, `surface`, `model`, and **does not include
  `arbc::cache`** (`17:65`: `arbc::cache` depends on `base`, `surface`; only
  `compositor` / `audio_engine` / `runtime` are granted an edge to it).
  `backend-cpu` is explicitly forbidden to kinds (`17:111-114`). Counters are
  component-owned plain `std::uint64_t` with `noexcept` accessors, and
  `arbc::cache` publishes `hits()/misses()/evictions()` (`17:124-138`) — the
  naming this task mirrors.
- **doc 16 — SDLC and Quality.** Behavioral counters, never wall-clock
  (`16:54-62`); claims register format and the `enforces:` tag (`16:14-21`);
  same-commit design-doc rule (`16:23-25`).

### Source seams this task extends

- `plugins/image/arbc/kind_image/image_content.hpp`
  - `Pyramid::resident_bytes()` (`:93-95`) — the cost function. Used as-is.
  - `class PyramidCache` (`:121-136`) — **the primary surface**. `d_by_uri`
    (`:134`) inverts from `weak_ptr` to a strong owning entry; gains a budget, an
    LRU recency order, a resident-byte total, and `evictions()` /
    `resident_bytes()` accessors.
  - `PyramidCache::resolve()` (`:126`, impl `image_content.cpp:130-152`) — the
    single decode site (`++d_decodes_issued` at `:149`, `emplace` at `:150`).
    Becomes a pin-returning lookup that decodes on a miss and evicts to fit.
  - `default_pyramid_cache()` (`:141`, impl `:159-162`) — gains the configured
    default budget.
  - `ImageContent::d_pyramid` (`:261`), `pyramid()` (`:247`), `available()`
    (`:244`) — the ownership site (Decision 1).
  - `ImageContent::bounds()` (`:213`, impl `image_content.cpp:239`) — reads the
    pyramid today; **must stop doing so** (Constraint 2).
  - `ImageContent::install_asset()` (`:241`, impl `image_content.cpp:209-236`) —
    the inherited re-install seam. Its in-source comment at
    `image_content.cpp:213-215` already anticipates this task by name:
    "*`kinds.image_master_budget` will generalize this discipline when it makes
    eviction re-open the slot; it inherits this seam rather than inventing a
    second one.*"
  - `ImageContent::render()` (impl `image_content.cpp:250-328`) — one acquire
    load at `:255`, and **render never decodes today**. It will (Decision 4).
- `plugins/image/image_content.cpp:369` — `make_image_content`, the second and
  only other `resolve()` caller.
- `src/runtime/document_serialize.cpp:593` — `content->install_asset(arrival.bytes)`,
  the writer-thread settle. Untouched by this task, and must stay green.

### Precedent this task copies rather than invents

- **The byte-budget spelling.** `Journal` (`src/model/arbc/model/journal.hpp:56-61`,
  `:104`): a defaulted ctor budget with a `k_no_budget =
  numeric_limits<size_t>::max()` sentinel, a cost function on the object, and a
  trim loop that goes "*until within budget, never below one entry*". Copied
  verbatim in shape (Decision 8). `offline_sequence.hpp:79` is the
  named-default-constant precedent (`cache_budget_bytes = k_default_...`).
- **The eviction/pin semantics.** `src/cache/arbc/cache/keyed_store.hpp` —
  `resident_bytes()/budget()/evictions()` (`:191-195`), `victim()` (`:229-249`),
  `evict_to_fit()` (`:251-261`), `CacheHold` as an RAII pin excluding an entry
  from eviction candidacy (`:78`). Copied in **shape and naming**, not by
  dependency (Decision 7).
- **Env-var configuration.** `ARBC_PLUGIN_PATH` (claim
  `03-layer-plugin-interface#plugin-path-scan-is-opt-in`,
  `tests/claims/registry.tsv:273`) is the house precedent for configuring a
  plugin-tier behavior the host cannot reach through an ABI.

### A correction to the inherited note (read this before starting)

`image.md`'s deferred-follow-up text and the `.tji` note both say this task
"needs the **residency counter** that task lands as its measuring instrument."
**No such counter was landed.** What shipped is `Pyramid::resident_bytes()` — a
per-object size method with **no claim, no test, and no aggregate on
`PyramidCache`** (which has no `resident_bytes()`, no budget, no LRU). The
instrument is a cost *function*, not a residency *counter*. This task lands the
cache-level `resident_bytes()` aggregate itself; do not go looking for one.

## Constraints / requirements

1. **The cache must own the pyramids; nothing else may hold a persistent strong
   reference.** This is the load-bearing constraint. A byte budget over a cache
   that does not own its values is a lie: while any `ImageContent` holds a strong
   `shared_ptr`, evicting the map entry frees zero bytes. After this task, the
   *only* persistent strong reference to a budgeted pyramid is the cache's; every
   other reference is a transient **pin** whose lifetime is one `render()` call.
   (The one exception is the un-keyed content — see Constraint 6.)

2. **Eviction must be invisible to `bounds()`, and `bounds()` must never
   decode.** `bounds()` is on the compositor's cull path. If an evicted image
   reported empty bounds, **it would cull itself out of the composition** and a
   photograph would vanish because memory got tight — the worst possible failure
   mode. So the content must retain the decoded **extent** (`width`, `height`)
   independently of pixel residency, set once when the pixels first arrive and
   **never cleared by eviction**. `bounds()` reads that extent and takes no pin,
   no lock, and issues no decode. `available()` likewise becomes "*this content
   has a known extent*", not "*its pyramid is resident*".

   This does **not** contradict `08-serialization#image-serializes-as-uri-only`
   ("no intrinsic size enters the document"). That claim governs **serialization**
   — `params` stays exactly `{"source": <uri>}`. An in-memory extent is not
   document state, and the layer still re-saves byte-identically.

3. **A re-decode must be byte-identical to the original decode.** `Pyramid::decode`
   (`image_content.cpp:61-126`) is a pure function of the encoded bytes over frozen
   `media` kernels. It must stay one. This is what makes eviction unobservable in
   pixels — and it is what lets the *existing* goldens, re-run under a
   forced-eviction cache, serve as the proof (Acceptance).

4. **The budget is a soft target; a pinned pyramid is never evicted.** Inherited
   verbatim from doc 02:268-284. A pin taken for an in-flight render excludes its
   entry from eviction candidacy, and — because the pin is an owning
   `shared_ptr` — even a *removed* entry's pixels survive until the last pin
   drops. Resident bytes may transiently exceed the budget when the pinned set
   alone does. **Correctness outranks the budget**: a lone pyramid larger than the
   entire budget stays resident and renders, exactly as `Journal` never trims
   below one entry (`journal.hpp:104`).

   The pinned working set is bounded by **concurrent renders, not by layer
   count**: a pin lives for one `render()` call, so a 30-photograph composition
   pins O(worker count) pyramids at once, not 30.

5. **The encoded bytes must be retained by the cache** (Decision 2). Without them
   a re-decode is impossible, because **this plugin performs no file I/O at all**
   (`image_content.hpp:42-45`) and has no `AssetSource` reach. They are retained
   once per resolved URI (shared across the N contents that dedup on it) and are
   **not** part of the evictable budget: they are the *source*, the pyramid is the
   *derived* data doc 15:129-131 says budgets govern.

6. **The un-keyed content path must keep working, unbudgeted.** The two-argument
   `ImageContent(authored_uri, pyramid)` (`image_content.hpp:204`) builds a
   content with an **empty `d_resolved`** — a caller handing over an
   already-decoded pyramid, which several tests do. It has no cache identity, so
   there is nothing to re-decode from and it must keep owning its pyramid
   strongly and un-evictably. Both paths resolve pixels through one accessor
   returning a pin; for this one the pin simply wraps the owned pointer.

7. **`render_thread_safe()` stays `true`, and the reasoning must be re-stated,
   not assumed** (Decision 3). It survives on three legs: (a) a `Pyramid` object
   is **immutable for its entire life** and eviction never mutates one, it only
   drops a reference; (b) a pin is an **owning** `shared_ptr`, so a concurrent
   eviction can never free pixels a render is reading; (c) `PyramidCache` is
   mutex-guarded. What it no longer rests on is *monotonicity of the content's
   pointer* — that leg is what this task removes.

8. **No new component edge, no new third-party dependency** (doc 10 policy). The
   LRU is built inside `PyramidCache`. `plugins/image/CMakeLists.txt` gains
   nothing. In particular, no `arbc::cache` include (Decision 7) — even though
   `check_levels.py` globs only `src/*/CMakeLists.txt` and would **not** catch it
   (the plugin links the `arbc` umbrella PUBLIC at
   `plugins/image/CMakeLists.txt:33`). The constraint here is doc 17's normative
   table, not the script; honor it because it is the design, not because a
   checker would fail.

9. **RT-safety is not at issue, and the implementation must not pretend it is.**
   `org.arbc.image` has no audio facet, so it is never pulled on the audio RT
   thread. Decodes and frees land on compositor workers, where allocation is
   already routine (`TileStore::acquire` allocates surfaces in `render` today,
   `image_content.cpp:171-184`). `scripts/check_rt_safety.py` must stay green,
   which it will.

## Acceptance criteria

### Conformance suite (mandatory for a content kind — doc 16)

`tests/image_conformance.t.cpp` runs `arbc::contract_tests(factory)` and stays
green — **and gains a second run of the entire suite against a cache whose budget
is 1 byte**, so every single pull evicts and every single render re-decodes. The
kind's contract behavior (render-scale honesty, within-bounds, undamaged-region
stability, static-time invariance, facet consistency) must be **invariant under
eviction pressure**. This is the strongest single check in the task: if any
conformance property depends on pixel residency, this run finds it.

### New claims-register entries (`tests/claims/registry.tsv` + an `enforces:`-tagged test)

| Claim id | What it pins |
|---|---|
| `15-memory-model#image-pyramid-evicts-under-byte-budget` | **Behavioral counter.** The plugin-side pyramid cache owns its decoded pyramids strongly and bounds them by a configurable byte budget, evicting **LRU** and skipping **pinned** entries (doc 02:268-284, doc 15:129-131). Under a budget admitting exactly two of three pyramids: a third decode evicts the least-recently-pinned one (`evictions()` == 1, `resident_bytes()` <= budget); **re-pulling the evicted image issues EXACTLY ONE further decode** (`decodes_issued()` + 1) and **re-pulling a still-resident one issues ZERO** (`decodes_issued()` unchanged); a pyramid **larger than the entire budget stays resident and renders** (the budget is a soft target — correctness outranks it, doc 02:278-284, mirroring `Journal`'s never-below-one-entry rule); and a **pinned** entry is never chosen as a victim while its pin is live. Never a wall-clock assertion (doc 16:54-62). |
| `03-layer-plugin-interface#image-re-decode-is-byte-identical-and-model-invisible` | An evicted-then-re-decoded `org.arbc.image` renders **BYTE-IDENTICAL** pixels to the same image rendered while resident — the decode is a pure function of the retained encoded bytes over `media`'s frozen kernels — and eviction is **model-invisible**: it bumps NO revision, emits NO damage, and mutates no model state, so composed tiles keyed on `(content id, revision, …)` (doc 02:257) stay valid across it, because nothing a composed tile depends on changed. Residency is not composition state. |

### Amended claims-register entries (rows rewritten; the design-doc deltas ride the same commit)

Both existing rows are **falsified** by this task as written and must be edited,
not merely re-enforced:

- **`03-layer-plugin-interface#image-decodes-once-per-resolved-uri`**
  (`registry.tsv:300`). Today it says "re-loading or re-rendering an unchanged
  image issues **ZERO** further decodes." Under a byte budget that is no longer
  unconditionally true. The row is rescoped: the resolved-identity dedup (N
  authored spellings → one decode) is **unchanged and unconditional**; the
  zero-further-decodes half becomes "**while the pyramid remains resident**", and
  gains its complement — an evicted image costs exactly one further decode on the
  next pull. `tests/image_content.t.cpp:109-137` (which already re-decodes once
  after the last content dies, and whose comment at `:131-134` names this task) is
  the natural home for the amendment.
- **`03-layer-plugin-interface#image-pyramid-publishes-once`**
  (`registry.tsv:307`). Today it asserts the pyramid is published "monotonically
  (null -> non-null, **never reverting** and never replaced)" and that this is
  what keeps `render_thread_safe()` true. Eviction + re-decode is precisely a
  non-null → null → non-null transition. Amended (Decision 3): what publishes
  once, monotonically, is the content's **extent**; the **pixels** become
  budgeted derived data that may be evicted and re-derived byte-identically, and
  `render_thread_safe()` rests on pyramid immutability + owning pins +
  cache mutex (Constraint 7). The `enforces:`-tagged test in
  `tests/image_concurrency_stress.t.cpp` is rewritten to that shape.

### Re-enforced claims (second `enforces:` tag — **no new row**)

- `03-layer-plugin-interface#image-has-no-editable-facet` — the read-only shape is
  untouched.
- `08-serialization#image-serializes-as-uri-only` — an evicted image still
  re-saves byte-identically; the retained extent is in-memory, never in `params`
  (Constraint 2).
- `08-serialization#unavailable-asset-is-not-a-read-error` — **the sharp edge.**
  An *evicted* image is **not** an *unavailable* one: it keeps non-empty bounds
  and renders pixels. Only a genuinely unavailable asset (no bytes / undecodable)
  has empty bounds. The test must assert the two states are distinguishable.
- `08-serialization#pending-asset-installs-live` — the async settle
  (`document_serialize.cpp:593`) still publishes one revision and one damage
  batch, over the reworked `install_asset`.
- `09-surfaces-and-backends#image-provided-surface-covers-requested-region` — the
  provided surface still covers the requested region, resident or re-decoded.

### `tests/image_budget.t.cpp` (new) — behavioral

1. Three distinct fixtures, a budget admitting two → the third decode evicts the
   LRU victim: `evictions() == 1`, `resident_bytes() <= budget` — *pins*
   `#image-pyramid-evicts-under-byte-budget`.
2. Re-pull the evicted URI → `decodes_issued()` advances by **exactly 1**; re-pull
   a resident URI → advances by **exactly 0** — *pins* the same, and the amended
   `#image-decodes-once-per-resolved-uri`.
3. Recency is by **pin**, not by insert order: pin A, then B, then A again; admit
   C past budget; **B** is the victim, not A.
4. A single pyramid exceeding the whole budget stays resident and renders (soft
   budget) — *pins* the same.
5. Eviction while pinned: hold a pin, force eviction of that key, assert the
   pixels remain readable and byte-correct through the pin, and that the entry was
   **not** chosen as a victim while pinned (doc 02:268-277).
6. `bounds()` of an evicted image is **unchanged and non-empty**, and reading it
   issues **zero** decodes (`decodes_issued()` unmoved) — *pins* Constraint 2, the
   vanishing-layer regression.
7. `bounds()` of a genuinely unavailable image is still empty — *re-enforces*
   `#unavailable-asset-is-not-a-read-error`.

### Byte-exact goldens

`tests/image_goldens.t.cpp` — its four existing pins (native, a downscale rung, an
`Exact` upscale, a `BestEffort` upscale) are **re-run against a forced-eviction
cache** (budget = 1 byte) and must produce **byte-identical** output to the
resident run. **No new golden data is authored** — the existing goldens *are* the
oracle, which is exactly the point: eviction is a no-op in pixel space. Tolerances
are not used (doc 16 makes tolerance the justified exception, never the default).

### Concurrency (TSan/ASan — doc 16, concurrency-touching task)

`tests/image_concurrency_stress.t.cpp` is extended. The task turns a
construction-time-only cache into one **touched from every render thread**, and
the memory-safety risk it introduces is precise: **evict-while-pinned must not
free.** Cases:

1. N worker threads render the same image concurrently under a budget that forces
   continuous eviction; every rendered tile is compared **byte-for-byte against
   the resident reference**, so a torn read or a half-built pyramid surfaces as a
   *pixel mismatch*, not merely as a TSan report (the discipline
   `#image-pyramid-publishes-once` already established).
2. A racing evictor: one thread pins/renders while another admits fresh decodes
   that force `evict_to_fit` to run. Assert no use-after-free (ASan) and no data
   race (TSan) — the pin is an owning `shared_ptr`, so an evicted-but-pinned
   pyramid outlives its cache entry.
3. The existing "*8 threads race `resolve()` on one URI → `decodes_issued() == 1`*"
   case (`tests/image_concurrency_stress.t.cpp:255-287`) stays green **unchanged**:
   one decode per key under concurrency is preserved by keeping the decode under
   the cache mutex (Decision 9).
4. The existing late-`install_asset` races (`:353`, `:382`) stay green over the
   reworked install.

Standing caveat inherited from `pool/big_block_pool.md:279-283`: the repo has no
per-push TSan CI *lane*; the stress runs under the asan lane meanwhile.

### Gates

≥90% diff coverage on changed lines; `scripts/gate` green (configure, build,
ctest, clang-format, `check_levels.py`, `check_claims.py`, `check_rt_safety.py`);
`tests/image_containment.t.cpp` still green (no decode symbol leaks into
`libarbc`); `tj3 project.tjp 2>&1 | grep -iE "error|warning"` silent.

### Deferred follow-ups (closer registers in WBS)

- **`kinds.image_level_granular_eviction`** — *3d, M10, depends
  `!image_master_budget`.* Evict **individual mip levels**, not whole pyramids.
  Level 0 is ~3/4 of a pyramid's bytes, and a zoomed-out view of a large
  composition reads only the small upper rungs — so whole-pyramid eviction makes a
  30-photograph composition thrash (each layer's render evicts the previous, and
  `decodes_issued()` grows without bound) in exactly the case where keeping every
  pyramid's *upper rungs* resident would cost a few MB and issue zero re-decodes.
  Requires per-level residency accounting and a re-decode path that rebuilds only
  the missing rungs from the retained encoded bytes. Source-of-debt: this
  refinement, Decision 6.
- **`kinds.image_decode_in_flight`** — *2d, M10, depends `!image_master_budget`.*
  Replace the decode-under-the-cache-mutex discipline (Decision 9) with a per-key
  in-flight promise: concurrent pins on **distinct** URIs decode in parallel
  instead of serializing behind one global mutex, while concurrent pins on **one**
  URI still wait on a single decode (preserving `decodes_issued() == 1`). Matters
  because this task moves decoding onto render workers, where a ~24 MP re-decode
  under the global lock stalls every other image render in the frame. A latency
  refinement, not a correctness gap — hence M10. Source-of-debt: this refinement,
  Decision 9.

## Decisions

### Decision 1 — The cache owns the pyramids strongly; the content holds a cache identity and takes a transient pin

`PyramidCache::d_by_uri` inverts from
`unordered_map<string, weak_ptr<const Pyramid>>` to a map of owning entries:

```
struct Entry {
  std::shared_ptr<const std::vector<unsigned char>> encoded;  // the source (Decision 2)
  PyramidPtr resident;        // the DERIVED data the budget governs; null when evicted
  std::size_t bytes;          // Pyramid::resident_bytes() at admit
  std::uint64_t recency;      // LRU order, bumped on pin
  std::size_t pins;           // doc 02:268-277 — never a victim while > 0
  int width, height;          // the extent, published once (Constraint 2)
};
```

`ImageContent` stops holding the pyramid as its own state. It holds `d_resolved`
(the cache key, already a member since `kinds.image`'s shipped three-part
`ContentConfig` frame) and a once-published extent, and resolves pixels through
`cache.pin(d_resolved)` — an RAII hold, live for one `render()` call.

**Rationale.** *Until the cache owns the values, "evict under a byte budget" is a
no-op.* This is not a stylistic preference; it is the arithmetic of the task. With
`weak_ptr` entries and a strong `ImageContent::d_pyramid`, evicting a map entry
frees **zero bytes** for as long as the layer exists — and a layer exists for as
long as the document is open, which is the entire duration the budget is supposed
to govern. `image.md`'s own follow-up text presumed a residency counter would be
enough; it is not. Ownership is the task.

The transient pin is what makes it safe, and it is doc 02:268-277's *residency
pin* applied verbatim to a second cache. Because the pin is an owning
`shared_ptr` rather than a bare index, it is strictly stronger than
`KeyedStore`'s `CacheHold`: even a *removed* entry's pixels survive until the last
render reading them finishes.

**Rejected — keep the weak cache and evict only pyramids no live content holds.**
This is the one alternative that preserves every existing invariant untouched (no
ownership change, no publish-once amendment, no doc delta). It is also **useless**:
the set of pyramids no live content holds is, by construction, the set that is
*already dead* — `shared_ptr` freed them the moment the last content dropped them,
which `tests/image_content.t.cpp:109-137` already demonstrates. Such an "eviction"
would free nothing, bound nothing, and reduce the budget to decoration. It is
rejected because it does not do the task.

**Rejected — make `ImageContent` hold a `weak_ptr` and lock it per render.**
Superficially a smaller diff. But a `weak_ptr::lock()` that fails must then
re-decode, which means the content needs the encoded bytes and the LRU recency
update anyway — i.e. it needs the cache. Routing through the cache is the same
work with one owner instead of two, and it keeps the recency/pin bookkeeping in
the one place that can serialize it.

### Decision 2 — The cache retains the encoded bytes; a re-decode never re-fetches

Each cache entry keeps the encoded bytes it decoded from, shared across the N
contents that resolve to that URI. Eviction drops the **pyramid**, never the
bytes.

**Rationale.** *This plugin performs no file I/O at all* — `image_content.hpp:42-45`
states it as a defining property, and `kinds.image` Decision 5 built the whole
core-fetches / plugin-decodes split around it. A re-decode therefore has exactly
one possible source of bytes: bytes the plugin already has. Retaining them is not
a concession, it is the enabling condition.

The ratio is what makes it obviously correct: a 24 MP photograph is **~8 MB
encoded** against a **~512 MB** decoded pyramid — a **~64:1** reduction. Retaining
the source to make the derived data evictable is the entire trade, and it is
precisely doc 15:129-131's framing: budgets govern **derived** data. Thirty
photographs cost ~240 MB of retained encoded bytes, against the ~15 GB of pyramids
the budget now bounds.

This does not weaken doc 08 Principle 3 ("stores nothing"): that principle governs
what enters the **document**, and `params` remains exactly `{"source": <uri>}`.

**Rejected — re-fetch the bytes through `AssetSource` on a cache miss.** It is the
"purest" option — nothing retained, the URI re-resolved on demand — and it is
wrong on three counts. (a) `AssetSource` is a **load-time `LoadContext` hook**
(`kinds.image` Decision 5), not a render-time service; reaching it from a render
worker means threading a fetch channel from `runtime` (L5) down into an L4 plugin,
inverting the levelization. (b) It would put **file I/O inside the plugin**, the
one thing the codec-line split exists to prevent. (c) It makes every eviction a
potential **disk or network** round trip on a render worker, where the retained-bytes
design makes it a pure CPU decode. This alternative turns a plugin-local 2d task
into a cross-component 5d one and buys ~1.5% of the memory back.

**Rejected — evict the encoded bytes too, under the same budget.** They are 1-2%
of the footprint, and evicting them makes the entry unrecoverable (see above). The
budget would gain nothing and lose the ability to re-decode.

### Decision 3 — Publish-once moves from the pixels to the extent; `render_thread_safe()` stays true on new legs

`image_async_pending.md` Decision 6 made the content's pyramid pointer
**monotonic** — null → non-null, never reverting — and doc 03:150-160 wrote that
monotonicity into the `install_asset` contract as the thing that "buys the flag".
That refinement then said, at `:813-819`, aimed directly at this task:

> "`kinds.image_master_budget` will evict decoded pyramids under a byte budget and
> re-decode on demand — which means a *second* publish, and a non-null → null →
> non-null transition that this task's monotonicity forbids. That is deliberate:
> it is that task's job to **generalize the discipline** … **Do not pre-weaken
> it.**"

It was not pre-weakened, and this is the task that generalizes it. The resolution:
**split the two things the one pointer was carrying.**

- The **extent** (the decoded `width`/`height`) publishes **exactly once,
  monotonically, and is never cleared** — by eviction or anything else. This is
  the half that must be stable, because it is the half the compositor culls on
  (Constraint 2). Doc 03's monotonic-publish obligation survives intact, applied to
  it.
- The **pixels** become **budgeted derived data**: evictable, and re-derivable
  byte-identically from the retained bytes (Decision 4).

`render_thread_safe()` stays `true` on the three legs of Constraint 7 —
immutability of a `Pyramid` object, owning pins, and the cache mutex — none of
which is monotonicity of a content-owned pointer.

**Rationale.** This is the smallest amendment that admits eviction, and it keeps
the property doc 03 actually cares about: *a worker never observes a partial or
torn state.* Under the pin discipline a worker observes either a fully-built
pyramid it owns for the duration, or no pixels at all — never a third value. What
changes is only that the "no pixels" state is now reachable *after* pixels existed
— and Constraint 2 makes that state unobservable in the composition, because
bounds and geometry come from the extent, not the pixels.

**Carries a doc 03 delta** (`install_asset`, `03:143-161`) and a rewrite of
`registry.tsv:307`.

**Rejected — keep monotonicity by never evicting a pyramid a live content
published.** That is Decision 1's rejected weak-cache option wearing a different
hat: it preserves the invariant by making the feature do nothing.

**Rejected — a versioned cache slot with a generation counter on the content.**
`image_async_pending.md:813-819` floated this ("*making the pointer a proper
versioned cache slot and paying for it*"). It would let the content keep a
self-invalidating direct pointer and skip the per-render cache lookup. Rejected as
**paying for a hot-path optimization before measuring one**: the pin is a mutex
lock, a hash lookup, and a recency bump **once per `render()` call** — not per
tile, not per pixel — against a render that is already allocating a surface and
resampling a region. A generation-counter protocol between an evicting cache and N
content pointers is a genuinely subtle piece of lock-free code, and it buys a hash
lookup. If profiling ever shows the pin on the hot path, `kinds.image_decode_in_flight`
is where that work belongs.

### Decision 4 — Eviction is a no-op in pixel space; the existing goldens are the oracle

A re-decode reproduces a **byte-identical** pyramid, so a render of an evicted
image is byte-identical to a render of a resident one.

**Rationale.** `Pyramid::decode` is already a pure function: `imdec` over the
encoded bytes, a fixed sRGB8→working conversion (`image_content.cpp:84-95`), and a
frozen Lanczos-3 half-band chain through `media` (`:98-124`). No randomness, no
threading, no cached intermediate. Nothing about eviction touches it.

This has a large payoff for the test plan: **the acceptance test for eviction is
the existing golden suite, re-run under a 1-byte budget.** No new golden data is
authored, and the check is maximally strong — every pixel of every rung at every
scale must match. Same for the conformance suite. A task that changes memory policy
should be provable by showing that *nothing observable changed*, and here it is.

### Decision 5 — Eviction is model-invisible: no revision bump, no damage

Eviction touches no model state, opens no `Model::Transaction`, bumps no revision,
and emits no `Damage`. It is entirely plugin-local.

**Rationale.** `image_async_pending.md` Decision 4 required a revision bump and a
damage route for the *arrival* of a pending asset, and stated exactly why it would
not transfer: an arrival changes bounds from **empty to non-empty**, so the
composition's geometry changes. An eviction changes **nothing observable** —
Decision 4 guarantees the pixels are identical and Constraint 2 guarantees the
bounds are unchanged. That refinement anticipated this and wrote the test for the
successor to apply: "*argue that a re-decode needs no damage at all, since pixels
are identical and only residency changed; **but then the revision must not bump
either, or you invalidate every composed tile for nothing***."

Both halves are honored. Composed tiles keyed on `(content id, revision, scale
rung, tile coords)` (doc 02:257) **stay valid across an eviction**, because
nothing a composed tile depends on changed. Bumping a revision here would orphan
every cached tile in the document to announce that some pixels moved from RAM to
recomputable — the exact pathology `model.per_object_revision` was created to fix.

**Residency is not composition state.** That sentence is the claim.

*Consequence, and it is a feature:* this keeps the entire diff inside
`plugins/image/`. No `runtime` edge, no `model` edge, no settle hook, no
`serialize` change — which is what holds the task at 2d.

### Decision 6 — Whole-pyramid eviction granularity, not per-level

The evictable unit is one `Pyramid`. Level-granular eviction is deferred to
`kinds.image_level_granular_eviction`.

**Rationale.** It is what the `.tji` note specifies ("an LRU over the plugin-side
pyramid cache"), and it is the granularity at which the promised instrument
(`Pyramid::resident_bytes()`, a whole-pyramid sum) already measures. It fits 2d.

The honest cost, stated plainly rather than buried: **whole-pyramid eviction
thrashes on the very workload that motivated the task.** A zoomed-out view of 30
photographs at a budget holding 2 will evict and re-decode on essentially every
layer, every frame — `decodes_issued()` grows without bound — even though that view
reads only the *small upper rungs*, which for all 30 images together would cost a
few MB. Level-granular eviction is therefore not a nice-to-have; it is the policy
that makes the budget *good* rather than merely *correct*. It is deferred, not
dismissed, and it is registered as a named leaf with the arithmetic attached.

This task's contract is bounded memory. The follow-up's is bounded memory *without*
thrash.

**Rejected — build level-granular eviction now.** It needs per-level residency
accounting, a partial-rebuild decode path (rebuild rungs 3..n without materializing
rung 0 — which the current chain cannot do, since each rung decimates the one
below), and its own golden matrix. That is a 3d task on top of this one's 2d, and
it cannot even be written until the ownership inversion here exists. Sequencing it
after is not a compromise; it is the only order in which it can be built.

### Decision 7 — A purpose-built LRU inside `PyramidCache`, not `arbc::cache`'s `KeyedStore`

**Rationale.** Two independent disqualifications, either sufficient:

1. **Levelization forbids the edge.** A kind is L4 and its only granted direct
   edge is `contract` (doc 17:70); `arbc::cache` is **not** in `contract`'s
   transitive closure (`17:65` — `cache` depends on `base` + `surface`, and only
   `compositor`/`audio_engine`/`runtime` are granted an edge to it). Using
   `KeyedStore` here means **editing doc 17's normative table** to grant every kind
   a cache edge — a large, permanent widening of the plugin ABI's reach to save
   ~150 lines. Note the edge would **not** be mechanically caught:
   `scripts/check_levels.py` globs only `src/*/CMakeLists.txt`, and the plugin
   links the `arbc` umbrella PUBLIC (`plugins/image/CMakeLists.txt:33`). That makes
   honoring doc 17 here a matter of design discipline rather than of passing a
   check, which is precisely when it matters most.
2. **`KeyedStore` is not thread-safe.** `keyed_store.hpp:141-145`, verbatim:
   "*Render-/mix-thread-confined and **not thread-safe** by design … It holds no
   lock; concurrent reader-lookup vs. worker-fill is a designed future mode whose
   hardening is deferred.*" `PyramidCache` is the opposite by design — explicitly
   raced and TSan-driven (`image_content.hpp:118-120`,
   `tests/image_concurrency_stress.t.cpp:255-287`) — and this task puts it on
   **every render thread**. Adopting `KeyedStore` would mean wrapping it in the
   very mutex it was written to avoid, inheriting its priority classes and prefetch
   rings (meaningless for whole decoded assets keyed by URI), and taking on a
   documented future-hardening liability.

What is copied is its **shape and vocabulary** — `resident_bytes()`, `budget()`,
`evictions()`, `victim()`, `evict_to_fit()`, an RAII pin (`:78`, `:191-195`,
`:229-261`) — so the two caches read alike and doc 17:124-138's counter idiom holds.
Copying a 150-line pattern across a levelization boundary is the correct move here;
`imageseq` already keeps its own plugin-side LRU for the same reason.

### Decision 8 — The budget is a defaulted constructor parameter plus an env override, following `Journal` and `ARBC_PLUGIN_PATH`

- `explicit PyramidCache(std::size_t byte_budget = k_no_budget)` with
  `k_no_budget = std::numeric_limits<std::size_t>::max()` — copied verbatim in
  shape from `Journal` (`journal.hpp:56-61`). A test-constructed cache is therefore
  **unbudgeted unless it asks**, so every existing test keeps its current behavior
  with no edit.
- `default_pyramid_cache()` — the process-wide instance the registered factory uses
  — takes a real default of **1 GiB** (`k_default_pyramid_budget`, a named
  constant, following `offline_sequence.hpp:79`), overridable by
  `ARBC_IMAGE_PYRAMID_BUDGET_BYTES`.
- `set_byte_budget()` for an in-process host that links the impl archive directly.

**Rationale.** The plugin's registration surface is `registry.add(kind_id, factory,
KindMetadata)` (`image_plugin.cpp:16-21`) and its per-content channel is the opaque
`ContentConfig` frame — **neither carries plugin-wide policy**, and a host that
`dlopen`s the MODULE has no other way to reach a function-local static. An
environment variable is the seam that already exists for exactly this shape of
problem: `ARBC_PLUGIN_PATH` is the house precedent (claim
`03-layer-plugin-interface#plugin-path-scan-is-opt-in`), and it needs no ABI
change. The `k_no_budget` default on the *class* keeps the change invisible to
existing tests while the *default instance* gets a real ceiling, which is where the
ceiling is actually needed.

1 GiB is a judgment call (≈ two 24 MP masters), and the soft-budget rule
(Constraint 4) is what makes a wrong guess non-fatal in either direction: too low
merely costs re-decodes, never correctness.

**Rejected — add a policy channel to the plugin ABI.** Widening `ContentFactory` or
`KindMetadata` to carry host policy is an ABI change to `contract` (L3) affecting
every kind, to configure one number in one plugin. `kinds.image` Decision 5 rejected
widening the factory signature for a strictly better reason than this one.

### Decision 9 — The decode stays under the cache mutex

A pin that misses decodes **while holding `d_mutex`**, exactly as `resolve()` does
today (`image_content.cpp:137-152`).

**Rationale.** It preserves, for free and by construction, the one-decode-per-key
invariant under concurrency that `tests/image_concurrency_stress.t.cpp:282` pins (8
threads racing one URI → `decodes_issued() == 1`) — an invariant that becomes
*harder*, not easier, to hold once misses can originate on N render workers rather
than one load thread. Keeping the existing serialization means that test stays green
**unchanged**, which is the strongest available evidence that the ownership inversion
did not quietly break the dedup.

The cost is real and named: a ~24 MP re-decode on a worker holds the global cache
mutex, stalling pins of *unrelated* images. That is a latency defect, not a
correctness one, and fixing it properly means a per-key in-flight promise — which is
`kinds.image_decode_in_flight`, registered above with the arithmetic attached.
Shipping the correct-but-coarse version first, with the invariant provably intact, is
the right order.

**Rejected — decode outside the lock with a double-checked re-insert.** The obvious
"fix" races: two workers missing on one key both decode, and one decode is discarded
— `decodes_issued()` becomes 2 and the dedup claim breaks. Getting it right requires
the in-flight promise anyway, so this is a strictly-worse version of the deferred
task, not a cheaper one.

## Design-doc deltas (ride in the closer's commit — doc 16 same-commit rule)

1. **`docs/design/03-layer-plugin-interface.md:143-161`** — the `install_asset`
   contract comment. Amend the "publish ATOMICALLY and AT MOST ONCE" /
   "monotonicity, not immutability, buys the flag" paragraph to say what is now
   true (Decision 3): a kind's **extent** publishes once and monotonically, while
   its decoded **pixels** may be budgeted derived data — evictable and re-derivable
   byte-identically — and `render_thread_safe()` is then bought by *immutable
   values + owning pins*, not by pointer monotonicity. Keep the existing obligation
   for kinds that do not evict. Name `org.arbc.image` as the kind that takes the
   second form.

   **The same text is mirrored in the source** — the `install_asset` doc comment at
   `src/contract/arbc/contract/content.hpp:645-666` carries doc 03's paragraph
   nearly verbatim ("ATOMICALLY and AT MOST ONCE", "monotonicity, not immutability,
   that buys the flag"). Amend both in the same commit, or the header will contradict
   the doc it quotes. Likewise the rationale comments in
   `plugins/image/arbc/kind_image/image_content.hpp:54-57, 220-230` and
   `plugins/image/image_content.cpp:210-215`, which state the retired
   monotonicity argument in full and already name this task as the one that
   generalizes it.

2. **`docs/design/15-memory-model.md`** — the populations table (`:16-22`) and the
   "budgets as the policy" section (`:117-131`). Doc 15 today covers budgets over
   *cache values* (composed tiles, audio blocks) and over the *journal*, but
   **nothing covers a decoded source asset held inside a kind**. Add the population
   and state the policy: a kind's decoded asset is **derived data** (re-derivable
   from a retained encoded source), governed by a byte budget with LRU eviction and
   a residency pin, per doc 02:268-284 — and eviction of derived data is
   **model-invisible** (no revision, no damage; Decision 5).

3. **`docs/design/00-overview.md`** — one decision-record bullet. The project-shaping
   half is not the LRU; it is that **decoded pixels are derived, not state**:
   `org.arbc.image` retains its encoded source and treats the pyramid as a
   recomputable cache, so residency is a memory-policy question and never a
   composition-state one.

## Open questions

(none — all decided.)

One item is surfaced to the closer for `tasks/parking-lot.md` rather than encoded as
a WBS task, because it is a human judgment call and not implementable work: **the
1 GiB default budget** (Decision 8) is an engineering guess, not a measured value.
The soft-budget rule makes any guess safe (a low budget costs re-decodes, never
correctness), and `ARBC_IMAGE_PYRAMID_BUDGET_BYTES` makes it tunable — but the
number a shipped v0.1 should default to is a product decision about the target
machine, and it deserves a human's eye before the tag.

## Status

**Done** — 2026-07-14.

- `PyramidCache` (`plugins/image/arbc/kind_image/image_content.hpp`) inverted from `weak_ptr` to owning entries; gains LRU recency list, byte total, `evictions()`/`resident_bytes()` accessors, and a configurable byte budget (`k_no_budget` default on the class; 1 GiB default on `default_pyramid_cache()`, overridable by `ARBC_IMAGE_PYRAMID_BUDGET_BYTES`).
- `ImageContent` (`plugins/image/image_content.cpp`) drops strong pyramid ownership; resolves pixels through a transient RAII pin from `PyramidCache::pin(d_resolved)` for each `render()` call; retains a once-published extent (`width`/`height`) so `bounds()` and `available()` are never cleared by eviction.
- New test suite `tests/image_budget.t.cpp` (8 cases): LRU victim selection, exactly-one re-decode vs zero, recency-by-pin, soft-budget oversized pyramid, evict-while-pinned safety, evicted-bounds-never-empty, absence-not-served-from-cache, model-invisibility via `RecordingDamageSink`.
- Conformance suite (`tests/image_conformance.t.cpp`) gains a second run under 1-byte budget — every pull evicts and every render re-decodes; all contract properties stay invariant.
- Goldens suite (`tests/image_goldens.t.cpp`) re-run under forced-eviction cache; four existing golden cases produce byte-identical output — no new golden data.
- Concurrency stress (`tests/image_concurrency_stress.t.cpp`) extended with N-worker continuous-eviction race and evictor-vs-live-pin race; both ASan/TSan-clean.
- Two new claims registered (`tests/claims/registry.tsv`): `15-memory-model#image-pyramid-evicts-under-byte-budget`, `03-layer-plugin-interface#image-re-decode-is-byte-identical-and-model-invisible`; two existing claims amended (`#image-decodes-once-per-resolved-uri`, `#image-pyramid-publishes-once`).
- Design-doc deltas landed in same commit: `docs/design/00-overview.md` (derived-pixels decision record), `docs/design/03-layer-plugin-interface.md` (`install_asset` contract — extent publishes once, pixels are budgeted derived data), `docs/design/15-memory-model.md` (kind-decoded-asset population + eviction policy).
- `src/contract/arbc/contract/content.hpp` install_asset doc comment updated to match doc 03 amendment.
- Follow-ups registered: `kinds.image_level_granular_eviction` (3d, M10), `kinds.image_decode_in_flight` (2d, M10).
