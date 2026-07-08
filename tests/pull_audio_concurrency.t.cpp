// Concurrency stress for `PullServiceImpl::pull_audio` (doc 16 tier-6, tsan lane;
// refinement Acceptance "Concurrency / TSan") -- the audio twin of
// `pull_service_async.t.cpp`. Many `pull_audio` requests for the SAME and for
// DISTINCT `BlockKey`s are issued from the single monitor thread: a resident
// exact-fresh block serves inline there (a single-threaded cache read, exactly as
// the tile cache is single-writer on the frame thread), while a miss is dispatched
// onto a real thread pool whose workers render and settle the `AudioCompletion`
// concurrently. TSan must report no data race on the completion plumbing or the
// block cache, and every completion must settle EXACTLY once. Cross-component
// (CpuBackend + compositor), so it links the umbrella `arbc` and lives here.

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/cache/key_shapes.hpp>
#include <arbc/cache/keyed_store.hpp>
#include <arbc/compositor/pull_service.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/media/audio_block.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using namespace arbc;

// A stateless, thread-safe audio leaf: sample = parabolic sine over an exact flick
// phase (never std::sin), so concurrent renders into thread-confined buffers are
// race-free and deterministic. Reports the requested rate / exact (rate-honoring).
class AudioLeaf final : public Content {
public:
  explicit AudioLeaf(std::uint32_t freq_hz) : d_facet(freq_hz) {}
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
    explicit Facet(std::uint32_t freq_hz) : d_freq(freq_hz) {}
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
        const std::int64_t r = (static_cast<std::int64_t>(d_freq) * t) % fps;
        double p = 2.0 * (static_cast<double>(r) / static_cast<double>(fps));
        if (p > 1.0) {
          p -= 2.0;
        }
        const double abs_p = p < 0.0 ? -p : p;
        const float v = static_cast<float>(0.5 * (4.0 * p * (1.0 - abs_p)));
        for (std::uint32_t c = 0; c < ch; ++c) {
          request.target.samples[static_cast<std::size_t>(f) * ch + c] = v;
        }
      }
      return AudioResult{request.sample_rate, true};
    }

  private:
    std::uint32_t d_freq;
  };
  Facet d_facet;
};

// One pull's caller-owned state: its target buffer, block view, completion, and
// (for a hit) the samples the cache should return.
struct Pull {
  std::vector<float> buf;
  AudioBlock block{};
  std::shared_ptr<AudioCompletion> done;
  bool expect_hit{false};
};

constexpr std::uint32_t k_rate = 48'000;
constexpr std::uint32_t k_frames = 16;

AudioRequest make_request(Pull& p, std::uint32_t rate, Time start) {
  p.buf.assign(static_cast<std::size_t>(k_frames) * 2, 0.0F);
  p.block = AudioBlock{p.buf.data(), k_frames, ChannelLayout::Stereo, rate};
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(rate);
  return AudioRequest{
      TimeRange{start, Time{start.flicks + static_cast<std::int64_t>(k_frames) * fpf}},
      rate,
      ChannelLayout::Stereo,
      p.block,
      Exactness::Exact,
      StateHandle{},
  };
}

} // namespace

// enforces: 12-audio#pull-audio-is-cache-first-single-settle
TEST_CASE("pull_audio: concurrent same/distinct-key pulls settle race-free, each exactly once") {
  constexpr int k_distinct = 8; // distinct contents -> distinct BlockKeys
  constexpr int k_reps = 8;     // repeats per content -> same-key concurrency
  constexpr std::uint64_t k_rev = 1;

  CpuBackend backend;
  TileCache cache(64u * 1024 * 1024);
  BlockCache blocks(64u * 1024 * 1024);

  std::vector<std::unique_ptr<AudioLeaf>> leaves;
  std::unordered_map<const Content*, ObjectId> ids;
  for (int i = 0; i < k_distinct; ++i) {
    leaves.push_back(std::make_unique<AudioLeaf>(static_cast<std::uint32_t>(220 + 40 * i)));
    ids.emplace(leaves.back().get(), ObjectId{static_cast<std::uint32_t>(i + 1)});
  }

  // The deferred-dispatch seam: a miss appends a render-and-settle job that runs on
  // a worker thread. `pull_audio` returns before the job runs (an off-thread miss),
  // so the block cache is touched only on the issuing thread here.
  std::vector<std::function<void()>> jobs;
  AudioDispatch dispatch = [&jobs](Content* content, const AudioRequest& request,
                                   std::shared_ptr<AudioCompletion> done) {
    jobs.emplace_back([content, request, done]() {
      AudioFacet* af = content != nullptr ? content->audio() : nullptr;
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
    });
  };

  PullConfig config;
  config.id_of = [&ids](const Content* c) {
    const auto it = ids.find(c);
    return it != ids.end() ? it->second : ObjectId{};
  };
  config.contribution = [](const Content*) { return k_rev; };
  config.audio_dispatch = dispatch;
  config.blocks = &blocks;
  PullServiceImpl service(cache, backend, direct_dispatch(), config);

  // Pre-populate every even-indexed content as a resident exact-fresh block: those
  // pulls serve inline on this (issuing) thread with zero dispatch, exercising the
  // cache-first read path alongside the concurrent misses.
  for (int i = 0; i < k_distinct; i += 2) {
    Pull probe;
    const AudioRequest req = make_request(probe, k_rate, Time::zero());
    AudioBlockValue value;
    value.samples.assign(static_cast<std::size_t>(k_frames) * 2, static_cast<float>(i) + 100.0F);
    value.frames = k_frames;
    value.layout = ChannelLayout::Stereo;
    value.rate = k_rate;
    value.meta = AudioResult{k_rate, true};
    const std::size_t bytes = value.samples.size() * sizeof(float);
    const BlockKey key{ids[leaves[static_cast<std::size_t>(i)].get()], k_rev,
                       audio_block_index(req), k_rate};
    blocks.insert(key, std::move(value), bytes, PriorityClass::Visible);
  }

  // Issue every pull from the single monitor thread: repeats of one content share a
  // BlockKey; different contents give distinct keys. Even contents hit (served
  // inline), odd contents miss (deferred to the worker pool).
  std::vector<std::unique_ptr<Pull>> pulls;
  for (int i = 0; i < k_distinct; ++i) {
    for (int rep = 0; rep < k_reps; ++rep) {
      auto p = std::make_unique<Pull>();
      p->done = std::make_shared<AudioCompletion>();
      p->expect_hit = (i % 2 == 0);
      const AudioRequest req = make_request(*p, k_rate, Time::zero());
      service.pull_audio(leaves[static_cast<std::size_t>(i)].get(), req, p->done);
      pulls.push_back(std::move(p));
    }
  }

  // Every hit settled inline on this thread already; only the misses are deferred.
  // Run them across a real worker pool so their completions settle concurrently.
  std::atomic<std::size_t> next{0};
  auto worker = [&jobs, &next]() {
    for (;;) {
      const std::size_t idx = next.fetch_add(1, std::memory_order_relaxed);
      if (idx >= jobs.size()) {
        break;
      }
      jobs[idx]();
    }
  };
  constexpr int k_threads = 8;
  std::vector<std::thread> threads;
  threads.reserve(k_threads);
  for (int t = 0; t < k_threads; ++t) {
    threads.emplace_back(worker);
  }
  for (std::thread& t : threads) {
    t.join();
  }

  // Every completion settled EXACTLY once, on whichever path; hits carry the
  // resident samples, misses the worker-rendered samples. A missing settle, a
  // double settle, or a corrupted block would fail here (and TSan flags any race).
  for (const std::unique_ptr<Pull>& p : pulls) {
    REQUIRE(p->done->settled());
    const auto settled = p->done->take();
    REQUIRE(settled.has_value());
    REQUIRE(settled->has_value());
    CHECK((*settled)->achieved_rate == k_rate);
    CHECK((*settled)->exact);
    CHECK(p->done->take() == std::nullopt); // the single settlement is yielded once
    if (p->expect_hit) {
      for (const float v : p->buf) {
        CHECK(v >= 100.0F); // the distinctive resident fill
      }
    }
  }
  // No fill happened on any path -- the resident set is exactly what was seeded.
  CHECK(blocks.resident_bytes() == static_cast<std::size_t>(k_distinct / 2) *
                                       (static_cast<std::size_t>(k_frames) * 2) * sizeof(float));
}
