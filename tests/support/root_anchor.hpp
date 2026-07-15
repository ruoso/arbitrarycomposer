#pragma once

#include <arbc/base/ids.hpp>       // ObjectId
#include <arbc/model/model.hpp>    // DocRoot::find_first_composition, CompositionRecord
#include <arbc/model/records.hpp>  // CompositionRecord
#include <arbc/runtime/document.hpp> // Document::pin, DocStatePtr

#include <catch2/catch_test_macros.hpp>

namespace arbc::test {

// The document's root composition (lowest-id wins, the serializer /
// working_space() rule), for anchoring a `Viewport` at a test that drives the
// composition-scoped frame walk directly (compositor.root_composition_frame_walk,
// doc 05:28-36). The render drivers source this same id themselves, so a test
// going through `render_offline` / `SequenceRenderer` / `HostViewport` does not
// need it; a test calling `render_frame` / `render_frame_interactive` /
// `render_frame_anchored` directly does, since the compositor never re-derives
// the root (Decision 2/3).
inline ObjectId root_composition_of(const DocRoot& state) {
  ObjectId root{};
  const CompositionRecord* rec = nullptr;
  REQUIRE(state.find_first_composition(root, rec));
  return root;
}

inline ObjectId root_composition_of(const Document& doc) {
  const DocStatePtr pin = doc.pin();
  return root_composition_of(*pin);
}

} // namespace arbc::test
