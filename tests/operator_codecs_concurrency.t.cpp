// Tier-6 TSan lane (runtime.operator_codecs Constraint 10, mirroring
// runtime.document_serialize Decision 6): an operator-graph save captured on the
// writer thread -- INCLUDING the input-child reverse-map capture that walks
// Content::inputs() -- emits off-thread against ongoing edits without a torn read. The
// single writer thread (main) churns add_content/attach mutations while a background
// thread re-emits a pinned ContentSnapshot; the snapshot is fully immutable (a pinned
// DocRoot + captured raw Content* + owned kind strings), so the emit shares no mutable
// state with the writer. Assertions are OUTCOMES only -- the off-thread bytes equal a
// synchronous save of the same pinned revision, and TSan is clean -- never a
// wall-clock assertion (doc 16). Catch2 macros are main-thread-only, so the worker
// latches its verdicts into atomics.

#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/kind_crossfade/crossfade_content.hpp>
#include <arbc/kind_fade/fade_content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/document_serialize.hpp>
#include <arbc/serialize/codec.hpp> // CodecTable (to hold builtin_codecs() by value)

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>

TEST_CASE("a captured operator-graph snapshot serializes off-thread while the writer mutates") {
  using arbc::Affine;
  using arbc::CrossfadeContent;
  using arbc::CrossfadeParams;
  using arbc::CrossfadeShape;
  using arbc::Document;
  using arbc::FadeContent;
  using arbc::FadeParams;
  using arbc::FadeShape;
  using arbc::FadeWindow;
  using arbc::KindBridge;
  using arbc::ObjectId;
  using arbc::Rgba;
  using arbc::SolidContent;
  using arbc::Time;

  const arbc::CodecTable codecs = arbc::builtin_codecs();

  KindBridge bridge;
  Document doc;
  const ObjectId comp = doc.add_composition(1920.0, 1080.0);
  const ObjectId shared =
      doc.add_content(std::make_shared<SolidContent>(Rgba{1.0F, 0.5F, 0.25F, 1.0F}),
                      bridge.intern(SolidContent::kind_id, "1"));
  const ObjectId other =
      doc.add_content(std::make_shared<SolidContent>(Rgba{0.0F, 0.0F, 1.0F, 1.0F}),
                      bridge.intern(SolidContent::kind_id, "1"));
  const FadeParams fp{FadeShape::Linear,
                      FadeWindow{Time{0}, Time{Time::flicks_per_second}}, std::nullopt};
  const ObjectId fade =
      doc.add_content(std::make_shared<FadeContent>(doc.resolve(shared), fp),
                      bridge.intern(FadeContent::kind_id, "1"));
  const CrossfadeParams cp{CrossfadeShape::Linear, Time{0}, Time{Time::flicks_per_second}};
  const ObjectId crossfade = doc.add_content(
      std::make_shared<CrossfadeContent>(doc.resolve(shared), doc.resolve(other), cp),
      bridge.intern(CrossfadeContent::kind_id, "1"));
  doc.attach_layer(comp, doc.add_layer(fade, Affine::identity(), 1.0));
  doc.attach_layer(comp, doc.add_layer(crossfade, Affine::identity(), 1.0));

  // Capture on the writer thread (this thread), then compute the reference bytes once.
  const arbc::ContentSnapshot snap = arbc::capture_snapshot(doc, bridge);
  const arbc::expected<std::string, arbc::SerializeError> reference =
      arbc::serialize_snapshot(snap, codecs);
  REQUIRE(reference.has_value());
  const std::string expected_bytes = *reference;

  constexpr int k_iterations = 2000;
  std::atomic<bool> serialize_error{false};
  std::atomic<bool> mismatch{false};
  std::atomic<int> serialized{0};

  std::thread reader([&] {
    for (int i = 0; i < k_iterations; ++i) {
      const arbc::expected<std::string, arbc::SerializeError> out =
          arbc::serialize_snapshot(snap, codecs);
      if (!out.has_value()) {
        serialize_error.store(true);
      } else if (*out != expected_bytes) {
        mismatch.store(true);
      } else {
        serialized.fetch_add(1);
      }
    }
  });

  // The main thread is the single writer: append fresh content + layer each round.
  for (int i = 0; i < k_iterations; ++i) {
    const ObjectId c =
        doc.add_content(std::make_shared<SolidContent>(Rgba{0.0F, 0.0F, 0.0F, 1.0F}),
                        bridge.intern(SolidContent::kind_id, "1"));
    doc.attach_layer(comp, doc.add_layer(c, Affine::identity(), 1.0));
  }

  reader.join();
  CHECK_FALSE(serialize_error.load());
  CHECK_FALSE(mismatch.load());
  CHECK(serialized.load() == k_iterations);
  CHECK(doc.pin()->revision() > snap.state->revision());
}
