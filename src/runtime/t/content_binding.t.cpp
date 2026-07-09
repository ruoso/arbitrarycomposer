#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/model/records.hpp>
#include <arbc/runtime/document.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <optional>

// Content binding (`model.content_binding`, doc 14:50-56, doc 17:66-72):
// `Document::add_content` migrates the walking-skeleton bare-id shortcut into a
// versioned `ContentRecord` published into `DocState`, while the id->Content
// vtable binding stays in the runtime side-map `resolve()` serves. These tests
// exercise the record-minting + side-map binding through the `Document` facade;
// they render no pixels (the render drivers own that under their own tasks).

namespace {

using arbc::Content;
using arbc::RenderCompletion;
using arbc::RenderRequest;
using arbc::RenderResult;
using arbc::Stability;

// A minimal leaf `Content` double: no state, no editable/audio facet. It answers
// `render` with an inert result so it satisfies the pure-virtual contract, but
// these tests never drive a render -- they witness the versioned record and the
// side-map binding, not pixels.
class StubContent : public Content {
public:
  std::optional<arbc::Rect> bounds() const override { return std::nullopt; }
  Stability stability() const override { return Stability::Static; }
  std::optional<arbc::TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<RenderResult> render(const RenderRequest&, std::shared_ptr<RenderCompletion>) override {
    return RenderResult{};
  }
};

} // namespace

// enforces: 14-data-model-and-editing#content-add-is-versioned
TEST_CASE("Document::add_content mints a versioned content record") {
  arbc::Document document;

  const std::uint64_t rev_before = document.pin()->revision();

  constexpr std::uint64_t k_kind = 42;
  const arbc::ObjectId id = document.add_content(std::make_shared<StubContent>(), k_kind);

  const arbc::DocStatePtr pinned = document.pin();

  // Exactly one new published version, like every other Document mutator.
  CHECK(pinned->revision() == rev_before + 1);

  // The returned id is the record's own key in the published DocState, and the
  // record embeds the caller-supplied kind and an inert (k_state_none) handle.
  const arbc::ContentRecord* rec = pinned->find_content(id);
  REQUIRE(rec != nullptr);
  CHECK(rec->kind == k_kind);
  CHECK_FALSE(rec->state.has_state());

  // Each add self-commits its own version and gets its own distinct record key.
  const arbc::ObjectId id2 = document.add_content(std::make_shared<StubContent>(), 7);
  const arbc::DocStatePtr pinned2 = document.pin();
  CHECK(pinned2->revision() == rev_before + 2);
  CHECK(id2 != id);
  const arbc::ContentRecord* rec2 = pinned2->find_content(id2);
  REQUIRE(rec2 != nullptr);
  CHECK(rec2->kind == 7);

  // Content lifetime is independent of any referencing layer: the first record is
  // still a live top-level map entry even though no layer binds it.
  CHECK(pinned2->find_content(id) != nullptr);

  // A default kind (the zero-blast case) round-trips as 0.
  const arbc::ObjectId id3 = document.add_content(std::make_shared<StubContent>());
  const arbc::ContentRecord* rec3 = document.pin()->find_content(id3);
  REQUIRE(rec3 != nullptr);
  CHECK(rec3->kind == 0);
}

// enforces: 14-data-model-and-editing#content-binding-resolves-via-side-map
TEST_CASE("the id-to-Content binding is served by the runtime side-map, not the model") {
  arbc::Document document;

  // A version pinned BEFORE the add must never observe the new record.
  const arbc::DocStatePtr before = document.pin();

  const auto content = std::make_shared<StubContent>();
  Content* const raw = content.get();
  const arbc::ObjectId id = document.add_content(content, 3);

  // resolve() returns the exact live Content* that was added -- the side-map
  // binding, unchanged by the migration.
  CHECK(document.resolve(id) == raw);

  // The versioned record exposes only the opaque {kind, state}; it carries no
  // vtable pointer (the model stays free of the Content vtable, doc 17:70-71).
  const arbc::ContentRecord* rec = document.pin()->find_content(id);
  REQUIRE(rec != nullptr);
  CHECK(rec->kind == 3);

  // Pinned-version isolation: the pre-add snapshot has no such record.
  CHECK(before->find_content(id) == nullptr);

  // The side-map is the sole home of the binding: an unknown id resolves to null.
  CHECK(document.resolve(arbc::ObjectId{}) == nullptr);
}
