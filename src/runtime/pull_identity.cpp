#include <arbc/contract/content.hpp> // Content, ContentRef, inputs()
#include <arbc/model/model.hpp>      // DocRoot::for_each_layer
#include <arbc/model/records.hpp>    // LayerRecord
#include <arbc/runtime/pull_identity.hpp>

#include <cstdint>
#include <unordered_set>
#include <utility>
#include <vector>

namespace arbc {

std::shared_ptr<const PullIdentityMap>
build_pull_identity_map(const DocRoot& state, const std::function<Content*(ObjectId)>& resolve) {
  auto ids = std::make_shared<PullIdentityMap>();

  // Seed the layer roots (each bound to a model `ObjectId`) in `for_each_layer`
  // order -- captured into a vector so the child walk below runs in a deterministic
  // order (Constraint 3), not the process-dependent hash order of the map.
  std::vector<const Content*> roots;
  state.for_each_layer([&](const LayerRecord& layer) {
    if (Content* c = resolve(layer.content)) {
      if (ids->emplace(c, layer.content).second) {
        roots.push_back(c);
      }
    }
  });

  // Transitive `Content::inputs()` walk over the pinned graph, mirroring the
  // serializer's frontier walk (`document_serialize.cpp:174-202`): a `walked` guard
  // stops cycles and shared re-encounters, and every newly-reached, un-mapped child
  // gets the next synthesized id. Synthesized ids are minted from the RESERVED half
  // of the id space (`synthetic_id`, bit 63 set -- `base/ids.hpp`, doc 14 § Identity),
  // which the model allocator never issues. So a synthesized id is disjoint from
  // every model `ObjectId` in the document STRUCTURALLY -- not from the layer roots
  // only, and with no dependence on allocation order or on what the model allocates
  // after this map is built. It is never `ObjectId{}` (the counter starts at 1
  // *within* the half, so the bare bit is never minted) and injective across children
  // because `next` increments per emplace. The walk order is deterministic (layer
  // order, then a LIFO frontier over the immutable graph), so the same child yields
  // the same id on every frame of one render sequence (Constraint 3, cross-frame
  // stability).
  std::uint64_t next = 1;
  std::vector<const Content*> frontier = roots;
  std::unordered_set<const Content*> walked(roots.begin(), roots.end());
  while (!frontier.empty()) {
    const Content* const c = frontier.back();
    frontier.pop_back();
    for (const ContentRef child : c->inputs()) {
      if (child == nullptr || !walked.insert(child).second) {
        continue; // null slot, or a shared child already reached (keyed once)
      }
      frontier.push_back(child);
      // A child already carrying a layer-root identity (shared root+input) keeps it:
      // that content IS a model object and should key under its real identity.
      if (ids->find(child) == ids->end()) {
        ids->emplace(child, synthetic_id(next++));
      }
    }
  }

  return ids;
}

std::function<ObjectId(const Content*)>
pull_identity_of(std::shared_ptr<const PullIdentityMap> ids) {
  return [ids = std::move(ids)](const Content* c) {
    const auto it = ids->find(c);
    return it != ids->end() ? it->second : ObjectId{};
  };
}

std::function<ObjectId(const Content*)>
make_pull_identity_of(const DocRoot& state, const std::function<Content*(ObjectId)>& resolve) {
  return pull_identity_of(build_pull_identity_map(state, resolve));
}

} // namespace arbc
