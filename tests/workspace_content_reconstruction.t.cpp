// The reconstructing reopen of `runtime.workspace_content_reconstruction` (issue #19):
// a workspace file that gives back its CONTENT, not just its record graph.
//
// `Document::open` restores records and binds no `Content` for any kind, so a reopened
// workspace can be neither rendered, edited nor hit-tested. Three things were missing
// from the file, not one: a content's construction parameters (a solid's colour lived
// only in the `Content` object), a stable kind identity (`ContentRecord.kind` is a
// `KindBridge` token interned per session in first-sight order), and an operator's
// input edges (doc 08 Principle 6 calls them core-owned; the canonical writer persists
// them, the workspace did not). `ContentRecord` now carries all three, captured through
// the kind's own registered codec at `add_content`, and `open_document` rebuilds each
// content through the SAME routing `load_document` uses for a canonical file.
//
// Cross-component (a runtime `Document` over a pool workspace file, plus the serialize
// codec table and real kinds), so it lives here rather than in src/runtime/t/.

#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/builtin_kinds.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/kind_fade/fade_content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/document_serialize.hpp>
#include <arbc/serialize/codec.hpp> // CodecTable (to hold builtin_codecs() by value)
#include <arbc/serialize/load_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#if ARBC_HAS_WORKSPACE_FILES
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif
#endif

#if ARBC_HAS_WORKSPACE_FILES

namespace {

using arbc::Document;
using arbc::KindBridge;
using arbc::ObjectId;
using arbc::Rgba;
using arbc::SolidContent;

// The `TempPath` recipe is src/runtime/t/housekeeping.t.cpp's.
class TempPath {
public:
  TempPath() {
#if defined(_WIN32)
    char dir[MAX_PATH];
    const DWORD n = ::GetTempPathA(MAX_PATH, dir);
    char buf[MAX_PATH];
    if (n != 0 && n < static_cast<DWORD>(MAX_PATH) && ::GetTempFileNameA(dir, "wcr", 0, buf) != 0) {
      d_path = buf;
    }
#else
    char tmpl[] = "/tmp/arbc_wcr_XXXXXX";
    const int fd = ::mkstemp(tmpl);
    if (fd >= 0) {
      ::close(fd);
    }
    d_path = tmpl;
#endif
  }
  ~TempPath() {
#if defined(_WIN32)
    ::DeleteFileA(d_path.c_str());
#else
    ::unlink(d_path.c_str());
#endif
  }
  TempPath(const TempPath&) = delete;
  TempPath& operator=(const TempPath&) = delete;
  const std::string& str() const noexcept { return d_path; }

private:
  std::string d_path;
};

// A park period long enough that no automatic tick fires, but FINITE: the park computes
// `now() + tick_period`, so `duration::max()` overflows it and the loop never wakes.
arbc::DocumentHousekeepingConfig no_cadence() {
  arbc::DocumentHousekeepingConfig config;
  config.thread.tick_period = std::chrono::hours(1);
  config.checkpoint_every_n_transactions = 0;
  return config;
}

// The registry a host running built-in kinds has: `open_document` uses it as the
// plugin-present witness, exactly as `load_document` does.
arbc::Registry builtin_registry() {
  arbc::Registry registry;
  arbc::register_builtin_kinds(registry);
  return registry;
}

} // namespace

// enforces: 15-memory-model#workspace-reopen-rebuilds-its-content
TEST_CASE("a reopened workspace rebuilds a leaf content with its own parameters") {
  TempPath path;
  const arbc::CodecTable codecs = arbc::builtin_codecs();
  const arbc::Rgba colour{0.25F, 0.5F, 0.75F, 1.0F};
  ObjectId comp{};
  ObjectId cid{};
  ObjectId layer{};
  {
    KindBridge bridge;
    auto made = Document::create(path.str(), no_cadence());
    REQUIRE(made.has_value());
    Document& doc = **made;
    // The capture hook is what makes the record self-describing. Without it the
    // record keeps only the per-session kind token, which is what issue #19 reports.
    doc.set_content_identity_capture(arbc::codec_identity_capture(codecs, bridge));
    comp = doc.add_composition(8.0, 8.0);
    cid = doc.add_content(std::make_shared<SolidContent>(colour),
                          bridge.intern(SolidContent::kind_id, "1"));
    layer = doc.add_layer(cid, arbc::Affine::identity());
    doc.attach_layer(comp, layer);
    REQUIRE(doc.checkpoint().has_value());
  }

  const arbc::Registry registry = builtin_registry();
  arbc::LoadContext ctx{std::string{}};
  KindBridge bridge;
  auto reopened = arbc::open_document(path.str(), registry, codecs, ctx, bridge, no_cadence());
  REQUIRE(reopened.has_value());
  const std::unique_ptr<Document>& doc = reopened->document;

  // The content is BACK -- as an object, resolving on the id its layer names.
  CHECK(reopened->reconstructed == 1);
  CHECK(reopened->unreconstructed.empty());
  arbc::Content* const rebuilt = doc->resolve(cid);
  REQUIRE(rebuilt != nullptr);
  CHECK(doc->resolve(doc->pin()->find_layer(layer)->content) == rebuilt);

  // And it is the SAME solid, not a default-constructed one of the right type. That
  // distinction is the whole of refinement Decision 1: a factory called with no params
  // would have produced a transparent-black solid here, which nothing downstream could
  // tell from the real thing.
  const auto* const solid = dynamic_cast<const SolidContent*>(rebuilt);
  REQUIRE(solid != nullptr);
  CHECK(solid->color().r == colour.r);
  CHECK(solid->color().g == colour.g);
  CHECK(solid->color().b == colour.b);
  CHECK(solid->color().a == colour.a);
}

// enforces: 15-memory-model#workspace-reopen-rebuilds-its-content
TEST_CASE("a reopened workspace rebuilds an operator over its persisted input edges") {
  TempPath path;
  const arbc::CodecTable codecs = arbc::builtin_codecs();
  ObjectId leaf_id{};
  ObjectId fade_id{};
  {
    KindBridge bridge;
    auto made = Document::create(path.str(), no_cadence());
    REQUIRE(made.has_value());
    Document& doc = **made;
    doc.set_content_identity_capture(arbc::codec_identity_capture(codecs, bridge));
    const ObjectId comp = doc.add_composition(8.0, 8.0);
    const auto leaf = std::make_shared<SolidContent>(Rgba{1.0F, 0.0F, 0.0F, 1.0F});
    leaf_id = doc.add_content(leaf, bridge.intern(SolidContent::kind_id, "1"));
    fade_id = doc.add_content(std::make_shared<arbc::FadeContent>(leaf.get(), arbc::FadeParams{}),
                              bridge.intern(arbc::FadeContent::kind_id, "1"));
    doc.attach_layer(comp, doc.add_layer(fade_id, arbc::Affine::identity()));
    REQUIRE(doc.checkpoint().has_value());
  }

  const arbc::Registry registry = builtin_registry();
  arbc::LoadContext ctx{std::string{}};
  KindBridge bridge;
  auto reopened = arbc::open_document(path.str(), registry, codecs, ctx, bridge, no_cadence());
  REQUIRE(reopened.has_value());
  const std::unique_ptr<Document>& doc = reopened->document;

  // Both records rebuilt, and the operator's edge points at THIS document's rebuilt
  // leaf -- not at a second copy, and not at nothing. The input edges are core-owned
  // (doc 08 Principle 6) and now live in the record graph, which is what let the
  // reopen walk them in topological order and hand the codec live inputs.
  CHECK(reopened->reconstructed == 2);
  CHECK(reopened->unreconstructed.empty());
  arbc::Content* const leaf = doc->resolve(leaf_id);
  arbc::Content* const fade = doc->resolve(fade_id);
  REQUIRE(leaf != nullptr);
  REQUIRE(fade != nullptr);
  REQUIRE(fade->inputs().size() == 1);
  CHECK(fade->inputs()[0] == leaf);
}

// enforces: 15-memory-model#workspace-reopen-rebuilds-its-content
TEST_CASE("a content with no persisted identity is reported, never guessed at") {
  TempPath path;
  const arbc::CodecTable codecs = arbc::builtin_codecs();
  ObjectId cid{};
  {
    // NO capture hook installed: this is a workspace written before issue #19, and a
    // kind whose codec captures nothing, in one shape.
    auto made = Document::create(path.str(), no_cadence());
    REQUIRE(made.has_value());
    Document& doc = **made;
    const ObjectId comp = doc.add_composition(8.0, 8.0);
    cid = doc.add_content(std::make_shared<SolidContent>(Rgba{1.0F, 1.0F, 1.0F, 1.0F}), 1);
    doc.attach_layer(comp, doc.add_layer(cid, arbc::Affine::identity()));
    REQUIRE(doc.checkpoint().has_value());
  }

  const arbc::Registry registry = builtin_registry();
  arbc::LoadContext ctx{std::string{}};
  KindBridge bridge;
  auto reopened = arbc::open_document(path.str(), registry, codecs, ctx, bridge, no_cadence());
  REQUIRE(reopened.has_value());

  // Reported and left unbound. Binding a default-constructed solid here would be
  // silent data loss wearing a working feature's clothes; a null binding is a fact the
  // host can act on -- with `rebind_content`, which is exactly why that seam ships.
  CHECK(reopened->reconstructed == 0);
  REQUIRE(reopened->unreconstructed.size() == 1);
  CHECK(reopened->unreconstructed[0] == cid);
  CHECK(reopened->document->resolve(cid) == nullptr);

  // Back-compatible: the reopen itself SUCCEEDS. A workspace written before this task
  // must not fail to open, only to reconstruct.
  CHECK(reopened->document->pin()->find_content(cid) != nullptr);
}

// enforces: 15-memory-model#workspace-reopen-rebuilds-its-content
TEST_CASE("reconstruction survives a session that interns kinds in a different order") {
  // Finding 2 of the issue investigation, as a regression guard. `ContentRecord.kind`
  // is a token a `KindBridge` assigns on FIRST SIGHT, so the same document written by
  // one session and reopened by another that saw kinds in a different order names
  // different kinds by token. Only the persisted kind_id STRING survives that, which
  // is why the record carries the string and the reopen trusts it over the token.
  TempPath path;
  const arbc::CodecTable codecs = arbc::builtin_codecs();
  const arbc::Rgba colour{0.1F, 0.2F, 0.3F, 1.0F};
  ObjectId cid{};
  {
    KindBridge bridge;
    // Intern an unrelated kind FIRST, so the solid's token is not the one a
    // freshly-constructed bridge would hand it.
    bridge.intern("org.example.other", "1");
    bridge.intern("org.example.another", "1");
    auto made = Document::create(path.str(), no_cadence());
    REQUIRE(made.has_value());
    Document& doc = **made;
    doc.set_content_identity_capture(arbc::codec_identity_capture(codecs, bridge));
    const ObjectId comp = doc.add_composition(8.0, 8.0);
    cid = doc.add_content(std::make_shared<SolidContent>(colour),
                          bridge.intern(SolidContent::kind_id, "1"));
    doc.attach_layer(comp, doc.add_layer(cid, arbc::Affine::identity()));
    REQUIRE(doc.checkpoint().has_value());
  }

  const arbc::Registry registry = builtin_registry();
  arbc::LoadContext ctx{std::string{}};
  KindBridge bridge; // a fresh bridge: different intern order, different tokens
  auto reopened = arbc::open_document(path.str(), registry, codecs, ctx, bridge, no_cadence());
  REQUIRE(reopened.has_value());
  CHECK(reopened->reconstructed == 1);
  const auto* const solid = dynamic_cast<const SolidContent*>(reopened->document->resolve(cid));
  REQUIRE(solid != nullptr);
  CHECK(solid->color().r == colour.r);
}

// enforces: 15-memory-model#workspace-params-are-as-captured-not-as-of-checkpoint
TEST_CASE("a config rewritten through update_content_config survives a mapped reopen") {
  // Issue #38's first half, and the good news. `update_content_config` publishes a versioned
  // params rewrite THROUGH THE RECORD (issue #34), so the arena holds the edited params and a
  // reopen replays them. This is what makes it the required path for a durable param edit --
  // and it holds by construction, not because a particular mutation sequence happens to work.
  TempPath path;
  const arbc::CodecTable codecs = arbc::builtin_codecs();
  const arbc::Registry registry = builtin_registry();
  const arbc::Rgba created{1.0F, 0.0F, 0.0F, 1.0F};
  ObjectId cid{};
  {
    KindBridge bridge;
    arbc::LoadContext ctx{std::string{}};
    auto made = Document::create(path.str(), no_cadence());
    REQUIRE(made.has_value());
    Document& doc = **made;
    doc.set_content_identity_capture(arbc::codec_identity_capture(codecs, bridge));
    doc.set_content_reconstruct(arbc::codec_content_reconstruct(codecs, registry, ctx));
    const ObjectId comp = doc.add_composition(8.0, 8.0);
    cid = doc.add_content(std::make_shared<SolidContent>(created),
                          bridge.intern(SolidContent::kind_id, "1"));
    doc.attach_layer(comp, doc.add_layer(cid, arbc::Affine::identity()));

    // The post-creation edit, published the supported way. Checkpointed, NOT saved: the
    // canonical `.arbc` is a different path and re-runs the codec at save time, so it would
    // hide exactly the question being asked here.
    REQUIRE(doc.update_content_config(cid, R"({"color":[0.0,0.0,1.0,1.0]})", "Recolour"));
    REQUIRE(doc.checkpoint().has_value());
  }

  arbc::LoadContext ctx{std::string{}};
  KindBridge bridge;
  auto reopened = arbc::open_document(path.str(), registry, codecs, ctx, bridge, no_cadence());
  REQUIRE(reopened.has_value());
  CHECK(reopened->reconstructed == 1);
  const auto* const solid = dynamic_cast<const SolidContent*>(reopened->document->resolve(cid));
  REQUIRE(solid != nullptr);
  // The EDIT, not the creation. The arena carried the rewrite.
  CHECK(solid->color().b == 1.0F);
  CHECK(solid->color().r == 0.0F);
}

// enforces: 15-memory-model#workspace-params-are-as-captured-not-as-of-checkpoint
TEST_CASE("a live object that diverged from its record is rebuilt from the record") {
  // Issue #38's second half, and the limit worth stating loudly. Capture happens at
  // `add_content` and at `update_content_config`, and NOWHERE else -- no checkpoint
  // re-captures, because re-capture means running every kind's codec over every content on a
  // cadence meant to be cheap enough to run unattended. So a kind that mutates its own params
  // in place, and does not publish that through the record, leaves the arena describing what
  // it was BUILT from.
  //
  // `rebind_content` stands in for that self-mutation exactly: it swaps the live object for a
  // differently-parameterised one under the same id, publishing no version -- which is the
  // same divergence, reached through a public seam and with no test-only kind needed. The
  // point is not the seam; it is that the RECORD is what a mapped reopen replays.
  TempPath path;
  const arbc::CodecTable codecs = arbc::builtin_codecs();
  const arbc::Rgba created{1.0F, 0.0F, 0.0F, 1.0F};
  ObjectId cid{};
  {
    KindBridge bridge;
    auto made = Document::create(path.str(), no_cadence());
    REQUIRE(made.has_value());
    Document& doc = **made;
    doc.set_content_identity_capture(arbc::codec_identity_capture(codecs, bridge));
    const ObjectId comp = doc.add_composition(8.0, 8.0);
    cid = doc.add_content(std::make_shared<SolidContent>(created),
                          bridge.intern(SolidContent::kind_id, "1"));
    doc.attach_layer(comp, doc.add_layer(cid, arbc::Affine::identity()));

    // The live object now says GREEN and the record still says RED.
    REQUIRE(doc.rebind_content(cid, std::make_shared<SolidContent>(Rgba{0.0F, 1.0F, 0.0F, 1.0F})));
    const auto* const live = dynamic_cast<const SolidContent*>(doc.resolve(cid));
    REQUIRE(live != nullptr);
    REQUIRE(live->color().g == 1.0F);

    // A checkpoint does not reconcile them. This is the assertion the whole issue is about:
    // the durable record is unchanged, so the divergence is not something the cadence heals.
    REQUIRE(doc.checkpoint().has_value());
    CHECK(doc.pin()->content_identity(cid).params == R"({"color":[1.0,0.0,0.0,1.0]})");
  }

  const arbc::Registry registry = builtin_registry();
  arbc::LoadContext ctx{std::string{}};
  KindBridge bridge;
  auto reopened = arbc::open_document(path.str(), registry, codecs, ctx, bridge, no_cadence());
  REQUIRE(reopened.has_value());
  CHECK(reopened->reconstructed == 1);
  const auto* const solid = dynamic_cast<const SolidContent*>(reopened->document->resolve(cid));
  REQUIRE(solid != nullptr);
  // RED: the captured params, replayed. Reconstruction SUCCEEDED and gave back the original,
  // which is the shape that looks like success from every angle -- and is why the rule is
  // documented rather than left to be discovered.
  CHECK(solid->color().r == 1.0F);
  CHECK(solid->color().g == 0.0F);
}

#endif // ARBC_HAS_WORKSPACE_FILES
