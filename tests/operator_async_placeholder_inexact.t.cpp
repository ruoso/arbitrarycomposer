// kinds.operator_async_placeholder_inexact: the transient-placeholder rule of the
// operator contract (doc 13:122-144), driven per operator kind and per facet.
//
// An operator whose input pull comes back UNSETTLED has been told nothing except
// "the service dispatched this render to a worker". It cannot wait -- its descent is
// synchronous on the frame thread -- so it renders a placeholder: a transparent tile,
// or a silent block. That placeholder is TRANSIENT. The real answer lands in the cache
// and a later pass must compose it, which happens only if THIS pass reports itself
// inexact; an exact-flagged placeholder is cached as a final answer and served straight
// back to the `Exactness::Exact` pass that was supposed to re-request the input, so the
// deferred input is frozen out of the frame -- or out of the mix -- forever.
//
// The other placeholder is FINAL: the pull FAILED or exceeded its budget, nothing more
// is coming for this revision, and the silence/transparency IS the honest answer. It is
// reported exact. Getting that half wrong would be a different bug (an operator that
// never converges), so both halves are asserted here, per kind, per facet.
//
// The visual half of this rule was fixed in runtime.worker_dispatch_leaf_only; these
// cases pin it at the unit level. The AUDIO half is what this task fixes: fade,
// crossfade and nested all reported their silent placeholder exact.
//
// Cross-component (real CpuBackend + kind_solid/kind_tone leaves under all three
// operator kinds), so per doc 17 levelization it lives in tests/ and links the
// umbrella `arbc`.

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/compositor/pull_service.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_crossfade/crossfade_content.hpp>
#include <arbc/kind_fade/fade_content.hpp>
#include <arbc/kind_nested/nested_content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/kind_tone/tone_content.hpp>
#include <arbc/media/audio_block.hpp>
#include <arbc/media/audio_format.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/model.hpp>
#include <arbc/surface/surface.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

using namespace arbc;

namespace {

constexpr std::uint32_t k_rate = 48'000;
constexpr std::uint32_t k_frames = 16;

std::int64_t flicks_per_frame() {
  return Time::flicks_per_second / static_cast<std::int64_t>(k_rate);
}

TimeRange block_window() {
  return TimeRange{Time::zero(), Time{static_cast<std::int64_t>(k_frames) * flicks_per_frame()}};
}

// A PullService double that scripts, per input content, HOW that input's pull answers
// -- the three outcomes the operator contract distinguishes:
//
//   Inline -- settle the input's facet inline (the resident-cache-hit path).
//   Defer  -- return leaving the completion UNSETTLED. Precisely what the worker-backed
//             service does when it dispatches a cold render to a worker, and the case
//             under test: the operator must render a TRANSIENT placeholder, report it
//             inexact, and cancel the completion it will not drain.
//   Fail   -- settle with an error (budget exceeded / unavailable): a FINAL placeholder,
//             which the operator reports EXACT.
//
// `degrade` settles a pull inline but overrides the input's own `AudioResult`, so an
// operator's fold of its inputs' honesty can be driven independently of the transient
// bits.
class ScriptedPull final : public PullService {
public:
  enum class Mode { Inline, Defer, Fail };

  void script(const Content* input, Mode mode) { d_modes[input] = mode; }
  void degrade(const Content* input, AudioResult result) { d_degraded[input] = result; }
  unsigned pulls() const { return d_pulls; }
  unsigned audio_pulls() const { return d_audio_pulls; }

  void pull(ContentRef input, const RenderRequest& request,
            std::shared_ptr<RenderCompletion> done) override {
    ++d_pulls;
    const Mode mode = mode_of(input);
    if (mode == Mode::Defer) {
      return; // dispatched to a worker: `done` stays unsettled
    }
    if (mode == Mode::Fail || input == nullptr) {
      done->fail(RenderError::ResourceUnavailable);
      return;
    }
    const std::optional<RenderResult> r = input->render(request, done);
    if (r.has_value()) {
      done->complete(*r);
    } else if (!done->settled()) {
      done->fail(RenderError::ContentFailed);
    }
  }

  void pull_audio(ContentRef input, const AudioRequest& request,
                  std::shared_ptr<AudioCompletion> done) override {
    ++d_audio_pulls;
    const Mode mode = mode_of(input);
    if (mode == Mode::Defer) {
      return; // dispatched to a worker: `done` stays unsettled
    }
    AudioFacet* af = input != nullptr ? input->audio() : nullptr;
    if (mode == Mode::Fail || af == nullptr) {
      done->fail(RenderError::ResourceUnavailable);
      return;
    }
    const std::optional<AudioResult> r = af->render_audio(request, done);
    if (!r.has_value()) {
      if (!done->settled()) {
        done->fail(RenderError::ContentFailed);
      }
      return;
    }
    const auto it = d_degraded.find(input);
    done->complete(it != d_degraded.end() ? it->second : *r);
  }

private:
  Mode mode_of(const Content* input) const {
    const auto it = d_modes.find(input);
    return it != d_modes.end() ? it->second : Mode::Inline;
  }

  std::unordered_map<const Content*, Mode> d_modes;
  std::unordered_map<const Content*, AudioResult> d_degraded;
  unsigned d_pulls{0};
  unsigned d_audio_pulls{0};
};

// --- render helpers -----------------------------------------------------------

struct AudioOut {
  AudioResult meta;
  std::vector<float> samples;
};

AudioOut render_audio_of(Content& content) {
  AudioFacet* af = content.audio();
  REQUIRE(af != nullptr);
  std::vector<float> samples(static_cast<std::size_t>(k_frames) * 2U, 0.0F);
  AudioBlock block{samples.data(), k_frames, ChannelLayout::Stereo, k_rate};
  const AudioRequest req{block_window(), k_rate,           ChannelLayout::Stereo,
                         block,          Exactness::Exact, StateHandle{}};
  auto done = std::make_shared<AudioCompletion>();
  const std::optional<AudioResult> r = af->render_audio(req, done);
  REQUIRE(r.has_value());
  return AudioOut{*r, std::move(samples)};
}

bool silent(const std::vector<float>& samples) {
  return std::all_of(samples.begin(), samples.end(), [](float s) { return s == 0.0F; });
}

struct VisualOut {
  RenderResult meta;
  std::vector<std::byte> bytes;
};

VisualOut render_visual_of(Content& content, CpuBackend& backend, Time time) {
  const auto target = backend.make_surface(2, 2, k_working_rgba32f);
  REQUIRE(target.has_value());
  const RenderRequest req{
      Rect::from_size(2.0, 2.0), 1.0, time, StateHandle{}, **target, Exactness::Exact,
      Deadline::none()};
  auto done = std::make_shared<RenderCompletion>();
  const std::optional<RenderResult> r = content.render(req, done);
  REQUIRE(r.has_value());
  const std::span<const std::byte> bytes = (**target).cpu_bytes();
  return VisualOut{*r, std::vector<std::byte>(bytes.begin(), bytes.end())};
}

// Premultiplied RGBA32F transparent black is all-zero bytes.
bool transparent(const std::vector<std::byte>& bytes) {
  return std::all_of(bytes.begin(), bytes.end(), [](std::byte b) { return b == std::byte{0}; });
}

// A fade whose envelope ramps across the block/tile under test, so the partial-envelope
// branch (a temp surface + composite, and the per-frame audio gain) is the one exercised.
FadeParams ramp_params() {
  return FadeParams{
      FadeShape::Linear,
      FadeWindow{Time{0}, Time{static_cast<std::int64_t>(k_frames) * flicks_per_frame()}},
      std::nullopt};
}

// A crossfade whose window makes the block/tile under test sit strictly INSIDE the
// dissolve (w == 0.5 at the tile's time), so both inputs are genuinely mixed rather
// than served through the endpoint identity.
CrossfadeParams dissolve_params() {
  return CrossfadeParams{CrossfadeShape::Linear, Time{0},
                         Time{2 * static_cast<std::int64_t>(k_frames) * flicks_per_frame()}};
}

Time interior_time() { return Time{static_cast<std::int64_t>(k_frames) * flicks_per_frame()}; }

NestedResolver map_resolver(std::unordered_map<ObjectId, Content*>& binding) {
  return [&binding](ObjectId id) -> Content* {
    const auto it = binding.find(id);
    return it != binding.end() ? it->second : nullptr;
  };
}

} // namespace

// --- fade ---------------------------------------------------------------------

// enforces: 13-effects-as-operators#transient-placeholder-is-never-exact
TEST_CASE("org.arbc.fade reports its async-placeholder audio block INEXACT") {
  CpuBackend backend;
  ScriptedPull pull;
  ToneContent tone{440, 0.5F};
  FadeContent fade{&tone, ramp_params()};
  fade.attach(pull, backend);

  SECTION("a settled pull: the real samples, reported exact") {
    const AudioOut out = render_audio_of(fade);
    CHECK(out.meta.exact);
    CHECK(out.meta.achieved_rate == k_rate);
    CHECK_FALSE(silent(out.samples)); // the ramp is open over most of the block
  }

  SECTION("a DEFERRED pull: a silent TRANSIENT placeholder, reported inexact") {
    pull.script(&tone, ScriptedPull::Mode::Defer);
    const AudioOut out = render_audio_of(fade);
    CHECK(silent(out.samples));
    // The bug this task fixes: `true` here caches the silence as a final answer and the
    // tone's dispatched render, once it lands, is never composed -- a cold-cache
    // parallel audio render exports silence.
    CHECK_FALSE(out.meta.exact);
    CHECK(pull.audio_pulls() == 1);
  }

  SECTION("a FAILED pull: a silent FINAL placeholder, reported exact") {
    pull.script(&tone, ScriptedPull::Mode::Fail);
    const AudioOut out = render_audio_of(fade);
    CHECK(silent(out.samples));
    CHECK(out.meta.exact); // nothing more is coming for this revision
  }
}

// enforces: 13-effects-as-operators#transient-placeholder-is-never-exact
TEST_CASE("org.arbc.fade reports its async-placeholder tile INEXACT on both envelope "
          "branches") {
  CpuBackend backend;
  ScriptedPull pull;
  SolidContent solid{Rgba{0.5F, 0.25F, 0.125F, 1.0F}, Rect{0.0, 0.0, 2.0, 2.0}};
  FadeContent fade{&solid, ramp_params()};
  fade.attach(pull, backend);

  // Half-open (the temp-surface + composite branch) and fully-open (the pass-through
  // branch) pull the input through DIFFERENT code paths; both must degrade inexactly.
  const Time half = Time{static_cast<std::int64_t>(k_frames) * flicks_per_frame() / 2};
  const Time open = Time{2 * static_cast<std::int64_t>(k_frames) * flicks_per_frame()};

  SECTION("a DEFERRED pull: a transparent TRANSIENT placeholder, reported inexact") {
    pull.script(&solid, ScriptedPull::Mode::Defer);
    const VisualOut partial = render_visual_of(fade, backend, half);
    CHECK(transparent(partial.bytes));
    CHECK_FALSE(partial.meta.exact);

    const VisualOut full = render_visual_of(fade, backend, open);
    CHECK(transparent(full.bytes));
    CHECK_FALSE(full.meta.exact);
  }

  SECTION("a FAILED pull: a transparent FINAL placeholder, reported exact") {
    pull.script(&solid, ScriptedPull::Mode::Fail);
    const VisualOut partial = render_visual_of(fade, backend, half);
    CHECK(transparent(partial.bytes));
    CHECK(partial.meta.exact);
  }
}

// --- crossfade ----------------------------------------------------------------

// enforces: 13-effects-as-operators#transient-placeholder-is-never-exact
TEST_CASE("org.arbc.crossfade reports its audio mix INEXACT when EITHER input pull "
          "defers") {
  CpuBackend backend;
  ToneContent from{440, 0.5F};
  ToneContent to{660, 0.25F};

  SECTION("both settled: the real mix, reported exact") {
    ScriptedPull pull;
    CrossfadeContent xf{&from, &to, dissolve_params()};
    xf.attach(pull, backend);
    const AudioOut out = render_audio_of(xf);
    CHECK(out.meta.exact);
    CHECK(out.meta.achieved_rate == k_rate);
    CHECK_FALSE(silent(out.samples));
  }

  SECTION("input 0 deferred: inexact, and input 1 is STILL pulled") {
    ScriptedPull pull;
    CrossfadeContent xf{&from, &to, dissolve_params()};
    xf.attach(pull, backend);
    pull.script(&from, ScriptedPull::Mode::Defer);
    const AudioOut out = render_audio_of(xf);
    CHECK_FALSE(out.meta.exact);
    // A pull is what DISPATCHES a cold input's render: short-circuiting on input 0's
    // placeholder would leave input 1 undispatched, and a driver that reaps once would
    // re-render this crossfade against a still-cold input 1 forever.
    CHECK(pull.audio_pulls() == 2);
  }

  SECTION("input 1 deferred: inexact") {
    ScriptedPull pull;
    CrossfadeContent xf{&from, &to, dissolve_params()};
    xf.attach(pull, backend);
    pull.script(&to, ScriptedPull::Mode::Defer);
    const AudioOut out = render_audio_of(xf);
    CHECK_FALSE(out.meta.exact);
    CHECK(pull.audio_pulls() == 2);
  }

  SECTION("both failed: a silent FINAL placeholder, reported exact") {
    ScriptedPull pull;
    CrossfadeContent xf{&from, &to, dissolve_params()};
    xf.attach(pull, backend);
    pull.script(&from, ScriptedPull::Mode::Fail);
    pull.script(&to, ScriptedPull::Mode::Fail);
    const AudioOut out = render_audio_of(xf);
    CHECK(silent(out.samples));
    CHECK(out.meta.exact);
  }
}

// enforces: 13-effects-as-operators#transient-placeholder-is-never-exact
TEST_CASE("org.arbc.crossfade folds its inputs' own audio honesty into the mix") {
  CpuBackend backend;
  ScriptedPull pull;
  ToneContent from{440, 0.5F};
  ToneContent to{660, 0.25F};
  CrossfadeContent xf{&from, &to, dissolve_params()};
  xf.attach(pull, backend);

  // Input 1 settles INLINE -- no transient bit anywhere -- but answers below the
  // requested rate and says so. The mix adds only a per-frame weight, so it invents no
  // temporal resolution its input did not deliver: it carries the lower rate through
  // and stays inexact. (Before this task the settled input's result was taken and
  // discarded, and the mix reported exact at the request rate regardless.)
  pull.degrade(&to, AudioResult{k_rate / 2, false});
  const AudioOut out = render_audio_of(xf);
  CHECK_FALSE(out.meta.exact);
  CHECK(out.meta.achieved_rate == k_rate / 2);
  CHECK_FALSE(silent(out.samples));
}

// enforces: 13-effects-as-operators#transient-placeholder-is-never-exact
TEST_CASE("org.arbc.crossfade reports its async-placeholder tile INEXACT") {
  CpuBackend backend;
  ScriptedPull pull;
  SolidContent from{Rgba{0.5F, 0.25F, 0.125F, 1.0F}, Rect{0.0, 0.0, 2.0, 2.0}};
  SolidContent to{Rgba{0.125F, 0.375F, 0.75F, 1.0F}, Rect{0.0, 0.0, 2.0, 2.0}};
  CrossfadeContent xf{&from, &to, dissolve_params()};
  xf.attach(pull, backend);

  SECTION("a DEFERRED interior pull: inexact") {
    pull.script(&to, ScriptedPull::Mode::Defer);
    const VisualOut out = render_visual_of(xf, backend, interior_time());
    CHECK_FALSE(out.meta.exact);
    CHECK(pull.pulls() == 2); // both inputs pulled even though one deferred
  }

  SECTION("a FAILED interior pull: exact (input 0 alone is the honest answer)") {
    pull.script(&to, ScriptedPull::Mode::Fail);
    const VisualOut out = render_visual_of(xf, backend, interior_time());
    CHECK(out.meta.exact);
  }
}

// --- nested -------------------------------------------------------------------

// enforces: 13-effects-as-operators#transient-placeholder-is-never-exact
TEST_CASE("org.arbc.nested reports its composed audio block INEXACT when a child pull "
          "defers") {
  Model model;
  ToneContent tone_a{440, 0.5F};
  ToneContent tone_b{660, 0.25F};
  std::unordered_map<ObjectId, Content*> binding;
  ObjectId comp{};
  {
    auto tx = model.transact("two tones");
    comp = tx.add_composition(0.0, 0.0);
    const ObjectId ca = tx.add_content(1);
    const ObjectId cb = tx.add_content(1);
    tx.attach_layer(comp, tx.add_layer(ca, Affine::identity()));
    tx.attach_layer(comp, tx.add_layer(cb, Affine::identity()));
    tx.commit();
    binding[ca] = &tone_a;
    binding[cb] = &tone_b;
  }
  const DocStatePtr doc = model.current();

  CpuBackend backend;
  ScriptedPull pull;
  NestedContent nested{comp};
  nested.attach(pull, backend, map_resolver(binding), *doc);

  SECTION("both children settled: the real mix, reported exact") {
    const AudioOut out = render_audio_of(nested);
    CHECK(out.meta.exact);
    CHECK_FALSE(silent(out.samples));
  }

  SECTION("one child DEFERRED: the layer mixes silence and the block is inexact") {
    pull.script(&tone_b, ScriptedPull::Mode::Defer);
    const AudioOut out = render_audio_of(nested);
    // The other child still contributes -- the deferral silences ONE layer, not the mix.
    CHECK_FALSE(silent(out.samples));
    // The nested VISUAL descent already reported this correctly; the audio descent
    // returned early without touching `exact`, so the half-composed mix cached as final.
    CHECK_FALSE(out.meta.exact);
  }

  SECTION("one child FAILED: silence for that layer, and the block stays exact") {
    pull.script(&tone_b, ScriptedPull::Mode::Fail);
    const AudioOut out = render_audio_of(nested);
    CHECK(out.meta.exact);
  }
}

// enforces: 13-effects-as-operators#transient-placeholder-is-never-exact
TEST_CASE("org.arbc.nested reports its composed tile INEXACT when a child pull defers") {
  Model model;
  SolidContent solid{Rgba{0.5F, 0.25F, 0.125F, 1.0F}, Rect{0.0, 0.0, 2.0, 2.0}};
  std::unordered_map<ObjectId, Content*> binding;
  ObjectId comp{};
  {
    auto tx = model.transact("one solid");
    comp = tx.add_composition(0.0, 0.0);
    const ObjectId cs = tx.add_content(1);
    tx.attach_layer(comp, tx.add_layer(cs, Affine::identity()));
    tx.commit();
    binding[cs] = &solid;
  }
  const DocStatePtr doc = model.current();

  CpuBackend backend;
  ScriptedPull pull;
  NestedContent nested{comp};
  nested.attach(pull, backend, map_resolver(binding), *doc);

  SECTION("a DEFERRED child pull: a transparent TRANSIENT placeholder, reported inexact") {
    pull.script(&solid, ScriptedPull::Mode::Defer);
    const VisualOut out = render_visual_of(nested, backend, Time::zero());
    CHECK(transparent(out.bytes));
    CHECK_FALSE(out.meta.exact);
  }

  SECTION("a FAILED child pull: a transparent FINAL placeholder, reported exact") {
    pull.script(&solid, ScriptedPull::Mode::Fail);
    const VisualOut out = render_visual_of(nested, backend, Time::zero());
    CHECK(transparent(out.bytes));
    CHECK(out.meta.exact);
  }
}
