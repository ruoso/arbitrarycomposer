// Concurrency stress for org.arbc.raster (doc 16 tier-6; refinement Acceptance
// "Concurrency"): N worker threads render a pinned StateHandle H concurrently
// while the writer thread paints new versions. Every reader must observe
// byte-stable pixels for H, and TSan must report no data race -- the "render
// workers read frozen state while the writer keeps editing" invariant
// (doc 14:159-162). Cross-component (kind_raster + threads), so it lives in
// tests/ and links the umbrella `arbc` (doc 17:153).
//
// The edits (build + every paint) run on a single writer thread and the readers
// only ever `peek` -- exactly the pool's threading contract (doc 15:137-143;
// `pool.big_block_pool` writer-only `allocate`, any-thread `peek`, blob
// retain/release on the writer/drain thread only; refinement Decision 4). Here
// the main thread is that sole writer; the four worker threads are the RT-side
// readers. This is the end-to-end proof that pool-backed blob allocation stays
// off the reader threads and `peek` is race-free against concurrent writer
// allocation.

#include <arbc/kind_raster/raster_content.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/surface/surface.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <thread>
#include <vector>

namespace {

using namespace arbc;

class MemSurface final : public Surface {
public:
  MemSurface(int w, int h)
      : d_w(w), d_h(h),
        d_bytes(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 16U, std::byte{0}) {}
  int width() const override { return d_w; }
  int height() const override { return d_h; }
  SurfaceFormat format() const override { return k_working_rgba32f; }
  std::span<std::byte> cpu_bytes() override { return d_bytes; }
  std::span<const std::byte> cpu_bytes() const override { return d_bytes; }
  std::vector<std::byte> copy() const { return d_bytes; }

private:
  int d_w;
  int d_h;
  std::vector<std::byte> d_bytes;
};

DecodedImage gradient_16x16() {
  DecodedImage img;
  img.width = 16;
  img.height = 16;
  img.format = k_working_rgba32f;
  std::vector<float> f(static_cast<std::size_t>(16 * 16 * 4));
  for (std::size_t i = 0; i < f.size(); ++i) {
    f[i] = static_cast<float>(i % 97) / 97.0F;
  }
  const auto* src = reinterpret_cast<const std::byte*>(f.data());
  img.bytes.assign(src, src + f.size() * sizeof(float));
  return img;
}

std::vector<std::byte> render_snapshot(RasterContent& content, StateHandle h) {
  MemSurface target(16, 16);
  RenderRequest req{
      Rect{0.0, 0.0, 16.0, 16.0}, 1.0, Time::zero(), h, target, Exactness::Exact, Deadline::none()};
  auto done = std::make_shared<RenderCompletion>();
  (void)content.render(req, done);
  return target.copy();
}

} // namespace

// enforces: 03-layer-plugin-interface#render-pure-over-pinned-state
TEST_CASE("raster render workers read a frozen snapshot while the editor keeps painting") {
  RasterContent content(gradient_16x16(), /*tile_edge=*/4);

  // Capture a frozen snapshot H via a paint, and pin it live for the whole test
  // (a manual retain stands in for the model's record-slot hold).
  Rect touched{};
  const StateHandle h = content.store().paint(content.base_handle(), Rect{2.0, 2.0, 8.0, 8.0},
                                              WorkingPixel{0.5F, 0.25F, 0.125F, 0.5F}, touched);
  content.store().retain_version(h);

  const std::vector<std::byte> reference = render_snapshot(content, h);

  constexpr int k_readers = 4;
  constexpr int k_iterations = 400;
  std::atomic<bool> go{false};
  std::atomic<bool> bad{false};

  std::vector<std::thread> readers;
  for (int r = 0; r < k_readers; ++r) {
    readers.emplace_back([&] {
      while (!go.load(std::memory_order_acquire)) {
      }
      for (int i = 0; i < k_iterations; ++i) {
        if (render_snapshot(content, h) != reference) {
          bad.store(true, std::memory_order_relaxed);
        }
      }
    });
  }

  // The main thread is the sole pool writer: it churns new versions
  // (writer-side pool.allocate + retain/release) while the reader threads peek
  // the pinned snapshot H concurrently.
  go.store(true, std::memory_order_release);
  StateHandle base = content.base_handle();
  for (int i = 0; i < k_iterations; ++i) {
    Rect t{};
    base = content.store().paint(base, Rect{0.0, 0.0, 4.0, 4.0},
                                 WorkingPixel{1.0F, 0.0F, 0.0F, 1.0F}, t);
  }

  for (std::thread& t : readers) {
    t.join();
  }

  REQUIRE_FALSE(bad.load());
  content.store().release_version(h);
}
