#pragma once

// The contract conformance suite (`arbc-testing`), doc 16:31-44 -- the
// "crown jewel". A reusable, property-based harness that any `Content`
// implementation runs against to prove it honors the algebraic invariants the
// layer contract (doc 03) promises. Plugin authors, reference kinds, and the
// core-repo conformance driver all call `arbc::contract_tests(factory)` (or a
// granular per-family entry point) from inside their own Catch2 `TEST_CASE`.
//
// The suite is property-based and seeded (doc 16:39-40, Decision 5): every
// region, scale, time, and state handle is drawn from a suite-owned PRNG
// threaded off `Options::seed`, so a fixed seed reproduces an exact case set
// (CI-stable, and the surface the quality stress harness perturbs). No
// wall-clock, no ambient randomness.
//
// Contract-surface note. Doc 16 frames several families around an `Editable`
// facet and a `scale_range()` description method. Neither exists on `Content`
// yet -- the editable/audio facets are declared future work (content.hpp:163)
// and `scale_range()` was never added. At the contract level the only
// observable of editable state is `RenderRequest::snapshot` (the `StateHandle`
// that `snapshot_pins` established makes render a pure function of
// `(state, region, scale, time)`), so the capture/restore and facet-
// consistency families are expressed against the snapshot handle and the
// description methods that DO exist (`bounds`/`stability`/`time_extent`).
// Family skipping keys off `stability()` -- `Live` content opts out of the
// determinism-dependent families (doc 14:173-174), the contract-level analog
// of "content that does not expose the facet is skipped".

#include <arbc/base/geometry.hpp> // Rect
#include <arbc/base/time.hpp>     // Time
#include <arbc/contract/content.hpp>
#include <arbc/model/records.hpp> // StateHandle

#include <cstdint>
#include <functional>
#include <memory>

namespace arbc::testing {

// Produces a FRESH `Content` per invocation (doc 16:41). Each property case
// renders against a clean instance, so a family never observes state one case
// leaked into the next.
using ContentFactory = std::function<std::unique_ptr<Content>()>;

// Seed + coverage knobs for a suite run. Defaults give a fixed seed and a
// modest case count so a plain `contract_tests(factory)` is deterministic and
// fast; downstream raises `cases` for a heavier sweep.
struct Options {
  std::uint64_t seed{0x9e3779b97f4a7c15ULL}; // golden-ratio odd constant
  int cases{16};                             // property cases per family
  int width{4};                              // render-target width (device px)
  int height{4};                             // render-target height (device px)

  // Whether this content's `snapshot` handle is a genuine render input -- an
  // "editable" content whose pixels change with the pinned state. When set,
  // `check_render_purity` additionally asserts two requests differing only in
  // `snapshot` yield DIFFERENT pixels (the handle is a real input, not
  // ignored). Snapshot-insensitive content (a solid fill) leaves this false;
  // its capture/restore round-trip still holds, trivially.
  bool snapshot_sensitive{false};

  // Per-family toggles for the umbrella entry point.
  bool render_purity{true};
  bool scale_honesty{true};
  bool time_honesty{true};
  bool bounds_honesty{true};
  bool capture_restore{true};
  bool async_cancellation{true};
  bool facet_consistency{true};
  bool operator_graph{true};
};

// --- The seven doc-16 property families, as granular entry points so a
//     downstream task can run one family in isolation (doc 16:41, Constraint 2).

// Render purity (re-runs `#render-pure-over-pinned-state`): an identical
// `RenderRequest` yields byte-identical target pixels across calls. When
// `options.snapshot_sensitive`, two requests differing only in `snapshot`
// yield different pixels. Skipped for `Live` content.
void check_render_purity(const ContentFactory& factory, const Options& options = {});

// Scale honesty (`#render-scale-honest`): `achieved_scale` never exceeds
// `request.scale`; a degraded render (`achieved_scale < request.scale`) is
// never reported `exact`; an `Exact` request is rendered faithfully.
void check_scale_honesty(const ContentFactory& factory, const Options& options = {});

// Time honesty (re-runs `#render-time-honest` / `#static-time-invariant`):
// `Static` content ignores `time`, painting identically across times and
// reporting `nullopt` `achieved_time` and `time_extent`; `Timed` content is a
// deterministic function of time and reports the `achieved_time` it rendered.
void check_time_honesty(const ContentFactory& factory, const Options& options = {});

// Bounds/extent honesty (`#render-within-declared-bounds`): a request whose
// region lies entirely outside `bounds()` (or, for `Timed` content, whose time
// lies outside `time_extent()`) yields empty/transparent output.
void check_bounds_honesty(const ContentFactory& factory, const Options& options = {});

// Capture/restore round-trip (`#capture-restore-roundtrip`): re-rendering a
// previously captured `snapshot` reproduces its pixels byte-for-byte, even
// after intervening renders under other handles -- restoring a captured state
// reproduces its render exactly. Skipped for `Live` content (opts out, doc
// 14:173-174).
void check_capture_restore_roundtrip(const ContentFactory& factory, const Options& options = {});

// Async completion + cancellation (re-runs `#render-inline-or-async` /
// `#render-completion-settles-once`): render answers along one settle path
// (inline value OR nullopt+`RenderCompletion`); a `RenderCompletion` settles
// exactly once and `cancelled()` is advisory -- verified under concurrent
// `complete`/`cancel`/`take` (the TSan-covered family, Constraint 6).
void check_async_cancellation(const ContentFactory& factory, const Options& options = {});

// Facet consistency (`#facet-consistency`): the description methods
// (`bounds`/`stability`/`time_extent`) are idempotent and mutually coherent
// (`Static` <=> `nullopt` `time_extent`; `Timed` => a `time_extent`).
void check_facet_consistency(const ContentFactory& factory, const Options& options = {});

// --- Operator-graph member properties (doc 03:98-102 / doc 13, Decision 3).

// Leaf property (re-runs `#leaf-content-has-no-operator-graph`): a content
// overriding no operator-graph member exposes an empty `inputs()` span,
// `nullopt` `identity()` for every request, and the identity `map_input_damage`.
void check_leaf_no_operator_graph(const ContentFactory& factory, const Options& options = {});

// Names an input edit for the operator over-approximation check: input `input`
// changes from state `before` to `after`, a difference confined to
// `input_damage`. The suite renders the operator under each handle and
// confirms every output pixel that actually changed lies within
// `map_input_damage(input, input_damage)`.
struct OperatorDamageCase {
  std::size_t input{0};
  StateHandle before{};
  StateHandle after{};
  Rect input_damage{};
};

// Operator damage covering (`#operator-damage-covers`): `map_input_damage`
// over-approximates -- the true output footprint of an input-damage rect is
// contained in the reported mapped rect (never under-reports).
void check_operator_damage_covers(const ContentFactory& factory, const OperatorDamageCase& edit,
                                  const Options& options = {});

// Operator identity faithfulness (`#operator-identity-faithful`): at a request
// for which `identity()` returns input `N`, the operator's rendered output is
// byte-identical to input `N`'s output. `identity_time` is a request time at
// which the operator is expected to report identity.
void check_operator_identity_faithful(const ContentFactory& factory, Time identity_time,
                                      const Options& options = {});

// Undamaged-region stability (`#undamaged-regions-stable`): after an edit
// reporting damage confined to `edit.input_damage`, re-rendering any region
// disjoint from it returns bit-identical pixels. `edit.input` is unused here
// (the edit is on the content's own state); reuse `OperatorDamageCase` for the
// (before, after, damage) triple.
void check_damage_soundness(const ContentFactory& factory, const OperatorDamageCase& edit,
                            const Options& options = {});

} // namespace arbc::testing

namespace arbc {

// The umbrella entry point (doc 16:41). Runs every enabled family that a
// black-box `Content` can be driven through from a factory alone: render
// purity, scale/time honesty, bounds honesty, capture/restore round-trip,
// async completion+cancellation, facet consistency, and the leaf-vs-operator
// structural check. The damage-soundness and operator covering/identity
// families need caller-supplied (before, after, damage)/identity metadata, so
// the conformance driver invokes their granular entry points directly.
void contract_tests(const testing::ContentFactory& factory, const testing::Options& options = {});

} // namespace arbc
