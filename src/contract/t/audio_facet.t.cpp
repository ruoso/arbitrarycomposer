#include <arbc/base/time.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/media/audio_block.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <random>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

// --- cheapness of the request/result descriptors (constraint 3, doc 12) ---
// `AudioRequest` carries a caller-owned `AudioBlock&` reference member -- like
// `RenderRequest`'s `Surface&` -- yet stays trivially copyable (a reference
// member deletes only the assignment operators, which are not eligible), so the
// request is a cheap by-value descriptor with no allocation or atomic.
static_assert(std::is_trivially_copyable_v<arbc::AudioRequest>,
              "AudioRequest must be a cheap by-value descriptor");
static_assert(std::is_trivially_copyable_v<arbc::AudioResult>,
              "AudioResult must be trivially copyable");
static_assert(std::is_trivially_copyable_v<arbc::ChannelLayout>,
              "ChannelLayout must be trivially copyable");
static_assert(std::is_trivially_copyable_v<arbc::AudioBlock>,
              "AudioBlock must be a cheap by-value view");

// The facet stays an abstract interface; adding the defaulted `pull_audio` does
// not make `PullService` concrete (its `pull` is still pure).
static_assert(std::is_abstract_v<arbc::PullService>, "PullService is an abstract interface");

// A minimal, self-contained visual-only `Content`: it overrides none of the
// audio members, so `audio()` keeps the `nullptr` default (constraint 1). Its
// `render` is a no-op -- this test never renders pixels, only probes facets.
class VisualOnlyContent : public arbc::Content {
public:
  std::optional<arbc::Rect> bounds() const override { return std::nullopt; }
  arbc::Stability stability() const override { return arbc::Stability::Static; }
  std::optional<arbc::TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<arbc::RenderResult> render(const arbc::RenderRequest&,
                                           std::shared_ptr<arbc::RenderCompletion>) override {
    return arbc::RenderResult{};
  }
};

// An `AudioFacet` double whose description methods are configured and whose
// `render_audio` settles either INLINE (returns a value) or ASYNCHRONOUSLY
// (returns nullopt and settles later via `deliver()`), exercising the one code
// path (constraint 4). Set `d_fail` to make the async settlement a failure.
class FacetDouble : public arbc::AudioFacet {
public:
  FacetDouble(std::optional<arbc::TimeRange> extent, arbc::Stability stability, bool async)
      : d_extent(extent), d_stability(stability), d_async(async) {}

  std::optional<arbc::TimeRange> audio_extent() const override { return d_extent; }
  arbc::Stability audio_stability() const override { return d_stability; }

  std::optional<arbc::AudioResult>
  render_audio(const arbc::AudioRequest& request,
               std::shared_ptr<arbc::AudioCompletion> done) override {
    if (!d_async) {
      return arbc::AudioResult{request.sample_rate, true}; // settle inline
    }
    d_done = std::move(done); // settle later
    d_request_rate = request.sample_rate;
    return std::nullopt;
  }

  // Async settlement, driven by the test after `render_audio` returned nullopt.
  void deliver() { d_done->complete(arbc::AudioResult{d_request_rate, true}); }
  void deliver_fail() { d_done->fail(arbc::RenderError::ContentFailed); }

private:
  std::optional<arbc::TimeRange> d_extent;
  arbc::Stability d_stability;
  bool d_async;
  std::shared_ptr<arbc::AudioCompletion> d_done;
  std::uint32_t d_request_rate{0};
};

// A `Content` that owns an `AudioFacet` and exposes it through `audio()` -- a
// video clip / tone / synth. `audio()` returns the same facet on every call
// (identity preserved), and `latency()` is left at the `Time::zero()` default.
class AudioContent : public arbc::Content {
public:
  explicit AudioContent(std::optional<arbc::TimeRange> extent, arbc::Stability stability, bool async)
      : d_facet(extent, stability, async) {}

  std::optional<arbc::Rect> bounds() const override { return std::nullopt; }
  arbc::Stability stability() const override { return arbc::Stability::Static; }
  std::optional<arbc::TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<arbc::RenderResult> render(const arbc::RenderRequest&,
                                           std::shared_ptr<arbc::RenderCompletion>) override {
    return arbc::RenderResult{};
  }

  arbc::AudioFacet* audio() override { return &d_facet; }
  FacetDouble& facet() { return d_facet; }

private:
  FacetDouble d_facet;
};

// A `PullService` that overrides ONLY `pull`, inheriting the defaulted
// `pull_audio` (Decision 5). `pull` is a no-op stub -- the test drives audio.
class PullOnlyService : public arbc::PullService {
public:
  void pull(arbc::ContentRef, const arbc::RenderRequest&,
            std::shared_ptr<arbc::RenderCompletion>) override {}
};

// Drive a facet through the one settle path: render_audio -> (fold an inline
// result through complete, else let the caller settle async) -> take().
std::optional<arbc::expected<arbc::AudioResult, arbc::RenderError>>
drive_audio(FacetDouble& facet, const arbc::AudioRequest& request, bool async) {
  auto done = std::make_shared<arbc::AudioCompletion>();
  const std::optional<arbc::AudioResult> inline_result = facet.render_audio(request, done);
  if (inline_result.has_value()) {
    done->complete(*inline_result); // returned-inline == immediately-completed async
    REQUIRE_FALSE(async);
  } else {
    facet.deliver(); // async content settles off the render call
    REQUIRE(async);
  }
  return done->take();
}

} // namespace

// enforces: 03-layer-plugin-interface#audio-facet-optional
TEST_CASE("audio() is a null-default discovery hook; an exposed facet answers by identity") {
  // Default path: visual-only content is audio-less -- the observable invariant
  // that a purely visual kind costs the audio engine nothing (doc 12:73-77).
  VisualOnlyContent visual;
  REQUIRE(static_cast<arbc::Content&>(visual).audio() == nullptr);

  // Override path -- discovery by pointer identity across repeated calls.
  const arbc::TimeRange extent{arbc::Time::zero(), arbc::Time{100}};
  AudioContent clip(extent, arbc::Stability::Timed, /*async=*/false);
  arbc::AudioFacet* facet = static_cast<arbc::Content&>(clip).audio();
  REQUIRE(facet != nullptr);
  REQUIRE(static_cast<arbc::Content&>(clip).audio() == facet); // identity preserved

  // Override path -- description methods answer as configured, latency defaults.
  REQUIRE(facet->audio_extent() == extent);
  REQUIRE(facet->audio_stability() == arbc::Stability::Timed);
  REQUIRE(facet->latency() == arbc::Time::zero());

  // render_audio settles exactly once through the shared completion.
  arbc::AudioBlock block{};
  const arbc::AudioRequest request{extent, 48000U, arbc::ChannelLayout::Stereo, block};
  const std::optional<arbc::expected<arbc::AudioResult, arbc::RenderError>> settled =
      drive_audio(clip.facet(), request, /*async=*/false);
  REQUIRE(settled.has_value());
  REQUIRE(settled->has_value());
  REQUIRE((**settled).achieved_rate == 48000U);
}

TEST_CASE("a Static tone reports nullopt audio_extent; ChannelLayout channel counts") {
  AudioContent tone(std::nullopt, arbc::Stability::Static, /*async=*/false);
  arbc::AudioFacet* facet = static_cast<arbc::Content&>(tone).audio();
  REQUIRE(facet->audio_extent() == std::nullopt); // Static: time-invariant
  REQUIRE(facet->audio_stability() == arbc::Stability::Static);

  REQUIRE(arbc::channel_count(arbc::ChannelLayout::Mono) == 1U);
  REQUIRE(arbc::channel_count(arbc::ChannelLayout::Stereo) == 2U);
}

TEST_CASE("render_audio one code path: inline and async settle equivalently; fail surfaces") {
  const arbc::TimeRange extent{arbc::Time::zero(), arbc::Time{100}};
  arbc::AudioBlock block{};
  const arbc::AudioRequest request{extent, 44100U, arbc::ChannelLayout::Stereo, block};

  // Synchronous facet: settles inline. Asynchronous facet: returns nullopt then
  // settles later. Both flow through render -> settle -> take() equivalently.
  AudioContent sync_clip(extent, arbc::Stability::Timed, /*async=*/false);
  AudioContent async_clip(extent, arbc::Stability::Timed, /*async=*/true);
  const auto sync_settled = drive_audio(sync_clip.facet(), request, /*async=*/false);
  const auto async_settled = drive_audio(async_clip.facet(), request, /*async=*/true);

  REQUIRE(sync_settled.has_value());
  REQUIRE(async_settled.has_value());
  REQUIRE(sync_settled->has_value());
  REQUIRE(async_settled->has_value());
  REQUIRE((**sync_settled).achieved_rate == (**async_settled).achieved_rate);
  REQUIRE((**sync_settled).exact == (**async_settled).exact);

  // A failed async settlement surfaces as the expected unexpected.
  AudioContent fail_clip(extent, arbc::Stability::Timed, /*async=*/true);
  auto done = std::make_shared<arbc::AudioCompletion>();
  REQUIRE_FALSE(fail_clip.facet().render_audio(request, done).has_value());
  fail_clip.facet().deliver_fail();
  const std::optional<arbc::expected<arbc::AudioResult, arbc::RenderError>> settled = done->take();
  REQUIRE(settled.has_value());
  REQUIRE_FALSE(settled->has_value());
  REQUIRE(settled->error() == arbc::RenderError::ContentFailed);
}

TEST_CASE("PullService::pull_audio default settles unexpected(ResourceUnavailable) exactly once") {
  PullOnlyService service;
  arbc::AudioBlock block{};
  const arbc::AudioRequest request{arbc::TimeRange{}, 48000U, arbc::ChannelLayout::Stereo, block};

  auto done = std::make_shared<arbc::AudioCompletion>();
  static_cast<arbc::PullService&>(service).pull_audio(nullptr, request, done);

  const std::optional<arbc::expected<arbc::AudioResult, arbc::RenderError>> settled = done->take();
  REQUIRE(settled.has_value());
  REQUIRE_FALSE(settled->has_value());
  REQUIRE(settled->error() == arbc::RenderError::ResourceUnavailable);
  REQUIRE_FALSE(done->take().has_value()); // settled exactly once, never hangs
}

TEST_CASE("AudioCompletion settle/take race: exactly one settlement, no torn payload") {
  // TSan/stress case (doc 16, doc 12 concurrency scope): the audio twin of
  // async_render's completion stress test, over the NEW AudioResult
  // instantiation of the shared Completion<Result> template. A renderer thread
  // settles while a consumer races cancel()/cancelled()/take(); asserts
  // exactly-one settlement and an intact payload (achieved_rate never torn).
  for (unsigned seed = 0; seed < 256U; ++seed) {
    auto c = std::make_shared<arbc::AudioCompletion>();
    const std::uint32_t expected_rate = 44100U + seed;

    std::thread renderer([&c, seed, expected_rate] {
      std::mt19937 rng(seed);
      if ((rng() & 1U) != 0U) {
        std::this_thread::yield();
      }
      c->complete(arbc::AudioResult{expected_rate, false});
    });

    std::mt19937 rng(seed ^ 0x9e3779b9U);
    std::optional<arbc::expected<arbc::AudioResult, arbc::RenderError>> taken;
    for (int spin = 0; spin < 4096 && !taken.has_value(); ++spin) {
      if ((rng() & 1U) != 0U) {
        c->cancel();
      }
      (void)c->cancelled();
      taken = c->take();
      if ((rng() & 2U) != 0U) {
        std::this_thread::yield();
      }
    }

    renderer.join();
    if (!taken.has_value()) {
      taken = c->take(); // drain the settlement that landed after the loop
    }

    REQUIRE(taken.has_value());
    REQUIRE(taken->has_value());
    REQUIRE((**taken).achieved_rate == expected_rate); // no torn payload
    REQUIRE_FALSE((**taken).exact);
    REQUIRE_FALSE(c->take().has_value()); // settled exactly once
  }
}
