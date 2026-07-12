# runtime.worker_dispatch_leaf_only — Hoist the leaf-only worker-dispatch rule into a shared helper

## TaskJuggler entry

[`tasks/65-runtime.tji:146-151`](../../65-runtime.tji):

```
task worker_dispatch_leaf_only "Hoist leaf-only worker-dispatch rule into a shared helper" {
  effort 1d
  allocate team
  depends !interactive_pull_wiring
  note "Extract a worker_backed_dispatch(pool) helper in runtime that enforces the
        leaf-only worker-dispatch invariant (operator contents render inline on the
        driver thread), so runtime.interactive_pull_wiring and any future driver
        cannot reintroduce the TileCache/KeyedStore data race that TSan confirmed
        was latent for fade, crossfade, and nested in SequenceRenderer.
        Source-of-debt: tasks/refinements/kinds/nested_runtime_binding.md. Docs 02/13."
}
```

The closer appends `complete 100` and the
`Refinement: tasks/refinements/runtime/worker_dispatch_leaf_only.md` back-link
to the `note` (the ritual in [`tasks/refinements/README.md:47-72`](../README.md)).

## Effort estimate

**1d.** The helper itself is twenty lines — the code already exists as a lambda
(`offline_sequence.cpp:162-170`) and the move is mostly deletion. The day goes
into the two things that make it *load-bearing* rather than cosmetic: the
interactive swap (`interactive.cpp:254`), which turns `InteractiveRenderer`'s
`WorkerPool` from a park/wake primitive into a real executor and so brings the
frame loop's deadline-cancel path into contact with in-flight workers for the
first time; and the concurrency coverage that pins it — a thread-identity test
over fade/crossfade/nested on both drivers, an interactive cancel-with-workers
test, the design-doc delta the new claim hangs on, and a grep-lint that keeps
the rule single-copy.

## Inherited dependencies

**Settled:**

- **`kinds.nested_runtime_binding`** (Done) — minted this debt. Its Status
  bullet (`tasks/refinements/kinds/nested_runtime_binding.md:480`) is the
  origin: *"`SequenceRenderer` parallel path now fans out only leaf contents to
  the worker pool — operator contents (fade, crossfade, nested) render inline on
  the driver thread, preventing a `TileCache`/`KeyedStore` data race that TSan
  confirmed was latent for all three kinds."* That was an implementer's fix
  inside one driver, not a designed seam — which is precisely why it is now a
  task.
- **`runtime.interactive_pull_wiring`** (Done, the explicit `depends`) — built
  the interactive frame's `PullServiceImpl` and left the dispatch argument as
  `direct_dispatch()` specifically so this task could swap it. Its Decision 3
  (`:441-449`) is the work order: *"`runtime.worker_dispatch_leaf_only` exists to
  hoist that rule into `worker_backed_dispatch(pool)` … the interactive driver
  adopts it there. This task leaves the swap a one-line change at the dispatch
  argument."*
- **`runtime.interactive_binder_wiring`** (Done) — put `bind_operators` on the
  interactive frame (`interactive.cpp:276-280`), so operator contents now
  actually render on that path. Before it, the interactive leaf-only rule was
  vacuous; after it, it is real. Its Constraint 6 (`:248-258`) reserved the
  worker-backed change for this task by name.
- **`runtime.threading`** (Done) — `WorkerPool`, `RenderTask`, the degenerate
  inline executor at `worker_count == 0`, and the invariant this task enforces,
  stated but not enforced: `worker_pool.hpp:37-40`.

**Pending (deliberately downstream — this task must not pre-empt them):**

- **`pool.concurrent_pin_benchmark`**, **`quality.repo_linters`** — the lint this
  task adds is a single-purpose script in the established `scripts/check_*.py`
  shape; the general linter-consolidation task may later fold it in. Do not wait
  for it.
- **Audio worker dispatch.** `AudioDispatch` (`pull_service.hpp:63-64`) is the
  audio twin of `RenderDispatch`, and `AudioWorkerPool` exists — but the audio
  path has a *different* discipline (doc 12: the mixer pulls every layer through
  the seam and never calls `render_audio` inline, because arbitrary plugin audio
  code must come off the RT callback). The leaf-only rule is a *video*
  render-thread-confinement rule and does not transpose. See Decision 5.

## What this task is

Today exactly one place in the tree hands a render to a worker thread:
an anonymous lambda inside `SequenceRenderer::render_frame_at`
(`src/runtime/offline_sequence.cpp:162-170`). That lambda carries a
correctness invariant — *submit only leaves; render operators inline* — behind
thirteen lines of comment explaining that violating it is a TSan-confirmed data
race on the tile cache. The invariant is real, the comment is excellent, and the
enforcement is nothing: `WorkerPool::submit` will cheerfully accept an operator
`Content*`, and the next driver to want parallelism re-derives the rule from
scratch or doesn't.

This task gives the rule a name and a single home. It adds
`RenderDispatch worker_backed_dispatch(WorkerPool&)` to `runtime`, moves the
gate (and its rationale) into it, rewrites `SequenceRenderer` to obtain its
dispatch from the helper, and swaps the interactive driver's `direct_dispatch()`
for `worker_backed_dispatch(d_pool)` — which is the one-line change
`interactive_pull_wiring` left staged, and which incidentally closes a live
inconsistency: `InteractiveRenderer` takes a `WorkerPoolConfig`, owns a
`WorkerPool`, and parks on it (`interactive.cpp:308`), but never submits to it,
so a host passing `worker_count > 0` today gets threads that never receive work.

It then makes the rule structural rather than aspirational: a design-doc delta
that says worker dispatch is leaf-only (doc 02 § Threading model had no operator
carve-out at all), a claims-register entry, thread-identity assertions over
fade/crossfade/nested on both drivers, and a grep-lint that fails the build if a
second `RenderTask` submit appears anywhere outside the helper.

## Not this task

- **Making the interactive path parallel by default.** `WorkerPoolConfig::worker_count`
  keeps its `0` default (`worker_pool.hpp:63`), so the shipped interactive
  configuration stays byte-for-byte inline. This task makes `worker_count > 0`
  *work*; it does not make it the default. Choosing a production worker count is
  a tuning question with no data behind it yet.
- **Any pixel change.** The task is pixel-neutral by construction (Constraint 6).
  A golden that needs a new baseline is a bug in this task, not a new baseline.
- **Audio dispatch** (Decision 5).
- **Speculation/prefetch dispatch.** Step 7's speculation is render-free today;
  it stays that way.

## Why it needs to be done

1. **The invariant has one enforcement site and two consumers.** With
   `interactive_binder_wiring` landed, the interactive frame binds operators and
   renders them; the moment anyone wires `d_pool` into that path's dispatch —
   which is the obvious and intended next step, and which the header comment at
   `interactive.hpp:66-68` explicitly advertises — the leaf-only gate must exist
   there too. Copying it is the failure mode both predecessor refinements named:
   *"a second hand-rolled copy of it in the tree — which is the exact thing
   `runtime.worker_dispatch_leaf_only` exists to prevent"*
   (`interactive_binder_wiring.md:443`).
2. **Getting it wrong is silent.** Submitting an operator to a worker does not
   fail to compile, does not assert, and does not usually produce wrong pixels on
   a developer's machine. It produces a data race between the worker's re-entrant
   `pull` (cache probe + insert, `pull_service.cpp:223,311`) and the driver
   thread's own probes and `poll_refinements` — plus a torn read of the service's
   frame-thread-confined descent depth (`pull_service.hpp:208-213`). It surfaces
   as a TSan report, or as corruption in the field.
3. **The one worker-dispatching path in the tree has no claim.** `WorkerPool` has
   three registry rows (`registry.tsv:146-148`) covering degenerate-inline,
   serialization, and graceful stop. The *parallel-exact* path — the only code
   that actually fans renders out — has none: `offline_sequence.t.cpp:439`
   (`"parallel-exact rendering is byte-identical to the inline path"`) carries no
   `enforces:` tag, and no registry row mentions the leaf/operator split. The
   riskiest path in the codebase is the one with nothing pinning it.
4. **`InteractiveRenderer` currently lies about its own configuration.** It
   accepts `WorkerPoolConfig` (`interactive.hpp:162`), constructs `d_pool` from
   it, exposes `worker_pool()` (`:219`), and never calls `submit`. A host that
   asks for four render threads gets four threads that park forever. That is
   either a bug or a missing feature; this task makes it the feature.

## Inputs / context

### Design docs (normative)

- **doc 02:118-137 (§ Threading model)** — the governing section. It says
  *"Layer rendering runs on a worker pool"* and *"Compositing itself happens on
  the render thread"*, and it says nothing about operators — because operators
  (doc 13) postdate it. **This task lands the delta that closes that gap**; see
  Decision 4 and the design-doc delta below.
- **doc 02:135-137** — *"This is the *model*; v1 may degenerate to 'everything on
  one thread' while keeping the request/completion structure, so concurrency
  arrives as an optimization rather than a redesign."* The licence for keeping
  `worker_count = 0` the default while making `> 0` real.
- **doc 13:69-71** — *"Operators do not call `input->render()` directly … At
  attach, content receives a `PullService`."* The reason an operator render is
  not a self-contained unit of work and therefore not a dispatchable one: it
  re-enters shared, render-thread-confined state.
- **doc 17:56 vs :60** — `compositor` is L4 (`contract, cache (+ below)`);
  `runtime` is L5 (*"everything below"*). The helper must live in `runtime`
  (it names `WorkerPool`) while returning a `compositor` type (`RenderDispatch`).
  L5 → L4 is legal; the reverse is not, which is why `RenderDispatch` is a
  `std::function` over `contract` types in the first place
  (`pull_service.hpp:28-34`).

### Source seams

| What | Where |
| --- | --- |
| The gate to hoist, and its thirteen-line rationale | `src/runtime/offline_sequence.cpp:142-170` |
| The dispatch seam it satisfies | `src/compositor/arbc/compositor/pull_service.hpp:38-52` (`RenderDispatch`, `direct_dispatch()`) |
| The invariant, stated but unenforced | `src/runtime/arbc/runtime/worker_pool.hpp:37-42` (*"Workers never touch the cache … The `PullService` dispatch seam is injected downward at wiring time (a `std::function` over `submit`)"*) |
| `RenderTask` and its lifetime rule | `src/runtime/arbc/runtime/worker_pool.hpp:44-54` (*"A submitted task must not outlive its backing `PendingTile`"*) |
| The degenerate inline executor | `src/runtime/arbc/runtime/worker_pool.hpp:57-63` (`worker_count = 0`) |
| Leaf vs operator — the whole mechanism | `src/compositor/arbc/compositor/operator_graph.hpp:83-85` (`is_operator` ⇔ non-empty `inputs()`) |
| The interactive dispatch site to swap | `src/runtime/interactive.cpp:239-254` |
| Interactive's park/cancel step (now meets real workers) | `src/runtime/interactive.cpp:300-315` |
| Interactive's arrival reap | `src/runtime/interactive.cpp:317-321` (`poll_refinements`) |
| Member order that makes the swap safe | `src/runtime/arbc/runtime/interactive.hpp:256-258` — `d_pending` declared **before** `d_pool`, so `d_pool` destructs **first** and joins its threads while the pending surfaces are still alive |
| Frame-local binding scope in-flight renders must not reach | `src/runtime/interactive.cpp:276-280`, `operator_binding.hpp:95-96` |
| Stale comments this task retires | `src/runtime/interactive.hpp:66-68` (*"`runtime.worker_dispatch_leaf_only` owns the worker-backed swap"*), `src/runtime/interactive.cpp:246-247`, `src/runtime/arbc/runtime/interactive.hpp:52` |
| The untagged parallel-exact test to pin | `src/runtime/t/offline_sequence.t.cpp:439` |
| Grep-lint precedent (CI `lint` job) | `scripts/check_rt_safety.py`, `.github/workflows/ci.yml:26-31` |
| Claims register | `tests/claims/registry.tsv:146-148` (worker pool), `:171` (surface retained until settle) |

### Predecessor decisions this task inherits verbatim

- `interactive_pull_wiring` Decision 3 (`:441-449`) — the helper's name and shape
  are fixed (`worker_backed_dispatch(pool)`, in `runtime`), and **the interactive
  adoption belongs to this task**, as a one-line change at the dispatch argument.
- `interactive_binder_wiring` Decision 5 (`:436-448`) — *"folding it in would put
  the concurrency change and the pixel change in one commit, so a golden
  regression and a race would be indistinguishable at bisect time."* The
  corollary now that the concurrency change is *this* commit: this commit must
  contain **no** pixel change (Constraint 6).
- `interactive_binder_wiring` Constraint 2 (`:206-215`) — the `OperatorBindingScope`
  holds a `PullService&` into a stack frame and must not be outlived. Constraint 3
  below is the worker-facing half of that rule.
- `kinds.nested_runtime_binding` Constraint 8 (`:230-232`) — binding runs on the
  driver thread *before* any worker dispatch; nested's borrowed pointers are
  read-only on workers thereafter. Both drivers already order it that way
  (`offline_sequence.cpp:177`, `interactive.cpp:276-280`); the helper must not
  disturb that ordering.
- `interactive_pull_wiring` Decision 5 — do not mint a claim id for a sentence no
  design doc contains. Hence the doc-02 delta *precedes* the registry row here,
  rather than the row asserting a rule the constitution never states.

## Constraints / requirements

1. **One enforcement site, structurally.** After this task,
   `WorkerPool::submit(RenderTask{...})` appears in exactly one non-test
   translation unit: the helper's. `SequenceRenderer`'s lambda is deleted, not
   left beside it. A grep-lint (A5) makes this a build failure rather than a
   review convention.

2. **The helper lives in `runtime`, in its own header.**
   `worker_backed_dispatch(WorkerPool&) -> RenderDispatch` must name both a
   `runtime` type (`WorkerPool`) and a `compositor` type (`RenderDispatch`), so
   it cannot live in `compositor` (L4 may not name L5). It must *not* go into
   `worker_pool.hpp` either: that header includes only `contract`
   (`worker_pool.hpp:3`), and adding `<arbc/compositor/pull_service.hpp>` to it
   would drag the compositor into every `WorkerPool` consumer. New files:
   `src/runtime/arbc/runtime/worker_dispatch.hpp` + `src/runtime/worker_dispatch.cpp`,
   registered in `src/runtime/CMakeLists.txt` (`SOURCES` + `PUBLIC_HEADERS`). No
   new `DEPENDS` edge — `runtime` already depends on `compositor`.

3. **A dispatched leaf may outlive the frame; it must borrow nothing frame-local.**
   This is the constraint the interactive swap introduces and the offline driver
   never had (offline reaps to quiescence inside the frame,
   `offline_sequence.cpp:185-188`; interactive parks to a deadline and *leaves
   unsettled renders in flight across frames*, `interactive.cpp:308-315`). A
   `RenderTask` holds `Content*` (owned by the pinned document), a
   `RenderRequest` — which holds `Surface& target` (owned by the `PendingTile` in
   the *member* `d_pending`, retained across frames by `poll_refinements`) and a
   `StateHandle snapshot` by value (`content.hpp:84-92`) — and a
   `shared_ptr<RenderCompletion>`. None of these is the frame-local `pulls` or the
   frame-local `OperatorBindingScope`. **This holds only because leaves don't
   pull.** It is the second, independent payoff of the leaf-only rule, and the
   implementation must not weaken it: the helper is the reason a worker can never
   be holding a `PullService&` when the frame's stack unwinds.

4. **`cancel()` must not free a surface a worker is writing.** Interactive's
   deadline path cancels unsettled `BestEffort` pendings (`interactive.cpp:309-315`)
   and `poll_refinements` drops settled/failed arrivals. With real workers this is
   the first time `cancel()` races an in-flight render. The contract already
   promises the right thing — cancellation is *advisory* (`content.hpp:122-123`),
   and `registry.tsv:171`
   (`13-effects-as-operators#pull-retains-render-surface-until-settle`) states
   *"a caller's `RenderCompletion::cancel()` never frees a surface an in-flight
   worker is still writing"*. This task is what makes that clause load-bearing on
   the interactive path, so it must **test** it there (A3), not assume it. If the
   implementation finds the promise is not actually kept, fixing it is in scope —
   it is the same bug class this task exists to prevent.

5. **Renderer teardown joins before it frees.** Destroying an
   `InteractiveRenderer` with renders in flight must join the pool before
   `d_pending`'s surfaces die. The declaration order at `interactive.hpp:256-258`
   (`d_pending`, then `d_counters`, then `d_pool`) already gives this — reverse
   destruction runs `~WorkerPool` first, which stops and joins
   (`worker_pool.hpp:96-113`). Do not reorder those members; pin the property with
   a test (A3) so a future reorder fails loudly rather than silently.

6. **Pixel-neutral, at every worker count.** `worker_count` keeps its `0` default
   (`worker_pool.hpp:63`), and at `0` the helper's `submit` *is* the degenerate
   inline executor, byte-identical to `direct_dispatch()` per
   `registry.tsv:146`. So every existing golden — interactive, offline, fade,
   crossfade, nested, damage, host-viewport — must pass **unchanged, with no
   re-baselining**. At `worker_count > 0`, output must be byte-identical to the
   `worker_count == 0` run (exactness is order-independent; this is what the
   offline path already claims at `offline_sequence.cpp:146-147`).

7. **No levelization change.** `runtime` (L5) → `compositor` (L4) for
   `RenderDispatch` and `is_operator`; both are already included by
   `offline_sequence.cpp`. `scripts/check_levels.py` must stay green with no
   edit to the component `DEPENDS`.

8. **Behavior at the seam is exactly the current lambda's.** `is_operator(content)`
   → render inline via a captured `direct_dispatch()`; otherwise
   `pool.submit(RenderTask{content, request, std::move(done)})`. A null `content`
   is `is_operator == false` today (`operator_graph.hpp:83-85` guards the null but
   returns `false`) and therefore reaches `submit`; preserve that — the pool's own
   null handling is its business, and quietly changing it here would be an
   unrelated behavior change riding a concurrency commit.

## Acceptance criteria

**A1 — The leaf-only rule holds on both drivers, observed by thread identity.**
A new `tests/worker_dispatch_leaf_only.t.cpp` builds a scene containing all
three operator kinds (fade, crossfade, nested) over leaf contents whose `render`
records `std::this_thread::get_id()`. Driven with `worker_count > 0` through
**both** drivers — `SequenceRenderer`'s parallel-exact path and
`InteractiveRenderer::render_frame` — the assertions are:
every operator content's recorded render thread ids are **all equal to the
driver thread's id** (zero operator renders on any worker), and at least one leaf
content records a thread id **different** from the driver's (proving the pool is
genuinely threaded and the test is not vacuously passing on an inline executor).
`enforces: 02-architecture#worker-dispatch-is-leaf-only`

**A2 — Byte-identical across worker counts, on both drivers.** The same scene
rendered at `worker_count == 0` and `worker_count > 0` (several counts, e.g. 1
and 4) produces byte-identical frames, on the offline and interactive drivers
alike — byte-exact, no tolerance. This subsumes and finally tags the untagged
`offline_sequence.t.cpp:439`; add the `enforces:` comment there rather than
duplicating the case.
`enforces: 02-architecture#worker-dispatch-is-leaf-only`
`enforces: 02-architecture#worker-pool-degenerates-to-inline`

**A3 — In-flight renders survive the frame, the cancel, and the teardown.** Three
assertions on the interactive path with `worker_count > 0` and leaf renders
deliberately slow enough to miss the frame deadline (a latch-gated test content,
not a sleep — **wall-clock tests lie in CI; latches don't**):
(a) the frame returns with renders still in flight and the deadline-cancel loop
(`interactive.cpp:309-315`) having run, and no surface is freed — the pending
tiles are still resident in `d_pending` and their surfaces still writable;
(b) releasing the latch, the arrivals reap normally through `poll_refinements` on
the next frame and composite correctly;
(c) destroying the `InteractiveRenderer` with renders still latched-in-flight
(then releasing) joins cleanly with no use-after-free — Constraint 5.
Under TSan and ASan, clean.
`enforces: 13-effects-as-operators#pull-retains-render-surface-until-settle`

**A4 — TSan, on the per-push lane.** `tests/worker_dispatch_leaf_only.t.cpp` runs
in the standard suite, which the `gcc-tsan` CI lane
(`.github/workflows/ci.yml:52`) executes with no exclusions — so the new
worker-count-> 0 fan-out over fade/crossfade/nested is TSan-covered on every
push, which is exactly the coverage the original race lacked. It must be clean.
Additionally, register the binary in `nightly.yml`'s `tsan-full` seed sweep
(`.github/workflows/nightly.yml:56-70`) with a `[.nightly]`-tagged
high-iteration case, so the fan-out gets repeated-schedule stress and not just a
single interleaving. Today that sweep lists only memory-model binaries
(`stress_publish_pin`, `stress_reclamation_queue`, `arena_growth_litmus`); this
is the first render-dispatch entry.

**A5 — A second copy cannot be added.** A new `scripts/check_worker_dispatch.py`,
run in the CI `lint` job alongside `check_levels.py` / `check_claims.py` /
`check_rt_safety.py` (`.github/workflows/ci.yml:23-31`), scans `src/` for
submissions of a `RenderTask` to a `WorkerPool` and fails unless the only
non-test occurrence is in `src/runtime/worker_dispatch.cpp`. This is the
mechanical form of the task's own note (*"any future driver cannot reintroduce"*)
— it turns "don't copy the lambda" from a comment into a build failure. Follow
`check_rt_safety.py`'s shape (grep-lint with an explicit allowlist and a
non-zero exit).

**A6 — Claims register grows by one row**, hung on the doc-02 delta this task
lands (Decision 4). Added to `tests/claims/registry.tsv`:

> `02-architecture#worker-dispatch-is-leaf-only`	Worker dispatch is leaf-only: every worker-backed RenderDispatch in the tree is obtained from runtime's single worker_backed_dispatch(pool) helper, which submits a content to the pool only when it is a leaf (empty inputs()) and renders an operator content (non-empty inputs() -- fade, crossfade, nested; doc 13) inline on the calling driver thread, because an operator's render re-enters the PullService whose cache probe/insert and descent-depth accounting are render-thread-confined. Driven with worker_count > 0 through both the offline parallel-exact path and the interactive frame loop over a scene holding all three operator kinds, every operator render is observed on the driver thread and none on any worker, at least one leaf render is observed off-thread, the frames are byte-identical to the worker_count == 0 run, and the suite is TSan-clean; a grep-lint keeps the helper the only submission site.

`scripts/check_claims.py` must pass (every row referenced by an `enforces:`
comment).

**A7 — No golden re-baselined, no `DEPENDS` edge added.** The full suite passes
with zero golden updates (Constraint 6) and `scripts/check_levels.py` is green
with `src/runtime/CMakeLists.txt`'s `DEPENDS` line untouched (Constraint 7). A
diff that touches a `tests/goldens/**` file is a failure of this task.

**A8 — Design-doc delta (same commit, doc 16's rule).** `docs/design/02-architecture.md`
§ Threading model gains the leaf-only bullet, and `docs/design/00-overview.md`
gains the decision-record bullet. Both are written and ride in the closer's
commit; see the delta section below.

**Coverage:** ≥90% diff coverage on changed lines. The helper is small and
fully exercised by A1–A3; the interactive swap's new branches (deadline-cancel
with a real in-flight worker) are exactly what A3 covers.

### Deferred follow-up (closer registers in WBS)

**`runtime.interactive_worker_count_default`** — effort `2d`, milestone
`m_performance` (the milestone owning `pool.concurrent_pin_benchmark`), depends
`!worker_dispatch_leaf_only`.

> note "Choose and ship a non-zero default `WorkerPoolConfig::worker_count` for `InteractiveRenderer`, backed by a behavioral-counter benchmark over representative scenes (leaf-heavy, operator-heavy, nested-deep): measure renders-per-frame and deadline-miss counts at worker counts 0/1/2/4/hardware_concurrency-1 and pick the default from the data, rather than the current `0` (inline). `runtime.worker_dispatch_leaf_only` made `worker_count > 0` correct and TSan-clean but deliberately left the default at `0` — there is no measurement behind any other number. Never a wall-clock assertion; the benchmark reports counters and the default is a reviewed choice. Source-of-debt: tasks/refinements/runtime/worker_dispatch_leaf_only.md 'Not this task'. Docs 02/16."

That is the only deferral. In particular, the audio dispatch seam is **not**
deferred to a follow-up task — it is out of scope on the merits (Decision 5), not
postponed.

## Decisions

**Decision 1 — The helper is a free function in a new `runtime` header:
`RenderDispatch worker_backed_dispatch(WorkerPool& pool)` in
`arbc/runtime/worker_dispatch.hpp`.**
It must name `WorkerPool` (L5) and return `RenderDispatch` (L4), so `runtime` is
the only level it can live at. A free function taking `WorkerPool&` matches the
name the predecessor fixed (`interactive_pull_wiring.md:445`), matches
`direct_dispatch()`'s shape exactly (a factory returning the `std::function`, so
the two are drop-in substitutable at the call site), and needs no new type.

*Alternative rejected:* put it in `worker_pool.hpp`, since the header already
reserves the seam in prose (`:41-42`). That header deliberately includes only
`<arbc/contract/content.hpp>` (`:3`, annotated *"L5->contract, doc 17:60"*).
Adding `<arbc/compositor/pull_service.hpp>` to it would make every consumer of
the pool — including tests that want nothing but the executor — compile the
compositor's public surface, and would blur the one clean statement in the tree
that the pool is a `contract`-only executor. A ten-line header is cheaper than
that coupling.

*Alternative rejected:* a `WorkerPool::render_dispatch()` member. Same
levelization problem in a worse place (the pool's own class would name a
compositor type), and it makes the leaf-only rule look like a property of the
pool rather than a property of *how drivers are allowed to use* the pool. The
pool remains a general executor with its own claims (`registry.tsv:146-148`,
tested directly by `src/runtime/t/worker_pool.t.cpp`); the leaf-only rule is a
policy layered above it. Keeping them as separate objects keeps both testable.

**Decision 2 — This task performs the interactive swap
(`interactive.cpp:254`: `direct_dispatch()` → `worker_backed_dispatch(d_pool)`),
rather than only landing the helper.**
`interactive_pull_wiring` Decision 3 assigned it here explicitly (*"the
interactive driver adopts it there … a one-line change at the dispatch
argument"*). And a helper with one caller does not *enforce* anything — the whole
claim of this task is that the rule is structural, which is only true once every
worker-backed dispatch in the tree comes from it. Landing the helper without the
second consumer would leave `InteractiveRenderer` still unable to honor its own
`WorkerPoolConfig`, and would leave the grep-lint (A5) guarding a rule with a
single trivial user.

The swap is safe to make in this commit because it is *pixel-neutral at the
shipped configuration*: `worker_count` defaults to `0`, and at `0` the pool is
the degenerate inline executor already claimed byte-identical to inline rendering
(`registry.tsv:146`). It changes behavior only for a host that opts in — which is
the thing that does not currently work.

**Decision 3 — `is_operator` stays the sole leaf/operator test; the helper adds
no new classification.**
`operator_graph.hpp:83-85` — *"operator" ⇔ non-empty `inputs()`* — is the whole
mechanism in the tree, with five call sites, and it is exactly the predicate the
existing gate uses. The helper reuses it verbatim.

*Alternative rejected:* introduce a `Content::dispatchable_to_worker()` contract
method, so content declares its own thread-affinity the way it declares
`render_thread_safe()`. This is superficially attractive — it is the same shape
as the existing serialization declaration (`worker_pool.hpp:65-72`, doc
02:126-130) — but it is wrong: whether a render may go to a worker is not a
property the *content* knows. It is a property of what the render will re-enter,
which is the *core's* `PullService`, which the core owns. A plugin operator
cannot opt out of the rule, and a plugin leaf cannot opt in to violating it;
making it a declaration invites both. Worse, it would add a contract method (a
public-ABI commitment, doc 03) to encode an internal threading invariant. The
core knows which contents have inputs; it should not ask.

**Decision 4 — Land a doc-02 delta, then hang the claim on it — rather than
minting a registry row for a rule the constitution never states.**
Doc 02 § Threading model (`02:118-137`) predates operators (doc 13) and says only
*"Layer rendering runs on a worker pool"* — read literally, it *permits* the very
dispatch that races. That is a real gap in the constitution, not merely an
unstated detail, and `interactive_pull_wiring` Decision 5 forbids minting a claim
id for a sentence no design doc contains. So this task amends doc 02 with an
explicit leaf-only bullet and adds a doc-00 decision-record bullet (the rule is
project-shaping: it fixes, permanently, *where* this engine's render parallelism
can live — at the leaves, never on the operator spine). The registry row (A6)
then enforces a sentence the constitution actually contains.

**Deltas written (they ride in the closer's commit, doc 16's same-commit rule):**
- `docs/design/02-architecture.md` § Threading model — new bullet *"Worker
  dispatch is leaf-only"*, between the external-async bullet and *"Compositing
  itself happens on the render thread"*.
- `docs/design/00-overview.md` § decision record — new bullet *"Worker dispatch
  is leaf-only"*, before the housekeeping-thread bullet.

**Decision 5 — Audio is explicitly out of scope, not deferred.**
`AudioDispatch` (`pull_service.hpp:63-64`) is the structural twin of
`RenderDispatch`, and it would be natural to reach for an
`audio_backed_dispatch(AudioWorkerPool&)` in the same header. Don't: the audio
path's discipline is the *opposite* one. Doc 12 requires that the mix engine pull
*every* layer through the seam and **never** call `render_audio` inline
(`pull_service.hpp:59-62`), because arbitrary plugin audio code must be kept off
the RT callback thread — so audio has no "render this one inline on the calling
thread" escape hatch to gate, and its cache-safety story is the lookahead ring's
(warm off-thread, drain on-thread), not the render thread's. A shared helper
would have to be parameterized into two contradictory policies, and the
similarity of the two seams' *types* would make that look reasonable. It isn't.
Audio's concurrency is already claimed and covered
(`registry.tsv:94,96,98,208`); leave it alone.

*Alternative rejected:* register a `runtime.audio_dispatch_leaf_only` follow-up
task "for symmetry." That would be a task with no defect behind it, and it would
encode a rule the audio design contradicts.

**Decision 6 — Enforce single-copy with a grep-lint, not with access control.**
The strongest possible enforcement would be to make `WorkerPool::submit` private
and friend the helper — then no driver *could* hand-roll a submission. Rejected:
`src/runtime/t/worker_pool.t.cpp` tests `submit` directly across six cases
(including the three claimed rows at `registry.tsv:146-148`), and the pool is
legitimately a general executor whose contract is worth testing on its own terms.
Friending the test suite to preserve that would trade a real coupling for a
cosmetic one. A grep-lint (A5) achieves the same practical guarantee — a second
submission site fails CI, loudly, with a message pointing at the helper — at zero
design cost, and it follows an established house pattern
(`scripts/check_rt_safety.py`, run in the CI `lint` job). The lint is a weaker
*guarantee* and a better *trade*.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-12.

- New helper `src/runtime/arbc/runtime/worker_dispatch.hpp` + `src/runtime/worker_dispatch.cpp`: `worker_backed_dispatch(WorkerPool&) → RenderDispatch` — the single enforcement site for the leaf-only rule, moving the gate and its thirteen-line rationale out of `offline_sequence.cpp`.
- Interactive swap: `src/runtime/interactive.cpp:254` now calls `worker_backed_dispatch(d_pool)` instead of `direct_dispatch()`; stale "reserved for this task" comments in `src/runtime/arbc/runtime/interactive.hpp` retired; `src/runtime/CMakeLists.txt` updated with new sources.
- Pre-existing correctness bug fixed: `src/kind_fade/fade_content.cpp` and `src/kind_crossfade/crossfade_content.cpp` were returning `exact=true` for the transparent/silence placeholder rendered when an input pull goes async — causing cold-cache parallel renders to export fully blank frames. Fixed to match `NestedContent`'s already-correct behavior; offline driver now loops composite→reap until quiesced so crossfade warms both inputs. Zero goldens changed.
- New test `tests/worker_dispatch_leaf_only.t.cpp` (6 cases): thread-identity assertions on both drivers over fade/crossfade/nested; byte-identity across worker counts 0/1/4 on both drivers; in-flight cancel, teardown-joins, and `[.nightly]` seeded TSan sweep; claim `02-architecture#worker-dispatch-is-leaf-only` registered with `enforces:` tags; `src/runtime/t/offline_sequence.t.cpp:439` tagged.
- Grep-lint `scripts/check_worker_dispatch.py` wired into CI `lint` job (`.github/workflows/ci.yml`) — enforces that `WorkerPool::submit(RenderTask{...})` appears in exactly one non-test TU.
- Nightly TSan sweep entry added to `.github/workflows/nightly.yml` — first render-dispatch binary in the `tsan-full` seed list.
- Claims register: `tests/claims/registry.tsv` +1 row (`02-architecture#worker-dispatch-is-leaf-only`); `tests/CMakeLists.txt` updated.
- Design-doc delta: `docs/design/02-architecture.md` § Threading model + `docs/design/00-overview.md` decision record — leaf-only bullet landed (same commit, per doc 16).
