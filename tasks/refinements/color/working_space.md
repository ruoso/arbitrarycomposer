# color.working_space — Per-composition working space

## TaskJuggler entry

`tasks/15-color.tji` → `color.working_space` ("Per-composition working
space").

## Effort estimate

1d.

## Inherited dependencies

- `color.format_set` — pending at refinement time: provides
  `SurfaceFormat` (the tag triple) that a working-space configuration
  *is*.
- NOT inherited: `color.kernels`. This task lands before or independently
  of kernels; the consequences are staged (see Decisions).

## What this task is

Make the working space **per-composition configuration** (doc 07 rule 2):
a composition-level record field holding the `SurfaceFormat` the
compositor blends in, plumbed from the model through frame planning to
target/temp surface creation — replacing the `Rgba32fLinearPremul`
hardcodes in `render_offline` and the compositor. Nesting-boundary
conversion (doc 07 rule 4) is `kinds.nested`'s wiring; display output
transforms are the interactive renderer's; this task is the
configuration and its plumbing.

## Why it needs to be done

Doc 07 rule 2 ("all compositing happens in the composition's working
space") currently holds by hardcode, not by model. The nested kind
(different working spaces per composition, converting at the boundary)
and serialization (`working_space` is a document field, doc 08's example
shows it) both need the real field. Gates `kinds.nested` (M4) via the
WBS edge added for exactly this.

## Inputs / context

- `docs/design/07-color-and-pixel-formats.md` — rules 2–4; the default
  (premultiplied linear-light sRGB primaries, RGBA16F).
- `docs/design/08-serialization.md` — the `working_space` field in the
  format example (this task defines what it configures; serialization
  itself is the `serialize` stream).
- `src/model/arbc/model/model.hpp` — `DocState` currently has no
  composition-level record; this task introduces one (revision-carrying,
  transaction-mutated like everything else).
- `src/compositor/compositor.cpp:50` (temp surface creation) and
  `src/runtime/offline.cpp` (target creation) — the two hardcodes.
- `docs/design/17-internal-components.md` — component table: `model`
  currently lists deps `{base, pool}`; storing a `SurfaceFormat` in a
  model record needs the `model → media` edge (see Decisions — this is a
  design-doc delta).

## Constraints / requirements

- `DocState` gains a composition record (`working_space`, and a natural
  home for the doc 01 canvas hint later — but scope only `working_space`
  now) mutated through `Transaction` like layer records, bumping the
  revision.
- The compositor reads the pinned state's working space for every
  temp/target allocation; `render_offline`'s format parameter-less
  hardcode goes away.
- **Default remains `Rgba32fLinearPremul` until `color.kernels` makes
  16f storable** — the doc 07 default (16f) flips in the kernels task's
  wiring (its refinement's Status must note the flip; if kernels lands
  first, this task sets the doc 07 default directly). Configured values
  the backend cannot store surface as errors at render time (capability
  honesty from `color.format_set`).
- **Design-doc delta (rides this task's commit)**: doc 17's component
  table and `scripts/check_levels.py` gain `media` in `model`'s allowed
  deps, with the one-line rationale that media descriptors are
  level-1 vocabulary and composition records are precisely where
  configuration vocabulary lives. No other component's edges change.
- Levelization otherwise unchanged: the compositor already sees both
  model and media through its closure.

## Acceptance criteria

- Unit tests (`src/model/t/`): working space defaults on a fresh model;
  set via transaction bumps revision; pinned states see their version's
  value (the doc 14 property extended to composition records).
- Integration test: `render_offline` produces a target whose tag triple
  equals the composition's configured working space; configuring an
  unstorable format yields the error path, not a crash.
- Claim (register + `enforces:`): `07-color-and-pixel-formats#compositing-in-working-space`
  — every surface the compositor allocates in a frame carries the
  composition's working-space tags (asserted via the tag triple on temps
  and target).
- `check_levels.py` passes with the amended table; doc 17 delta included
  in the same change; walking-skeleton goldens unchanged (default is
  still 32f).

## Decisions

- **`model → media` levelization edge, by design-doc delta.** The
  alternative — storing the working space as opaque integers in the
  model and interpreting them upstairs — keeps the table pristine at the
  cost of untyped configuration in the document model, which is exactly
  the kind of implicit convention doc 07 rule 1 exists to prevent.
  Media is pure level-1 vocabulary (descriptors, no machinery); the edge
  is levelization-clean (downward) and doc 17's table simply didn't
  anticipate composition-record configuration. Amend the constitution
  explicitly rather than smuggle around it.
- **Composition record now, singular.** The skeleton's `DocState` models
  exactly one composition; this task adds the composition record without
  multi-composition document structure (that lands with `kinds.nested` /
  `model.persistent_state`'s real shape). Rejected: blocking on the full
  model rework — the field and its plumbing are what downstream tasks
  need, and the record migrates with everything else.
- **Staged default** (32f now, 16f when storable) rather than configuring
  the doc 07 default into an error path on day one: a fresh document must
  render out of the box at every commit in between. The doc 07 default
  is a *target* the kernels task activates; this task makes the
  configuration exist.

## Open questions

(none — all decided)

## Status

_pending implementation_
