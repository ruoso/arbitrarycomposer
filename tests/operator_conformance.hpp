// Reusable L4 pull-routing conformance helper (refinement
// operators.operator_conformance, Acceptance "Reusable L4 helper"). Proves the
// doc-13 rule that an operator obtains every input's pixels and samples ONLY
// through `PullService::pull` / `pull_audio` and never reaches around the core
// via `input->render()` / `render_audio()` (doc 13:69-71).
//
// This is a `tests/`-local header, NOT a public `testing/` API: the property is
// meaningful only against the live L4 `PullServiceImpl` + `CompositorCounters`,
// which `arbc-testing` (on `arbc::contract`, L3) may not link (doc 17:14,
// conformance_suite.md:232-238). So the check lives here, linking the `arbc`
// umbrella, and the identity/damage legs stay in the shipped `arbc-testing`
// families (Decision 1, Decision 5).
//
// The check runs two facets over a poison-leaf input per `inputs()` entry
// (Decision 2), so it generalizes over input arity -- fade (1 input) and a
// future crossfade (2 inputs) reuse it verbatim (Constraint 5):
//   * isolation  -- a `RecordingPull` serves canned tiles/blocks and never
//                   touches the inputs, so any input render is a bypass
//                   (Constraint 3);
//   * integration -- the live `PullServiceImpl` bound to `CompositorCounters`,
//                    where every input render/audio the operator provokes must
//                    equal a dispatch the service issued (Constraint 4).

#ifndef ARBC_TESTS_OPERATOR_CONFORMANCE_HPP
#define ARBC_TESTS_OPERATOR_CONFORMANCE_HPP

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/cache/key_shapes.hpp>
#include <arbc/cache/keyed_store.hpp>
#include <arbc/compositor/counters.hpp>
#include <arbc/compositor/pull_service.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/media/audio_block.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/records.hpp>
#include <arbc/surface/surface.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace arbc::optest {

// A spy leaf standing in for an operator input. Under the isolation facet its
// render()/render_audio() must NEVER be called (a canned-tile service serves the
// operator instead), so a non-zero call count is a direct-render bypass. Under
// the integration facet the live service legitimately dispatches it, and the
// call counts must equal the service's dispatch counters. Both facets read the
// same counters -- isolation expects 0, integration expects the service delta.
class PoisonLeaf final : public Content {
public:
  std::optional<Rect> bounds() const override { return std::nullopt; }
  Stability stability() const override { return Stability::Static; }
  std::optional<TimeRange> time_extent() const override { return std::nullopt; }

  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion> /*done*/) override {
    ++d_render_calls;
    // A leaf under PullServiceImpl may settle inline without drawing: the pull
    // caches its own (cleared) tile. The pull-routing check asserts on counts,
    // not pixels (SolidContent shows a real leaf fills request.target instead).
    return RenderResult{request.scale, true};
  }

  AudioFacet* audio() override { return &d_audio; }

  std::uint64_t render_calls() const noexcept { return d_render_calls; }
  std::uint64_t render_audio_calls() const noexcept { return d_audio.calls(); }

private:
  class PoisonAudio final : public AudioFacet {
  public:
    std::optional<TimeRange> audio_extent() const override { return std::nullopt; }
    Stability audio_stability() const override { return Stability::Static; }
    Time latency() const override { return Time::zero(); }
    std::optional<AudioResult> render_audio(const AudioRequest& request,
                                            std::shared_ptr<AudioCompletion> /*done*/) override {
      ++d_calls;
      const std::size_t n =
          static_cast<std::size_t>(request.target.frames) * channel_count(request.layout);
      if (request.target.samples != nullptr) {
        for (std::size_t i = 0; i < n; ++i) {
          request.target.samples[i] = 0.0F;
        }
      }
      return AudioResult{request.sample_rate, true};
    }
    std::uint64_t calls() const noexcept { return d_calls; }

  private:
    std::uint64_t d_calls{0};
  };

  PoisonAudio d_audio;
  std::uint64_t d_render_calls{0};
};

// The isolation service: records one pull / pull_audio per input and settles the
// completion inline from a canned result, WITHOUT ever calling the input. So the
// operator that pulled through it gets a well-formed answer while its inputs stay
// untouched -- any PoisonLeaf render under this service is an unambiguous bypass.
class RecordingPull final : public PullService {
public:
  void pull(ContentRef input, const RenderRequest& request,
            std::shared_ptr<RenderCompletion> done) override {
    ++d_pulls[input];
    done->complete(RenderResult{request.scale, true, request.time});
  }
  void pull_audio(ContentRef input, const AudioRequest& request,
                  std::shared_ptr<AudioCompletion> done) override {
    ++d_audio_pulls[input];
    done->complete(AudioResult{request.sample_rate, true});
  }

  std::uint64_t pulls_for(ContentRef input) const {
    const auto it = d_pulls.find(input);
    return it != d_pulls.end() ? it->second : 0U;
  }
  std::uint64_t audio_pulls_for(ContentRef input) const {
    const auto it = d_audio_pulls.find(input);
    return it != d_audio_pulls.end() ? it->second : 0U;
  }

private:
  std::unordered_map<ContentRef, std::uint64_t> d_pulls;
  std::unordered_map<ContentRef, std::uint64_t> d_audio_pulls;
};

// Builds the operator under test over the supplied input edges, attached to the
// given service + backend. Operator-agnostic: the helper injects one PoisonLeaf
// per `input_count`, so an operator kind wires its own factory (fade wraps
// inputs[0]; crossfade wraps inputs[0]/[1]) without re-opening the helper.
using OperatorPullFactory = std::function<std::unique_ptr<Content>(
    std::span<const ContentRef> inputs, PullService& pull, Backend& backend)>;

struct PullRoutingCase {
  std::size_t input_count{1};                               // spy leaves to install
  Time time{Time{5 * Time::flicks_per_second}};             // a generic mid-timeline request time
  std::uint32_t sample_rate{48'000};                        // audio-facet request rate
  std::uint32_t audio_frames{512};                          // audio-facet block length
};

// The single rung-0 tile a 256x256 region at scale 1.0 covers (mirrors the
// existing fade counter driver's request geometry; fixed, no ambient now()).
inline RenderRequest visual_request(Surface& target, Time time) {
  return RenderRequest{Rect::from_size(256.0, 256.0), 1.0,           time, StateHandle{},
                       target,                        Exactness::BestEffort, Deadline::none()};
}

inline AudioRequest audio_request(AudioBlock& block, const PullRoutingCase& options) {
  const std::int64_t frame_flicks =
      Time::flicks_per_second / static_cast<std::int64_t>(options.sample_rate);
  const std::int64_t span = static_cast<std::int64_t>(options.audio_frames) * frame_flicks;
  return AudioRequest{TimeRange{options.time, Time{options.time.flicks + span}},
                      options.sample_rate,
                      ChannelLayout::Stereo,
                      block,
                      Exactness::BestEffort,
                      StateHandle{}};
}

// Prove the pull-routing property for one operator kind. Runs the isolation and
// integration facets over both the visual and audio paths (the audio leg runs
// only when the operator exposes an AudioFacet, mirroring the arbc-testing audio
// families). All assertions are behavioral counter deltas -- never wall-clock.
inline void check_operator_pulls_via_service(const OperatorPullFactory& make_operator,
                                             const PullRoutingCase& options = {}) {
  // ---- Isolation facet: canned service, inputs must stay untouched. ----
  {
    CpuBackend backend;
    std::vector<std::unique_ptr<PoisonLeaf>> spies;
    std::vector<ContentRef> refs;
    for (std::size_t i = 0; i < options.input_count; ++i) {
      spies.push_back(std::make_unique<PoisonLeaf>());
      refs.push_back(spies.back().get());
    }
    RecordingPull pull;
    const std::unique_ptr<Content> op = make_operator(refs, pull, backend);
    REQUIRE(op != nullptr);
    // Generalize over inputs(): one declared input edge per installed spy.
    REQUIRE(op->inputs().size() == options.input_count);

    const auto target = backend.make_surface(256, 256, k_working_rgba32f);
    REQUIRE(target.has_value());
    auto done = std::make_shared<RenderCompletion>();
    op->render(visual_request(**target, options.time), done);

    for (const auto& spy : spies) {
      CHECK(pull.pulls_for(spy.get()) >= 1U);   // the operator pulled every input,
      CHECK(spy->render_calls() == 0U);         // and reached none of them directly.
    }

    if (op->audio() != nullptr) {
      std::vector<float> buf(static_cast<std::size_t>(options.audio_frames) * 2, 0.0F);
      AudioBlock block{buf.data(), options.audio_frames, ChannelLayout::Stereo,
                       options.sample_rate};
      auto adone = std::make_shared<AudioCompletion>();
      op->audio()->render_audio(audio_request(block, options), adone);
      for (const auto& spy : spies) {
        CHECK(pull.audio_pulls_for(spy.get()) >= 1U);
        CHECK(spy->render_audio_calls() == 0U);
      }
    }
  }

  // ---- Integration facet: live service, every input render is a dispatch. ----
  {
    CpuBackend backend;
    TileCache cache(64u * 1024 * 1024);
    BlockCache blocks(64u * 1024 * 1024);
    std::vector<std::unique_ptr<PoisonLeaf>> spies;
    std::vector<ContentRef> refs;
    std::unordered_map<const Content*, ObjectId> ids;
    for (std::size_t i = 0; i < options.input_count; ++i) {
      spies.push_back(std::make_unique<PoisonLeaf>());
      refs.push_back(spies.back().get());
      ids.emplace(spies.back().get(), ObjectId{static_cast<std::uint64_t>(i + 1)});
    }

    CompositorCounters counters;
    PullConfig config;
    config.counters = &counters;
    config.id_of = [&ids](const Content* c) {
      const auto it = ids.find(c);
      return it != ids.end() ? it->second : ObjectId{};
    };
    config.contribution = [](const Content*) { return std::uint64_t{1}; };
    config.blocks = &blocks;
    config.audio_dispatch = direct_audio_dispatch();
    PullServiceImpl service(cache, backend, direct_dispatch(), config);

    const std::unique_ptr<Content> op = make_operator(refs, service, backend);
    REQUIRE(op != nullptr);

    const auto target = backend.make_surface(256, 256, k_working_rgba32f);
    REQUIRE(target.has_value());
    auto done = std::make_shared<RenderCompletion>();
    // Drive the operator's render() directly (NOT service.pull(op)): only the
    // input pulls it issues go through the service, so requests_issued counts
    // exactly the input renders the operator provoked.
    op->render(visual_request(**target, options.time), done);

    std::uint64_t observed_renders = 0;
    for (const auto& spy : spies) {
      observed_renders += spy->render_calls();
    }
    // Every input render the operator provoked is one the service dispatched: a
    // direct input->render() would show as a spy render with no matching bump.
    CHECK(observed_renders == counters.requests_issued());

    if (op->audio() != nullptr) {
      std::vector<float> buf(static_cast<std::size_t>(options.audio_frames) * 2, 0.0F);
      AudioBlock block{buf.data(), options.audio_frames, ChannelLayout::Stereo,
                       options.sample_rate};
      auto adone = std::make_shared<AudioCompletion>();
      op->audio()->render_audio(audio_request(block, options), adone);
      std::uint64_t observed_audio = 0;
      for (const auto& spy : spies) {
        observed_audio += spy->render_audio_calls();
      }
      CHECK(observed_audio == counters.audio_dispatches());
    }
  }
}

} // namespace arbc::optest

#endif // ARBC_TESTS_OPERATOR_CONFORMANCE_HPP
