#pragma once

#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/model/damage.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

// The operator-graph awareness layer for `arbc::compositor` (L4, doc 17:56).
// Doc 13's thesis is that effects are operators -- "content that consumes
// content" -- made core-visible through `Content::inputs()` (doc 13:33-52).
// `compositor.tile_planning` plans every visible layer as a flat leaf; this
// component teaches the planner, the damage router, and the counter surface to
// walk an operator's `inputs()` DAG and act on what they find, WITHOUT yet
// pulling or caching input outputs (that is `compositor.pull_service`, doc
// 13:69-89). Four pure behaviors over `const Content*`:
//
//   1. `aggregate_revision` -- a fold over the reachable `inputs()` DAG so an
//      operator's output key changes iff any reachable input changes (doc
//      05:82-91, doc 13:124-125).
//   2. `route_operator_damage` -- propagate damage on an input up to every
//      visible operator that reaches it, mapping the rect forward through the
//      composed `map_input_damage` chain (doc 13:54-57, doc 05:88-91).
//   3. `GraphBudget` / `GraphDiagnostics` -- the compositor-internal recursion
//      backstop doc 05:66-70 mandates: structural walks terminate on any cycle
//      by the visited set; the planning descent that exceeds the budget renders
//      the placeholder and reports a diagnostic naming the offending content.
//   4. `resolve_identity` -- the OpenFX identity short-circuit (doc 13:59-65):
//      a pass-through operator issues no render and creates no operator-output
//      cache entry.
//
// Pure per-frame library posture (doc 02:123-125, `compositor.hpp:26`): every
// function is a pure query over `const Content*` -- no persistent state, no
// lock, no allocation on the leaf path. `GraphBudget` is threaded by value and
// `GraphDiagnostics` by optional pointer, exactly as `DirtyRegion` /
// `CompositorCounters` are (Decisions 2, 6). Levelization (doc 17:56):
// `contract::Content` is a DEPENDS edge; `model::Damage` is reached through the
// same transitive `model` visibility `render_frame_interactive` already uses
// (`damage_planning.hpp:34-37`). No new DEPENDS edge, no `backend-cpu` edge.

namespace arbc {

// The compositor's per-request recursion-depth backstop (doc 05:66-70). The
// pure structural walks (`aggregate_revision`, `route_operator_damage`)
// terminate on any `inputs()` cycle by their visited set alone; the budget
// bounds the *planning descent* (`resolve_identity`, and future nested-
// composition descent), where a divergent cycle would otherwise not terminate.
// Threaded by value through every traversal so budgets flow through recursion
// rather than reset by it (doc 05:96-100).
inline constexpr unsigned k_max_recursion_depth = 64;

struct GraphBudget {
  unsigned max_depth{k_max_recursion_depth};
};

// One cycle/too-deep diagnostic: the offending content, the depth reached, and
// the traversal path from the descent root to it -- "a diagnostic naming the
// offending content path" (doc 05:66-70, doc 13:134-138). `content` names by
// pointer identity since input edges are raw `Content*` (`content.hpp:161`) and
// carry no id at this seam.
struct GraphDiagnostic {
  const Content* content{nullptr};
  unsigned depth{0};
  std::vector<const Content*> path;
};

// A caller-owned diagnostic sink, defaulting null so the byte-neutral path
// reports nothing, matching the `CompositorCounters*` / `DirtyRegion*` seam
// discipline (Decision 6).
struct GraphDiagnostics {
  std::vector<GraphDiagnostic> entries;
};

// True iff `content` is an operator (non-empty `inputs()`), i.e. NOT a graph
// leaf (doc 13:52). The single query the graph-aware planner branches on; a
// leaf answers `false` and keeps the byte-exact flat-key path.
inline bool is_operator(const Content* content) {
  return content != nullptr && !content->inputs().empty();
}

// Fold the reachable `inputs()` DAG of `root` into a single opaque aggregate
// revision (doc 05:82-91, doc 13:124-125). Each reachable node (including
// `root`) is folded exactly once via a `const Content*` visited set, so a shared
// diamond input is folded once and any `inputs()` cycle terminates.
//
// The combine is `acc += mix64(contribution(node))` -- a sum of a BIJECTIVE
// 64-bit mix of each contribution, not of the raw contributions
// (`model.per_object_revision` Decision 3,
// `05-recursive-composition#aggregate-fold-mixes-before-summing`). Addition is
// commutative and associative, so the fold stays order-independent (permuting
// `inputs()` yields the same value); mixing first is what makes it
// collision-resistant. A RAW sum is collision-free only while every contribution
// carries the same value, and per-object revision stamps are small monotone
// integers that cancel: two reachable inputs at stamps 7 and 3 fold to the same
// 10 as a later configuration at 6 and 4 -- reachable through an ordinary
// undo/redo interleaving, or a membership edit that swaps a high-stamp layer for
// a low-stamp one -- and a collision here serves the OTHER configuration's
// composed-result tile. Mixing before summing is what makes the registered
// "changes **iff** some reachable input's contribution changes" actually true,
// rather than true-by-accident of uniform contributions.
//
// A single node (a leaf, empty `inputs()`) degenerates to
// `mix64(contribution(root))`. The aggregate is an OPAQUE key value -- nothing
// reads meaning out of it -- so the numeric change from the raw sum invalidates
// no pixels, only cached keys.
//
// `contribution` is caller-supplied: the runtime passes each node's per-object
// revision stamp, folded with the arrangement of any composition the content
// names (`runtime/pull_identity.hpp`, Decisions 4-5). A caller with no per-object
// stamps to hand (a test double, the conformance suite) may still pass a constant
// -- correct and never stale, just less selective. Termination and full coverage
// are by the visited set; `budget` is the planning-descent backstop, not a fold
// cutoff (cutting the fold by depth would drop reachable contributions and be
// unsound), so the fold carries it only for signature uniformity.
std::uint64_t aggregate_revision(const Content* root,
                                 const std::function<std::uint64_t(const Content*)>& contribution,
                                 GraphBudget budget = {});

// A visible operator layer for damage routing: its content id (for the emitted
// `Damage.object`) and its resolved `Content*` (Decision 4). The caller builds
// this set from the visible layers whose `is_operator` holds; the visible set
// is naturally bounded by `for_each_layer`, so no document-wide
// `Content*->ObjectId` reverse map is needed.
struct OperatorLayer {
  ObjectId object{};
  const Content* content{nullptr};
};

// Route damage on an input up to the visible operators that show it (doc
// 13:54-57, 104-107; doc 05:88-91). For each operator in `operators` that
// reaches `damaged` through `inputs()`, fold `rect` forward through the composed
// chain of `map_input_damage` calls to the operator's output footprint and emit
// `Damage{operator.object, mappedRect, range}` -- unioning over every path (a
// diamond covers both), over-approximating and covering (Constraint: never
// under-report). The result feeds the landed `map_damage_to_device` /
// `invalidate_damage`. An operator that does not reach `damaged` receives none;
// an operator equal to `damaged` is skipped (damage on the operator's own object
// is the flat leaf path). Cycle-safe and depth-budgeted: each reachable node is
// evaluated once, a cycle back-edge contributes nothing, and the walk
// terminates.
std::vector<Damage> route_operator_damage(std::span<const OperatorLayer> operators,
                                          const Content* damaged, const Rect& rect,
                                          const TimeRange& range, GraphBudget budget = {});

// The outcome of resolving an operator layer's identity chain for one request.
struct IdentityResolution {
  // The content the driver should actually render/serve: the operator itself
  // (no short-circuit), the terminal input the identity chain resolves to
  // (short-circuit), or `nullptr` when the descent exceeded the budget (render
  // the placeholder).
  const Content* terminal{nullptr};
  // Whether at least one `identity()` edge was followed (a pass-through). When
  // true the driver issues NO operator render and creates NO operator-output
  // cache entry -- serving input N's tiles is `pull_service`'s (Decision 5).
  bool short_circuited{false};
  // Whether the descent exceeded `budget.max_depth` (a divergent identity
  // cycle): the layer renders the placeholder and one diagnostic was reported.
  bool budget_exceeded{false};
};

// Resolve `content`'s identity chain for `request` (doc 13:59-65,128, Decision
// 5). Follow `identity(request) == N` to input N, repeat, until a non-pass-
// through content (the terminal) or the depth budget is exceeded (a divergent
// cycle -> placeholder + one diagnostic on `diag`, when non-null). A leaf or a
// non-identity operator resolves to itself with `short_circuited == false`. A
// broken identity index (out of range / null edge) falls back to rendering the
// current node. Depth-budgeted so an identity cycle terminates on the budget,
// not on unbounded recursion.
IdentityResolution resolve_identity(const Content* content, const RenderRequest& request,
                                    GraphBudget budget = {}, GraphDiagnostics* diag = nullptr);

} // namespace arbc
