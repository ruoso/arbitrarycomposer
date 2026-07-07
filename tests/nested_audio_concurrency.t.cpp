#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_nested/nested_content.hpp>
#include <arbc/kind_tone/tone_content.hpp>
#include <arbc/media/audio_block.hpp>
#include <arbc/model/model.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

// Concurrency / TSan stress for org.arbc.nested's AUDIO facet (doc 16 tier-6,
// tsan lane, refinement Acceptance "Async / concurrency"). One nested audio scene
// is rendered many times CONCURRENTLY from independent threads, each into its own
// caller-owned block, through a STATELESS multi-thread PullService audio double.
// Nested's per-layer descent runs on the calling (worker) thread and only leaf
// audio pulls dispatch further; it reads only immutable-after-attach services and
// the pinned snapshot, so `render_thread_safe()` stays true. Asserts no data race
// (under TSan) and deterministic samples -- every concurrent render equals the
// single-threaded reference.

namespace {

using namespace arbc;

// Stateless inline honoring of the audio pull contract: routes `pull_audio` to
// the input's AudioFacet and settles inline. No mutable state, so it is safe under
// concurrent descent from many threads (the acyclic tone scene needs no depth
// backstop).
class InlineAudioPull final : public PullService {
public:
  void pull(ContentRef, const RenderRequest&, std::shared_ptr<RenderCompletion> done) override {
    done->fail(RenderError::ContentFailed);
  }
  void pull_audio(ContentRef input, const AudioRequest& request,
                  std::shared_ptr<AudioCompletion> done) override {
    AudioFacet* af = input != nullptr ? input->audio() : nullptr;
    if (af == nullptr) {
      done->fail(RenderError::ResourceUnavailable);
      return;
    }
    const std::optional<AudioResult> r = af->render_audio(request, done);
    if (r.has_value()) {
      done->complete(*r);
    } else if (!done->settled()) {
      done->fail(RenderError::ContentFailed);
    }
  }
};

// A stateless below-rate audio source (kinds.nested_audio_resampling): asked above
// its native rate it reports achieved_rate == native, exact == false, driving
// nested's windowed-sinc reconstruction. A byte-exact parabolic sine over an exact
// integer flick phase (never std::sin), so the samples are deterministic and the
// facet holds no mutable state -- safe under concurrent descent.
class BelowRateSource final : public Content {
public:
  explicit BelowRateSource(std::uint32_t native_rate) : d_native_rate(native_rate), d_facet(this) {}
  std::optional<Rect> bounds() const override { return std::nullopt; }
  Stability stability() const override { return Stability::Static; }
  std::optional<TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<RenderResult> render(const RenderRequest&,
                                     std::shared_ptr<RenderCompletion> done) override {
    done->fail(RenderError::ContentFailed);
    return std::nullopt;
  }
  AudioFacet* audio() override { return &d_facet; }

private:
  class Facet final : public AudioFacet {
  public:
    explicit Facet(BelowRateSource* owner) : d_owner(owner) {}
    std::optional<TimeRange> audio_extent() const override { return std::nullopt; }
    Stability audio_stability() const override { return Stability::Static; }
    std::optional<AudioResult> render_audio(const AudioRequest& request,
                                            std::shared_ptr<AudioCompletion>) override {
      const std::uint32_t ch = channel_count(request.layout);
      const std::int64_t fps = Time::flicks_per_second;
      const std::int64_t fpf = fps / static_cast<std::int64_t>(request.sample_rate);
      for (std::uint32_t f = 0; f < request.target.frames; ++f) {
        std::int64_t t = (request.window.start.flicks + static_cast<std::int64_t>(f) * fpf) % fps;
        if (t < 0) {
          t += fps;
        }
        const std::int64_t r = (static_cast<std::int64_t>(3000) * t) % fps;
        double p = 2.0 * (static_cast<double>(r) / static_cast<double>(fps));
        if (p > 1.0) {
          p -= 2.0;
        }
        const double abs_p = p < 0.0 ? -p : p;
        const float v = static_cast<float>(0.6 * (4.0 * p * (1.0 - abs_p)));
        for (std::uint32_t c = 0; c < ch; ++c) {
          request.target.samples[static_cast<std::size_t>(f) * ch + c] = v;
        }
      }
      const std::uint32_t achieved = std::min(request.sample_rate, d_owner->d_native_rate);
      return AudioResult{achieved, request.sample_rate <= d_owner->d_native_rate};
    }

  private:
    BelowRateSource* d_owner;
  };

  std::uint32_t d_native_rate;
  Facet d_facet;
};

bool bytes_equal(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

// No Catch2 macros: this runs on worker threads, where Catch2 assertions are not
// safe. The nested audio facet always settles inline, so the block is filled on
// return; correctness is asserted on the main thread via `bytes_equal`.
std::vector<float> render_audio(NestedContent& nested, const TimeRange& window, std::uint32_t rate,
                                std::uint32_t frames) {
  std::vector<float> buf(static_cast<std::size_t>(frames) * 2, 0.0F);
  AudioBlock block{buf.data(), frames, ChannelLayout::Stereo, rate};
  const AudioRequest req{window,           rate,         ChannelLayout::Stereo, block,
                         Exactness::Exact, StateHandle{}};
  auto done = std::make_shared<AudioCompletion>();
  (void)nested.audio()->render_audio(req, done);
  return buf;
}

} // namespace

TEST_CASE("nested audio renders race-free and deterministically across threads") {
  Model model;
  ToneContent tone_a(440, 0.5F);
  ToneContent tone_b(660, 0.25F);
  std::unordered_map<ObjectId, Content*> binding;
  ObjectId comp{};
  {
    auto tx = model.transact("scene");
    comp = tx.add_composition(0.0, 0.0);
    const ObjectId ca = tx.add_content(1);
    const ObjectId cb = tx.add_content(1);
    const ObjectId la = tx.add_layer(ca, Affine::identity());
    const ObjectId lb = tx.add_layer(cb, Affine::identity());
    tx.attach_layer(comp, la);
    tx.attach_layer(comp, lb);
    tx.commit();
    binding[ca] = &tone_a;
    binding[cb] = &tone_b;
  }
  const DocStatePtr doc = model.current();

  CpuBackend backend;
  InlineAudioPull pull;
  NestedContent nested(comp);
  nested.attach(
      pull, backend,
      [&binding](ObjectId id) -> Content* {
        const auto it = binding.find(id);
        return it != binding.end() ? it->second : nullptr;
      },
      *doc);

  const std::uint32_t rate = 48'000;
  const std::uint32_t frames = 32;
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(rate);
  const TimeRange window{Time::zero(), Time{static_cast<std::int64_t>(frames) * fpf}};

  const std::vector<float> ref = render_audio(nested, window, rate, frames);

  constexpr int k_threads = 8;
  std::vector<std::vector<float>> results(k_threads);
  std::vector<std::thread> threads;
  std::atomic<bool> go{false};
  threads.reserve(k_threads);
  for (int i = 0; i < k_threads; ++i) {
    threads.emplace_back([&, i] {
      while (!go.load(std::memory_order_acquire)) {
      }
      for (int rep = 0; rep < 16; ++rep) {
        results[static_cast<std::size_t>(i)] = render_audio(nested, window, rate, frames);
      }
    });
  }
  go.store(true, std::memory_order_release);
  for (std::thread& t : threads) {
    t.join();
  }

  // Every concurrent render produced byte-identical samples to the single-threaded
  // reference: no data race corrupted a shared read, and the descent is
  // deterministic.
  for (const std::vector<float>& r : results) {
    REQUIRE(bytes_equal(r, ref));
  }
}

TEST_CASE("nested below-rate audio reconstruction is race-free and deterministic across threads") {
  // The resampler adds a second (native-rate) pull and runs a pure windowed-sinc
  // kernel over caller-owned local buffers -- no shared state -- so a below-rate
  // scene rendered concurrently stays race-free (under TSan) and deterministic.
  Model model;
  BelowRateSource src(24'000);
  std::unordered_map<ObjectId, Content*> binding;
  ObjectId comp{};
  {
    auto tx = model.transact("below-rate scene");
    comp = tx.add_composition(0.0, 0.0);
    const ObjectId cs = tx.add_content(1);
    const ObjectId ls = tx.add_layer(cs, Affine::identity());
    tx.attach_layer(comp, ls);
    tx.commit();
    binding[cs] = &src;
  }
  const DocStatePtr doc = model.current();

  CpuBackend backend;
  InlineAudioPull pull;
  NestedContent nested(comp);
  nested.attach(
      pull, backend,
      [&binding](ObjectId id) -> Content* {
        const auto it = binding.find(id);
        return it != binding.end() ? it->second : nullptr;
      },
      *doc);

  const std::uint32_t rate = 48'000;
  const std::uint32_t frames = 32;
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(rate);
  const TimeRange window{Time::zero(), Time{static_cast<std::int64_t>(frames) * fpf}};

  const std::vector<float> ref = render_audio(nested, window, rate, frames);

  constexpr int k_threads = 8;
  std::vector<std::vector<float>> results(k_threads);
  std::vector<std::thread> threads;
  std::atomic<bool> go{false};
  threads.reserve(k_threads);
  for (int i = 0; i < k_threads; ++i) {
    threads.emplace_back([&, i] {
      while (!go.load(std::memory_order_acquire)) {
      }
      for (int rep = 0; rep < 16; ++rep) {
        results[static_cast<std::size_t>(i)] = render_audio(nested, window, rate, frames);
      }
    });
  }
  go.store(true, std::memory_order_release);
  for (std::thread& t : threads) {
    t.join();
  }

  for (const std::vector<float>& r : results) {
    REQUIRE(bytes_equal(r, ref));
  }
}
