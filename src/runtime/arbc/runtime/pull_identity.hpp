#ifndef ARBC_RUNTIME_PULL_IDENTITY_HPP
#define ARBC_RUNTIME_PULL_IDENTITY_HPP

#include <arbc/base/ids.hpp> // ObjectId

#include <functional>
#include <memory>
#include <unordered_map>

namespace arbc {

class Content;
class DocRoot;

// The reverse `Content* -> ObjectId` cache-identity map the offline/export render
// drivers feed to `PullConfig::id_of` (doc 13:141-154 -- "input tiles cache under
// the input's identity"). Keyed by `Content*` pointer identity so a single content
// reached from more than one parent keys under ONE identity, "shared by every
// consumer" (Constraint 2).
using PullIdentityMap = std::unordered_map<const Content*, ObjectId>;

// Build the identity map over a pinned revision (`runtime.operator_input_cache_identity`).
// Seeds from `state.for_each_layer` (layer roots carry a model `ObjectId`), then
// walks `Content::inputs()` transitively (mirroring `document_serialize.cpp`'s
// frontier walk with a `walked` guard) and assigns every reachable, un-mapped
// operator input child a freshly-synthesized `ObjectId` from the RESERVED half of
// the id space (`synthetic_id`, `base/ids.hpp` -- doc 14 § Identity). The model
// allocator never issues a reserved id, so a synthesized identity is disjoint from
// EVERY model `ObjectId` in the document -- not merely from the seeded layer roots,
// and not merely from the ids allocated so far. It is also distinct from every
// sibling child (the counter increments per emplace). So two same-stability inputs
// of one operator no longer both fall to `ObjectId{}` and alias one cache key, and
// no synthesized id can be evicted by damage naming a model object (doc 02:94-95
// invalidates by content id alone, revision-agnostic). `resolve` maps a layer
// record's `ObjectId` to its live `Content*` (the driver's document resolver). The
// map is built over the immutable pinned graph on the driver thread; the returned
// `const` map is safe to share read-only across parallel render workers (Constraint 7).
std::shared_ptr<const PullIdentityMap>
build_pull_identity_map(const DocRoot& state, const std::function<Content*(ObjectId)>& resolve);

// The `PullConfig::id_of` functor over an ALREADY-built identity map: a `Content*`
// with no entry (should not occur for a graph-reachable node) falls back to the
// default (root) `ObjectId{}`, exactly as the pre-existing driver lambdas did. The
// interactive driver memoizes one map per revision and needs both the functor and
// the map itself (to invert it for arrival-damage routing), so the lookup rule
// lives here once rather than being hand-rolled at the second call site.
std::function<ObjectId(const Content*)>
pull_identity_of(std::shared_ptr<const PullIdentityMap> ids);

// The `PullConfig::id_of` functor over a freshly built identity map. Both render
// drivers call this so the seam lands in one place (Constraint 8); a future
// interactive-audio `id_of` wiring reuses it verbatim.
std::function<ObjectId(const Content*)>
make_pull_identity_of(const DocRoot& state, const std::function<Content*(ObjectId)>& resolve);

} // namespace arbc

#endif // ARBC_RUNTIME_PULL_IDENTITY_HPP
