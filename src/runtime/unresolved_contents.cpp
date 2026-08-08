#include <arbc/base/geometry.hpp>    // Rect::empty
#include <arbc/contract/content.hpp> // Content, for_each_input, composition_ref()
#include <arbc/model/model.hpp>      // DocRoot::for_each_layer_in / find_layer / find_composition
#include <arbc/model/records.hpp>    // LayerRecord
#include <arbc/runtime/unresolved_contents.hpp>

#include <cstddef>
#include <optional>
#include <unordered_set>
#include <vector>

namespace arbc {
namespace {

// The predicate the header states, and nothing more: a content is unresolved when it NAMES an
// external target and does not have it. Both arms are checked -- a kind may in principle name
// both -- but each contributes at most one to the count, because the count is of CONTENTS
// (one blank region), not of references.
bool content_unresolved(const DocRoot& state, const Content& content) {
  if (!content.external_composition_ref().empty()) {
    // PENDING (a valid pre-allocated id naming no record yet) and UNAVAILABLE (no id at all)
    // are one thing here: the cell composites the doc-05 placeholder either way.
    const ObjectId child = content.composition_ref();
    if (!child.valid() || state.find_composition(child) == nullptr) {
      return true;
    }
  }
  if (!content.external_asset_ref().empty()) {
    // EMPTY bounds, not `nullopt`: `nullopt` means UNBOUNDED (doc 03:76-78), which an
    // asset-backed content is not. Empty is the no-bytes shape both pending and unavailable
    // are minted in (`image_content.cpp:463-472`).
    const std::optional<Rect> extent = content.bounds();
    if (extent.has_value() && extent->empty()) {
      return true;
    }
  }
  return false;
}

} // namespace

std::size_t count_unresolved_contents(const DocRoot& state,
                                      const std::function<Content*(ObjectId)>& resolve,
                                      ObjectId anchor) {
  if (!anchor.valid()) {
    return 0; // a document with no composition draws nothing and can hold no hole
  }
  std::size_t count = 0;
  // Two frontiers over one visited pair, mirroring `build_pull_identity_map`'s frontier walk
  // (`pull_identity.cpp`): compositions to enumerate, and contents whose edges to descend. The
  // guards are what make a self-referencing document terminate -- a document referencing
  // itself dedups to its own root composition (doc 08 Principle 3), so the composition guard
  // is load-bearing, not defensive.
  std::vector<ObjectId> compositions{anchor};
  std::unordered_set<ObjectId> seen_compositions{anchor};
  std::vector<const Content*> contents;
  std::unordered_set<const Content*> seen_contents;

  const auto reach_content = [&](const Content* content) {
    if (content == nullptr || !seen_contents.insert(content).second) {
      return; // an unresolved layer (a dangling record, not our question), or already counted
    }
    contents.push_back(content);
    if (content_unresolved(state, *content)) {
      ++count;
    }
  };

  while (!compositions.empty() || !contents.empty()) {
    if (!compositions.empty()) {
      const ObjectId composition = compositions.back();
      compositions.pop_back();
      state.for_each_layer_in(composition, [&](ObjectId layer_id) {
        const LayerRecord* const layer = state.find_layer(layer_id);
        if (layer == nullptr) {
          return;
        }
        reach_content(resolve(layer->content));
      });
      continue;
    }
    const Content* const content = contents.back();
    contents.pop_back();
    // Locked edge read (`for_each_input`), the same discipline every cross-thread edge walk
    // in the runtime uses: a bind on another thread can rebuild a memo-backed edge buffer.
    for_each_input(*content,
                   [&](std::size_t /*index*/, ContentRef input) { reach_content(input); });
    // Descend the nesting boundary. Unlike `build_pull_identity_map` -- which refuses to walk
    // an EXTERNAL child because the serializer refuses to inline it -- this walk descends an
    // external child that IS installed: its records are in this document's model, its members
    // composite in this frame, and a reference that resolved one level and not the next is
    // still a hole in the export. A child that is NOT installed was already counted above and
    // enumerates no layers, so it costs nothing here.
    if (const ObjectId child = content->composition_ref();
        child.valid() && seen_compositions.insert(child).second) {
      compositions.push_back(child);
    }
  }
  return count;
}

} // namespace arbc
