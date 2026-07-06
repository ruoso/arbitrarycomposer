#include <arbc/compositor/operator_graph.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace arbc {

std::uint64_t aggregate_revision(const Content* root,
                                 const std::function<std::uint64_t(const Content*)>& contribution,
                                 GraphBudget budget) {
  (void)budget; // Termination and full coverage are by the visited set (each
                // reachable node folded once); cutting the fold by depth would
                // drop reachable contributions and be unsound. The budget is the
                // planning-descent backstop (`resolve_identity`), carried here
                // only for signature uniformity (doc 05:66-70, and the
                // `graph-walk-bounds-cycles` claim: the fold terminates by the
                // visited set, not the budget).
  if (root == nullptr) {
    return 0;
  }
  // Iterative depth-first fold (no recursion, so no stack-depth concern even on
  // a pathologically deep acyclic DAG). Integer addition is commutative and
  // associative -> order-independent; the visited set folds each reachable node
  // exactly once, so a diamond folds its shared input once and a cycle
  // terminates. A single node degenerates to `contribution(root)`.
  std::vector<const Content*> stack;
  std::unordered_set<const Content*> visited;
  stack.push_back(root);
  visited.insert(root);
  std::uint64_t acc = 0;
  while (!stack.empty()) {
    const Content* node = stack.back();
    stack.pop_back();
    acc += contribution(node);
    for (const ContentRef input : node->inputs()) {
      if (input != nullptr && visited.insert(input).second) {
        stack.push_back(input);
      }
    }
  }
  return acc;
}

namespace {

// Map damage on `damaged` (rect `rect` on its output) up to `node`'s output,
// unioned over all `inputs()` paths and memoized per node, or `nullopt` if
// `node` does not reach `damaged`. On a cycle back-edge (a node already on the
// current descent stack) or a budget-exceeding depth the branch contributes
// nothing -- so a divergent cycle is bounded here (a convergent one bottoms out
// earlier by the sub-pixel cull, doc 05:61-65) and the walk terminates with
// each node evaluated once.
std::optional<Rect> map_damage_up(const Content* node, const Content* damaged, const Rect& rect,
                                  std::unordered_map<const Content*, std::optional<Rect>>& memo,
                                  std::unordered_set<const Content*>& on_stack, unsigned depth,
                                  GraphBudget budget) {
  if (node == damaged) {
    return rect; // the output of the damaged input is the damage rect itself
  }
  if (const auto it = memo.find(node); it != memo.end()) {
    return it->second;
  }
  if (depth >= budget.max_depth || !on_stack.insert(node).second) {
    return std::nullopt; // budget backstop or cycle back-edge: contribute nothing
  }
  std::optional<Rect> result;
  const std::span<const ContentRef> ins = node->inputs();
  for (std::size_t i = 0; i < ins.size(); ++i) {
    const ContentRef input = ins[i];
    if (input == nullptr) {
      continue;
    }
    const std::optional<Rect> sub =
        map_damage_up(input, damaged, rect, memo, on_stack, depth + 1, budget);
    if (sub.has_value()) {
      // Map input i's output damage forward to this node's output (covering /
      // over-approximating per `content.hpp:294-300`), unioning across paths.
      const Rect mapped = node->map_input_damage(i, *sub);
      result = result.has_value() ? rect_union(*result, mapped) : mapped;
    }
  }
  on_stack.erase(node);
  memo.emplace(node, result);
  return result;
}

} // namespace

std::vector<Damage> route_operator_damage(std::span<const OperatorLayer> operators,
                                          const Content* damaged, const Rect& rect,
                                          const TimeRange& range, GraphBudget budget) {
  std::vector<Damage> out;
  if (damaged == nullptr) {
    return out;
  }
  for (const OperatorLayer& op : operators) {
    if (op.content == nullptr || op.content == damaged) {
      // Route INPUT damage only; damage on the operator's own object is the flat
      // leaf path (`map_damage_to_device` / `invalidate_damage`).
      continue;
    }
    std::unordered_map<const Content*, std::optional<Rect>> memo;
    std::unordered_set<const Content*> on_stack;
    const std::optional<Rect> mapped =
        map_damage_up(op.content, damaged, rect, memo, on_stack, 0, budget);
    if (mapped.has_value()) {
      damage_add(out, Damage{op.object, *mapped, range});
    }
  }
  return out;
}

IdentityResolution resolve_identity(const Content* content, const RenderRequest& request,
                                    GraphBudget budget, GraphDiagnostics* diag) {
  IdentityResolution res;
  if (content == nullptr) {
    return res; // nothing to render: terminal stays null, not a budget failure
  }
  std::vector<const Content*> path;
  const Content* node = content;
  unsigned depth = 0;
  while (true) {
    path.push_back(node);
    const std::optional<std::size_t> id = node->identity(request);
    if (!id.has_value()) {
      res.terminal = node; // a genuine (non-pass-through) content: render/serve it
      return res;
    }
    res.short_circuited = true;
    // A divergent identity cycle exceeds the budget and degrades to the
    // placeholder plus one diagnostic naming the offending content (doc
    // 05:66-70). Checked before following the edge so a self/loop terminates.
    if (depth >= budget.max_depth) {
      if (diag != nullptr) {
        diag->entries.push_back(GraphDiagnostic{node, depth, path});
      }
      res.budget_exceeded = true;
      res.terminal = nullptr;
      return res;
    }
    const std::span<const ContentRef> ins = node->inputs();
    const std::size_t idx = *id;
    if (idx >= ins.size() || ins[idx] == nullptr) {
      // A broken identity index is not a usable pass-through: render this node.
      res.terminal = node;
      return res;
    }
    node = ins[idx];
    ++depth;
  }
}

} // namespace arbc
