// org.arbc.imageseq temporal prefetch: imageseq is the first production content
// the runtime's playback-hint drive fans a hint out to (doc 11:160-178,
// playback_hints). Drives drive_playback_prefetch with a real imageseq content
// as the participating decoder and a Timed anchor, asserting the temporal ring
// is bounded by the horizon and that the decoder pre-rolls exactly the frames
// the ring covers (behavioral counters, doc 16:54-62; never wall-clock).

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/rational_time.hpp>
#include <arbc/base/time.hpp>
#include <arbc/cache/key_shapes.hpp>
#include <arbc/cache/keyed_store.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/runtime/playback_hints.hpp>

#include "support/imageseq_fixtures.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>

using namespace arbc;
namespace fix = arbc::imageseq::testfix;

namespace {

Time instant(int frame) { return Time{frame * fix::k_period_flicks}; }

// Render one frame so the decoder's pre-roll anchor (its last resolved frame) is
// pinned to `frame`, matching the anchor tile key.
void prime_at(arbc::imageseq::ImageSeqContent& content, Backend& backend, int frame) {
  auto target = backend.make_surface(fix::k_width, fix::k_height, k_working_rgba32f);
  REQUIRE(target.has_value());
  auto done = std::make_shared<RenderCompletion>();
  const RenderRequest request{Rect{0.0, 0.0, fix::k_width, fix::k_height},
                              1.0,
                              instant(frame),
                              StateHandle{},
                              **target,
                              Exactness::Exact,
                              Deadline::none()};
  REQUIRE(content.render(request, done).has_value());
}

TileKey timed_anchor(int frame) {
  return TileKey{ObjectId{1}, 0, ScaleRung{0}, TileCoord{0, 0}, instant(frame)};
}

} // namespace

// enforces: 11-time-and-video#temporal-prefetch-ring-bounded-by-horizon
// enforces: 11-time-and-video#playback-prefetch-drives-temporal-ring
TEST_CASE("imageseq pre-rolls exactly the frames the temporal ring covers") {
  CpuBackend backend;
  const Time step{fix::k_period_flicks};

  SECTION("a forward hint pre-rolls the ring's frames and the ring is horizon-bounded") {
    auto content = fix::make_content();
    prime_at(*content, backend, 0); // pre-roll anchor at frame 0; one decode so far
    const std::uint64_t before = content->decodes_issued();
    REQUIRE(before == 1);

    // horizon = 2 native periods -> ring K = horizon/step = 2 buckets ahead.
    const PlaybackHint hint{+1, Rational{1, 1}, Time{2 * fix::k_period_flicks}};
    const TileKey anchor = timed_anchor(0);
    const std::array<Content*, 1> participating{content.get()};
    const std::array<TileKey, 1> anchors{anchor};

    TileCache cache(64u * 1024 * 1024);
    const std::vector<TileKey> want =
        drive_playback_prefetch(hint, participating, cache, anchors, step);

    // The decoder pre-rolled exactly the two upcoming frames (1, 2).
    CHECK(content->decodes_issued() == before + 2);

    // The ring is bounded by the horizon: exactly two upcoming buckets, each
    // ahead of the anchor and within the horizon, none reverse.
    CHECK(want.size() == 2);
    for (const TileKey& k : want) {
      REQUIRE(k.achieved_time.has_value());
      CHECK(k.achieved_time->flicks > instant(0).flicks);
      CHECK(k.achieved_time->flicks <= instant(0).flicks + 2 * fix::k_period_flicks);
    }
  }

  SECTION("a paused (empty) hint pre-rolls zero frames and builds an empty ring") {
    auto content = fix::make_content();
    prime_at(*content, backend, 0);
    const std::uint64_t before = content->decodes_issued();

    const PlaybackHint paused{0, Rational{0, 1}, Time{0}};
    const std::array<Content*, 1> participating{content.get()};
    const std::array<TileKey, 1> anchors{timed_anchor(0)};

    TileCache cache(64u * 1024 * 1024);
    const std::vector<TileKey> want =
        drive_playback_prefetch(paused, participating, cache, anchors, step);

    CHECK(content->decodes_issued() == before); // no pre-roll
    CHECK(want.empty());                         // empty ring
  }
}
