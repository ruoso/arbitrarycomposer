// Contract conformance driver (doc 16, contract.conformance_suite). Runs the
// public `arbc-testing` suite (arbc::contract_tests + the granular family entry
// points) over one purpose-built double per contract branch plus the
// org.arbc.solid reference kind, under a fixed seed. Every property family
// passes here; each `// enforces:` tag pins a claims-register row to the family
// this driver exercises it through.
//
// The seven doc-16 families plus the two operator-member properties are all
// implemented in the library; this file validates them against concrete
// fixtures (Decision 2 -- the reference-kind table grows into these runs as the
// kinds/operators streams land).

#include <arbc/base/geometry.hpp>
#include <arbc/base/time.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/media/audio_block.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/records.hpp>
#include <arbc/surface/surface.hpp>
#include <arbc/testing/contract_tests.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>

namespace {

using namespace arbc;

// The rect the spatial fixtures confine their snapshot dependence to (and the
// input-damage rect the operator over-approximates). Kept inside the 4x4
// default target so at least one interior pixel lands in it.
constexpr Rect k_damage{1.0, 1.0, 3.0, 3.0};

std::span<float> pixels_of(const RenderRequest& req) {
  return req.target.span<PixelFormat::Rgba32fLinearPremul>();
}

// Paint a deterministic, position-varying constant (the `base + i` pattern the
// existing contract doubles use), independent of snapshot and time.
void fill_constant(const RenderRequest& req, float base) {
  const std::span<float> px = pixels_of(req);
  for (std::size_t i = 0; i < px.size(); ++i) {
    px[i] = base + static_cast<float>(i);
  }
}

// Paint so that only pixels whose centers fall inside `d` depend on the
// snapshot; every other pixel is snapshot-invariant. An edit that changes the
// snapshot therefore damages exactly `d`.
void fill_spatial(const RenderRequest& req, const Rect& d) {
  const int w = req.target.width();
  const int h = req.target.height();
  const std::span<float> px = pixels_of(req);
  const float sval = static_cast<float>(req.snapshot.slot & 0xFFFFU); // small, deterministic
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const double cx = static_cast<double>(x) + 0.5;
      const double cy = static_cast<double>(y) + 0.5;
      const bool inside = cx >= d.x0 && cx < d.x1 && cy >= d.y0 && cy < d.y1;
      const std::size_t o = (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                             static_cast<std::size_t>(x)) *
                            4U;
      for (int ch = 0; ch < 4; ++ch) {
        px[o + static_cast<std::size_t>(ch)] =
            1.0F + static_cast<float>(ch) + (inside ? sval : 0.0F);
      }
    }
  }
}

// --- Fixtures, one per contract branch --------------------------------------

// A Timed content: a deterministic function of time that reports achieved_time,
// bounded to a wide extent (covering the suite's generated time range) and
// rendering nothing past that extent.
class TimedFixture final : public Content {
public:
  static constexpr std::int64_t k_extent_end = 4'000'000'000; // > gen_time max

  std::optional<Rect> bounds() const override { return std::nullopt; }
  Stability stability() const override { return Stability::Timed; }
  std::optional<TimeRange> time_extent() const override {
    return TimeRange{Time::zero(), Time{k_extent_end}};
  }
  std::optional<RenderResult> render(const RenderRequest& req,
                                     std::shared_ptr<RenderCompletion>) override {
    const TimeRange extent{Time::zero(), Time{k_extent_end}};
    if (!extent.contains(req.time)) {
      return RenderResult{req.scale, true, std::nullopt}; // outside extent: transparent
    }
    const std::int64_t frame = 24;
    const std::int64_t step = Time::flicks_per_second / frame;
    const std::int64_t quantized = (req.time.flicks / step) * step;
    fill_constant(req, static_cast<float>(quantized));
    RenderResult r{req.scale, true, std::nullopt};
    r.achieved_time = Time{quantized};
    return r;
  }
};

// A Static content with finite bounds that renders nothing when the requested
// region is entirely outside those bounds (bounds honesty).
class BoundedFixture final : public Content {
public:
  std::optional<Rect> bounds() const override { return Rect{0.0, 0.0, 2.0, 2.0}; }
  Stability stability() const override { return Stability::Static; }
  std::optional<TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<RenderResult> render(const RenderRequest& req,
                                     std::shared_ptr<RenderCompletion>) override {
    const Rect b{0.0, 0.0, 2.0, 2.0};
    if (!req.region.intersect(b).empty()) {
      fill_constant(req, 3.0F);
    }
    return RenderResult{req.scale, true, std::nullopt};
  }
};

// A Static content that degrades on BestEffort (reports achieved_scale below
// the request and exact=false) but is faithful on Exact (scale honesty).
class ScaleFixture final : public Content {
public:
  std::optional<Rect> bounds() const override { return std::nullopt; }
  Stability stability() const override { return Stability::Static; }
  std::optional<TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<RenderResult> render(const RenderRequest& req,
                                     std::shared_ptr<RenderCompletion>) override {
    fill_constant(req, 5.0F);
    if (req.exactness == Exactness::Exact) {
      return RenderResult{req.scale, true, std::nullopt};
    }
    return RenderResult{req.scale * 0.5, false, std::nullopt};
  }
};

// A content that answers on the async path (returns nullopt) but settles its
// RenderCompletion immediately, so the suite drives it through take().
class AsyncFixture final : public Content {
public:
  std::optional<Rect> bounds() const override { return std::nullopt; }
  Stability stability() const override { return Stability::Static; }
  std::optional<TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<RenderResult> render(const RenderRequest& req,
                                     std::shared_ptr<RenderCompletion> done) override {
    fill_constant(req, 7.0F);
    done->complete(RenderResult{req.scale, true, std::nullopt});
    return std::nullopt;
  }
};

// A Static content whose pixels are a genuine function of the snapshot handle
// (an "editable" content, at the only contract-level observable of editable
// state): render purity's snapshot-is-a-real-input check and the
// capture/restore round-trip drive this.
class EditableFixture final : public Content {
public:
  std::optional<Rect> bounds() const override { return std::nullopt; }
  Stability stability() const override { return Stability::Static; }
  std::optional<TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<RenderResult> render(const RenderRequest& req,
                                     std::shared_ptr<RenderCompletion>) override {
    const std::span<float> px = pixels_of(req);
    const float sval = static_cast<float>(req.snapshot.slot & 0xFFFFU);
    for (std::size_t i = 0; i < px.size(); ++i) {
      px[i] = 2.0F + static_cast<float>(i) + sval;
    }
    return RenderResult{req.scale, true, std::nullopt};
  }
};

// A Static content whose snapshot dependence is confined to `k_damage` -- the
// damage-soundness fixture, and the operator's input.
class SpatialFixture final : public Content {
public:
  std::optional<Rect> bounds() const override { return std::nullopt; }
  Stability stability() const override { return Stability::Static; }
  std::optional<TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<RenderResult> render(const RenderRequest& req,
                                     std::shared_ptr<RenderCompletion>) override {
    fill_spatial(req, k_damage);
    return RenderResult{req.scale, true, std::nullopt};
  }
};

// A single-input operator. It passes its input through (so output == input,
// making identity faithful trivially observable), reports identity at
// Time::zero(), and over-approximates input damage by a one-pixel inflation.
class PassthroughOperator final : public Content {
public:
  PassthroughOperator() : d_input(std::make_unique<SpatialFixture>()), d_inputs{{d_input.get()}} {}

  std::optional<Rect> bounds() const override { return std::nullopt; }
  Stability stability() const override { return Stability::Static; }
  std::optional<TimeRange> time_extent() const override { return std::nullopt; }

  std::optional<RenderResult> render(const RenderRequest& req,
                                     std::shared_ptr<RenderCompletion>) override {
    auto inner = std::make_shared<RenderCompletion>();
    std::optional<RenderResult> r = d_input->render(req, inner); // passthrough (settles inline)
    return r.has_value() ? r : std::optional<RenderResult>(RenderResult{});
  }

  std::span<const ContentRef> inputs() const override { return d_inputs; }

  std::optional<std::size_t> identity(const RenderRequest& req) const override {
    return req.time == Time::zero() ? std::optional<std::size_t>(0) : std::nullopt;
  }

  Rect map_input_damage(std::size_t /*input*/, const Rect& rect) const override {
    return {rect.x0 - 1.0, rect.y0 - 1.0, rect.x1 + 1.0, rect.y1 + 1.0}; // over-approximate
  }

private:
  std::unique_ptr<Content> d_input;
  std::array<ContentRef, 1> d_inputs;
};

// --- Audio fixtures (contract.audio_conformance) ----------------------------

// An `AudioFacet` whose samples are a deterministic, block-continuous function
// of each output frame's ABSOLUTE media time -- the frame at flick `t` always
// carries the same sample regardless of which window it was rendered in, so a
// window split at a frame boundary produces bit-identical concatenated samples.
// Frames whose time lies outside the declared extent render silence (zero).
// Sample values are integer-derived (no transcendental) so float comparisons
// are byte-exact. Settles inline, or asynchronously via `done->complete` when
// `async`.
class ToneFacet final : public AudioFacet {
public:
  ToneFacet(std::optional<TimeRange> extent, Stability stability, bool async)
      : d_extent(extent), d_stability(stability), d_async(async) {}

  std::optional<TimeRange> audio_extent() const override { return d_extent; }
  Stability audio_stability() const override { return d_stability; }

  std::optional<AudioResult> render_audio(const AudioRequest& req,
                                          std::shared_ptr<AudioCompletion> done) override {
    fill_block(req);
    const AudioResult result{req.sample_rate, true};
    if (d_async) {
      done->complete(result); // settle later (immediately, off the return path)
      return std::nullopt;
    }
    return result; // settle inline
  }

private:
  void fill_block(const AudioRequest& req) const {
    const std::uint32_t ch = channel_count(req.layout);
    const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(req.sample_rate);
    for (std::uint32_t f = 0; f < req.target.frames; ++f) {
      const std::int64_t t = req.window.start.flicks + static_cast<std::int64_t>(f) * fpf;
      const bool present = !d_extent.has_value() || d_extent->contains(Time{t});
      for (std::uint32_t c = 0; c < ch; ++c) {
        req.target.samples[static_cast<std::size_t>(f) * ch + c] = present ? sample_at(t, c) : 0.0F;
      }
    }
  }

  static float sample_at(std::int64_t t, std::uint32_t c) {
    const std::int64_t v = (t / 1000) + static_cast<std::int64_t>(c) * 7;
    return static_cast<float>(v & 0xFFFF) * 0.5F; // small, exactly representable
  }

  std::optional<TimeRange> d_extent;
  Stability d_stability;
  bool d_async;
};

// A `Content` exposing a `ToneFacet` through `audio()` (a tone / clip / synth).
// Its visual side is a benign Static fill so the umbrella's visual families pass
// too; the audio families drive `d_facet`.
class AudioFixture final : public Content {
public:
  AudioFixture(std::optional<TimeRange> extent, Stability audio_stability, bool async)
      : d_facet(extent, audio_stability, async) {}

  std::optional<Rect> bounds() const override { return std::nullopt; }
  Stability stability() const override { return Stability::Static; }
  std::optional<TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<RenderResult> render(const RenderRequest& req,
                                     std::shared_ptr<RenderCompletion>) override {
    fill_constant(req, 9.0F);
    return RenderResult{req.scale, true, std::nullopt};
  }

  AudioFacet* audio() override { return &d_facet; }

private:
  ToneFacet d_facet;
};

testing::ContentFactory solid_factory() {
  return []() -> std::unique_ptr<Content> {
    return std::make_unique<SolidContent>(Rgba{0.5F, 0.25F, 0.125F, 1.0F});
  };
}

} // namespace

// enforces: 03-layer-plugin-interface#static-time-invariant
// enforces: 03-layer-plugin-interface#facet-consistency
TEST_CASE("org.arbc.solid passes the contract conformance suite") {
  arbc::contract_tests(solid_factory());
}

// enforces: 03-layer-plugin-interface#render-time-honest
TEST_CASE("a Timed content passes the contract conformance suite") {
  arbc::contract_tests(
      []() -> std::unique_ptr<arbc::Content> { return std::make_unique<TimedFixture>(); });
}

// enforces: 03-layer-plugin-interface#render-within-declared-bounds
TEST_CASE("a bounds-honoring content renders nothing outside its bounds") {
  arbc::contract_tests(
      []() -> std::unique_ptr<arbc::Content> { return std::make_unique<BoundedFixture>(); });
}

// enforces: 03-layer-plugin-interface#render-scale-honest
TEST_CASE("a resolution-degrading content reports scale honestly") {
  arbc::contract_tests(
      []() -> std::unique_ptr<arbc::Content> { return std::make_unique<ScaleFixture>(); });
}

// enforces: 03-layer-plugin-interface#render-inline-or-async
// enforces: 03-layer-plugin-interface#render-completion-settles-once
TEST_CASE("an async-completing content settles through the one render path") {
  arbc::contract_tests(
      []() -> std::unique_ptr<arbc::Content> { return std::make_unique<AsyncFixture>(); });
}

// enforces: 03-layer-plugin-interface#render-pure-over-pinned-state
// enforces: 03-layer-plugin-interface#capture-restore-roundtrip
TEST_CASE("an editable (snapshot-sensitive) content is pure and round-trips") {
  arbc::testing::Options options;
  options.snapshot_sensitive = true;
  arbc::contract_tests(
      []() -> std::unique_ptr<arbc::Content> { return std::make_unique<EditableFixture>(); },
      options);
}

// enforces: 03-layer-plugin-interface#leaf-content-has-no-operator-graph
TEST_CASE("a leaf content exposes no operator graph") {
  arbc::testing::check_leaf_no_operator_graph(
      []() -> std::unique_ptr<arbc::Content> { return std::make_unique<EditableFixture>(); });
}

// enforces: 03-layer-plugin-interface#undamaged-regions-stable
TEST_CASE("regions disjoint from an edit's damage are bit-identical across it") {
  arbc::testing::OperatorDamageCase edit;
  edit.before = arbc::StateHandle{};
  edit.before.slot = 1;
  edit.after = arbc::StateHandle{};
  edit.after.slot = 2;
  edit.input_damage = k_damage;
  arbc::testing::check_damage_soundness(
      []() -> std::unique_ptr<arbc::Content> { return std::make_unique<SpatialFixture>(); }, edit);
}

// enforces: 03-layer-plugin-interface#operator-damage-covers
// enforces: 03-layer-plugin-interface#operator-identity-faithful
TEST_CASE("a single-input operator covers input damage and is identity-faithful") {
  const auto op_factory = []() -> std::unique_ptr<arbc::Content> {
    return std::make_unique<PassthroughOperator>();
  };

  arbc::testing::OperatorDamageCase edit;
  edit.input = 0;
  edit.before = arbc::StateHandle{};
  edit.before.slot = 1;
  edit.after = arbc::StateHandle{};
  edit.after.slot = 2;
  edit.input_damage = k_damage;
  arbc::testing::check_operator_damage_covers(op_factory, edit);

  arbc::testing::check_operator_identity_faithful(op_factory, arbc::Time::zero());
}

// A Static tone: nullopt audio_extent, deterministic block-continuous samples.
// enforces: 03-layer-plugin-interface#audio-facet-consistent
TEST_CASE("a Static tone's audio facet passes the contract conformance suite") {
  arbc::contract_tests([]() -> std::unique_ptr<arbc::Content> {
    return std::make_unique<AudioFixture>(std::nullopt, arbc::Stability::Static, /*async=*/false);
  });
}

// A Timed audio facet: present extent, a deterministic function of its window,
// silent past the extent, seam-free across a block-boundary split.
// enforces: 03-layer-plugin-interface#audio-facet-consistent
TEST_CASE("a Timed audio facet is a deterministic, block-continuous function of its window") {
  const arbc::TimeRange extent{arbc::Time::zero(), arbc::Time{arbc::Time::flicks_per_second}};
  arbc::contract_tests([extent]() -> std::unique_ptr<arbc::Content> {
    return std::make_unique<AudioFixture>(extent, arbc::Stability::Timed, /*async=*/false);
  });
}

// An async-settling audio facet drains once through the shared AudioCompletion
// under concurrent complete/cancel/take (the re-run of the settle-once
// invariant over generated audio content).
// enforces: 03-layer-plugin-interface#audio-facet-optional
TEST_CASE("an async-settling audio facet settles exactly once through the one audio path") {
  const arbc::TimeRange extent{arbc::Time::zero(), arbc::Time{arbc::Time::flicks_per_second}};
  arbc::contract_tests([extent]() -> std::unique_ptr<arbc::Content> {
    return std::make_unique<AudioFixture>(extent, arbc::Stability::Timed, /*async=*/true);
  });
}

// Visual-only content exposes no audio facet: the umbrella probes audio(),
// finds nullptr, and runs no audio family over it (skip at zero cost).
TEST_CASE("visual-only content exposes no audio facet and skips the audio families") {
  auto probe = solid_factory()();
  REQUIRE(probe->audio() == nullptr);
  arbc::contract_tests(solid_factory());
}
