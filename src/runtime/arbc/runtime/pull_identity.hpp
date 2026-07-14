#ifndef ARBC_RUNTIME_PULL_IDENTITY_HPP
#define ARBC_RUNTIME_PULL_IDENTITY_HPP

#include <arbc/base/ids.hpp> // ObjectId

#include <cstdint>
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

// The SECOND COLUMN of the same per-revision memo (`model.per_object_revision`
// Decision 4): each identified object's revision CONTRIBUTION -- the value the
// compositor projects into the opaque `TileKey`/`BlockKey` revision slot in place of the
// document-global revision, so an edit to one object stops orphaning every other
// object's cached tiles.
//
// Keyed by `ObjectId` rather than `Content*` because BOTH read sides need it and they
// hold different things: the visual/pull seams (`PullConfig::contribution`) receive a
// raw `Content*`, while the audio lookahead ring computes its warm keys from an
// `ObjectId` (`lookahead.hpp`). One map, two functors over it, is what keeps the ring's
// write-side key and the pull's read-side probe key equal BY CONSTRUCTION rather than by
// coincidence -- if they diverged the ring would warm blocks nobody probes and every
// pull would miss.
using PullStampMap = std::unordered_map<ObjectId, std::uint64_t>;

// Build the contribution column over a pinned revision and an ALREADY-built identity
// map. For each identified content `N`:
//
//     contribution(N) = mix64(object_revision(id_of(N)))
//                     + (N->composition_ref().valid()
//                          ? composition_revision(N->composition_ref())
//                          : 0)
//
// The second term is Decision 5, and it is a wrong-pixel guard, not a refinement. The
// compositor's aggregate fold walks `inputs()`, which yields CONTENTS -- but a nested
// content's composed pixels also depend on the child composition's layer ORDER and on
// each member layer's transform / opacity / span / time map, which live on records with
// their own ids and are reachable from no content. Reorder a child composition, or nudge
// one member layer's transform, and no child content's stamp moves: with the content
// stamp alone the embedder's composed-result key would be unchanged and the cache would
// serve the PRE-EDIT composite. Today's document-global revision masks this by bumping
// everything.
//
// This is where the two halves are joined, and it has to be HERE: `composition_ref()` is
// on the `Content` vtable and doc 17:80-84 keeps the model free of that vtable, while
// `object_revision` / `composition_revision` are pure model vocabulary. `runtime` is the
// only level that sees both, so no new levelization edge is introduced.
//
// EAGER and IMMUTABLE, built on the frame/driver thread over the pinned (immutable)
// `DocRoot`, then read lock-free by parallel render workers -- the same posture
// `build_pull_identity_map` already has. A lazily-filled memo would need mutable state
// and a lock on the pull path, which is precisely the shape that produced the
// audio-lookahead cache-thread-safety trap already on record.
std::shared_ptr<const PullStampMap> build_pull_stamp_map(const DocRoot& state,
                                                         const PullIdentityMap& ids);

// The `PullConfig::contribution` functor: `Content* -> contribution`, composing the two
// memo columns. A content with no identity, or an id with no stamp (a synthesized
// operator-input child is not a model object), contributes 0 -- which is sound because
// such a content renders as a pure function of its own inputs and of the operator record
// that constructed it, and BOTH are model objects reachable in the same `inputs()` fold.
std::function<std::uint64_t(const Content*)>
pull_contribution_of(std::shared_ptr<const PullIdentityMap> ids,
                     std::shared_ptr<const PullStampMap> stamps);

// The same column read by id: the audio lookahead ring's warm-key contribution
// (`LookaheadRingConfig::contribution`). Same map, same values, so the ring warms exactly
// the keys `PullServiceImpl::pull_audio` later probes.
std::function<std::uint64_t(ObjectId)>
object_contribution_of(std::shared_ptr<const PullStampMap> stamps);

} // namespace arbc

#endif // ARBC_RUNTIME_PULL_IDENTITY_HPP
