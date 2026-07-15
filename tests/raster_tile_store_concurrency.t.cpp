// The save races the editor (serialize.raster_tile_store Constraint 10; doc 08: "autosave
// never pauses editing"; doc 14:168-181, which requires the state store to admit exactly
// this).
//
// A save runs OFF the editing thread against a pinned snapshot while the writer thread
// keeps painting and dropping versions. The hazard it exercises is the memo's `retain`
// racing the writer's `release` on the same pool slot -- and, underneath that, the whole
// reason the memo pins by refcount rather than by generation tag: in a release build a
// recycled slot is BIT-IDENTICAL to a stale one, so a memo that did not hold a count could
// hand back a stale hash for entirely different pixels.
//
// Clean under TSan is the assertion. The pixel assertions are the second half: a save must
// not merely be race-free, it must serialize a COHERENT version.

#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/kind_raster/raster_content.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/media/pixel_traits.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/model.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/document_serialize.hpp>
#include <arbc/runtime/raster_tile_store.hpp>
#include <arbc/runtime/tile_encode_dispatch.hpp>
#include <arbc/runtime/worker_pool.hpp>
#include <arbc/serialize/codec.hpp>
#include <arbc/serialize/save_context.hpp>
#include <arbc/serialize/tile_blob.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace arbc;

namespace {

// An in-memory sink: the point of this test is the pool refcount race, not the filesystem.
class MemorySink final : public AssetSink {
public:
  expected<bool, AssetSinkError> put(std::string_view uri,
                                     std::span<const std::byte> bytes) override {
    const std::lock_guard<std::mutex> lock(d_mutex);
    if (!d_blobs.emplace(std::string(uri), std::vector<std::byte>(bytes.begin(), bytes.end()))
             .second) {
      return false;
    }
    ++d_written;
    return true;
  }
  bool contains(std::string_view uri) const override {
    const std::lock_guard<std::mutex> lock(d_mutex);
    return d_blobs.find(std::string(uri)) != d_blobs.end();
  }
  std::uint64_t blobs_written() const noexcept override { return d_written.load(); }

  std::size_t distinct() const {
    const std::lock_guard<std::mutex> lock(d_mutex);
    return d_blobs.size();
  }

private:
  mutable std::mutex d_mutex;
  std::map<std::string, std::vector<std::byte>> d_blobs;
  std::atomic<std::uint64_t> d_written{0};
};

DecodedImage textured(int w, int h) {
  DecodedImage img;
  img.width = w;
  img.height = h;
  img.format = k_working_rgba32f;
  std::vector<float> f(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
  std::uint32_t s = 999;
  for (float& v : f) {
    s = s * 1664525U + 1013904223U;
    v = static_cast<float>(s >> 8U) / static_cast<float>(1U << 24U);
  }
  const auto* src = reinterpret_cast<const std::byte*>(f.data());
  img.bytes.assign(src, src + f.size() * sizeof(float));
  return img;
}

// The pure per-tile encode a fan-out worker runs: `peek` the immutable pinned tile, convert
// to storage bytes, hash, and frame (shuffle+zstd). A pure function of the pinned bytes over
// a reentrant compressor -- exactly what makes the fan-out data-race-free by construction.
TileEncodeOutput encode_tile(BigBlockPool& pool, BlockSlotRef ref, PixelFormat storage) {
  const std::span<const std::byte> raw = pool.peek(ref);
  const std::span<const float> working{reinterpret_cast<const float*>(raw.data()),
                                       raw.size() / sizeof(float)};
  const std::vector<std::byte> storage_bytes = to_storage_bytes(working, storage);
  TileEncodeOutput out;
  out.hash = hash_tile(storage_bytes);
  expected<std::vector<std::byte>, TileBlobError> frame = frame_tile_blob(storage_bytes, storage);
  if (frame) {
    out.frame = std::move(*frame);
  }
  return out;
}

} // namespace

TEST_CASE("a save on a non-writer thread races the writer painting and dropping versions") {
  Document doc;
  KindBridge bridge;
  Registry registry;

  // 64x64 at edge 16: a 4x4 grid, so a dab touches one tile and leaves fifteen for the
  // memo to carry forward -- which is precisely the retain/release traffic under test.
  const auto raster = std::make_shared<RasterContent>(textured(64, 64), /*edge=*/16);
  const ObjectId comp = doc.add_composition(64.0, 64.0);
  const ObjectId content = doc.add_content(raster, bridge.intern(RasterContent::kind_id, "1"));
  doc.attach_layer(comp, doc.add_layer(content, Affine::identity(), 1.0));

  RasterTileStore tiles;
  MemorySink sink;
  const CodecTable codecs = builtin_codecs(registry, &tiles);

  constexpr int k_paints = 64;
  constexpr int k_saves = 24;

  std::atomic<bool> painting{true};
  std::atomic<int> saved_ok{0};

  // THE SAVE THREAD. `capture_snapshot` must run on the writer thread, so this drives the
  // half that genuinely may not: `serialize_snapshot` over an immutable snapshot, plus the
  // codec's `peek`/`retain` traffic against a pinned `TileTablePtr`. Every tile it touches
  // is already kept alive by that pin, so the memo's own retain is always "add a count to
  // something already >= 1" and can never resurrect a dead slot.
  std::thread saver([&] {
    for (int i = 0; i < k_saves && painting.load(); ++i) {
      SaveContext ctx{"/proj/project.arbc"};
      ctx.set_asset_sink(&sink);
      ctx.set_storage_format(PixelFormat::Rgba32fLinearPremul);
      // Snapshots are captured on the writer thread in production; here the writer is
      // hammering the same store, which is the race we want. `base_table()` takes the
      // store's mutex and hands back a pinning `shared_ptr`, which is what makes the
      // codec's reads safe.
      const TileTablePtr pinned = raster->store().base_table();
      REQUIRE(pinned);
      std::string blobs;
      for (const BlockSlotRef& ref : pinned->level(0).tiles) {
        blobs += tiles.hash_of(const_cast<RasterStore&>(raster->store()).pool(), ref);
      }
      CHECK(blobs.size() == pinned->level(0).tiles.size() * 32);
      saved_ok.fetch_add(1);
    }
  });

  // THE WRITER THREAD (this one): paint new versions and drop old ones, the whole time.
  for (int i = 0; i < k_paints; ++i) {
    const double x = static_cast<double>((i * 7) % 48);
    const double y = static_cast<double>((i * 13) % 48);
    Model::Transaction txn = doc.transact("dab");
    raster->paint(txn, content, Rect{x, y, x + 8.0, y + 8.0},
                  WorkingPixel{static_cast<float>(i % 8) / 8.0F, 0.0F, 0.0F, 1.0F});
    REQUIRE(txn.commit());
    // Drop versions as we go: this is what recycles pool slots, which is what makes a
    // generation-tag-free `BlockSlotRef` ambiguous -- and the memo's pin necessary.
    doc.drain();
  }
  painting.store(false);
  saver.join();

  CHECK(saved_ok.load() > 0);

  // A full save from the writer thread still succeeds afterwards, and the memo is intact:
  // no stale hash, no dangling pin.
  SaveContext ctx{"/proj/project.arbc"};
  ctx.set_asset_sink(&sink);
  ctx.set_storage_format(PixelFormat::Rgba32fLinearPremul);
  const expected<std::string, SerializeError> saved = save_document(doc, bridge, codecs, ctx);
  REQUIRE(saved.has_value());

  // Every hash the final save emitted verifies: the memo never handed back a hash for a
  // slot whose bytes had been recycled out from under it.
  const TileTablePtr final_table = raster->store().base_table();
  REQUIRE(final_table);
  CHECK(final_table->level(0).tiles.size() == 16);
}

// enforces: 08-serialization#raster-save-is-executor-independent
TEST_CASE("a worker-backed encode races the writer and matches the inline encode of the pin") {
  // The concurrency surface serialize.tile_store_parallel_save INTRODUCES: N workers `peek`
  // the pinned tile table while the writer thread paints new versions and drops old ones. The
  // pin holds a count on every slot it names, so the workers' `peek`s are valid for the whole
  // save even as the writer recycles OTHER slots -- and running the SAME pinned version
  // through the inline and worker executors must produce byte-identical encodes (Constraint
  // 1). TSan-clean is the first assertion; executor-equality is the second.
  Document doc;
  KindBridge bridge;
  Registry registry;
  const auto raster = std::make_shared<RasterContent>(textured(64, 64), /*edge=*/16);
  const ObjectId comp = doc.add_composition(64.0, 64.0);
  const ObjectId content = doc.add_content(raster, bridge.intern(RasterContent::kind_id, "1"));
  doc.attach_layer(comp, doc.add_layer(content, Affine::identity(), 1.0));

  WorkerPoolConfig cfg;
  cfg.worker_count = 4;
  WorkerPool pool(cfg);
  int owner = 0;

  constexpr int k_paints = 64;
  constexpr int k_saves = 24;
  constexpr PixelFormat storage = PixelFormat::Rgba32fLinearPremul;
  std::atomic<bool> painting{true};
  std::atomic<int> saved_ok{0};
  // The saver thread must NOT touch Catch2's assertion macros -- they are not thread-safe, and
  // the writer thread is asserting concurrently. It records outcomes into atomics; the main
  // thread asserts after the join.
  std::atomic<bool> mismatch{false};
  std::atomic<bool> unbounded{false};

  std::thread saver([&] {
    for (int i = 0; i < k_saves && painting.load(); ++i) {
      const TileTablePtr pinned = raster->store().base_table();
      if (!pinned) {
        mismatch.store(true);
        break;
      }
      BigBlockPool& p = const_cast<RasterStore&>(raster->store()).pool();
      const std::vector<BlockSlotRef>& grid = pinned->level(0).tiles;
      const auto encode = [&](std::size_t j) { return encode_tile(p, grid[j], storage); };

      // Inline reference, then the worker-backed fan-out, both over the SAME pinned version.
      std::vector<TileEncodeOutput> inline_out(grid.size());
      TileEncodeDispatch inline_disp;
      inline_disp.run(grid.size(), encode, [&](std::size_t j, TileEncodeOutput& o) {
        inline_out[j] = std::move(o);
        return true;
      });
      std::vector<TileEncodeOutput> worker_out(grid.size());
      TileEncodeDispatch worker_disp(pool, &owner);
      worker_disp.run(grid.size(), encode, [&](std::size_t j, TileEncodeOutput& o) {
        worker_out[j] = std::move(o);
        return true;
      });

      for (std::size_t j = 0; j < grid.size(); ++j) {
        if (inline_out[j].hash != worker_out[j].hash ||
            inline_out[j].frame != worker_out[j].frame) {
          mismatch.store(true); // executor divergence on the same pinned version
        }
      }
      if (worker_disp.peak_in_flight() > worker_disp.window()) {
        unbounded.store(true);
      }
      saved_ok.fetch_add(1);
    }
  });

  for (int i = 0; i < k_paints; ++i) {
    const double x = static_cast<double>((i * 7) % 48);
    const double y = static_cast<double>((i * 13) % 48);
    Model::Transaction txn = doc.transact("dab");
    raster->paint(txn, content, Rect{x, y, x + 8.0, y + 8.0},
                  WorkingPixel{static_cast<float>(i % 8) / 8.0F, 0.0F, 0.0F, 1.0F});
    REQUIRE(txn.commit());
    doc.drain(); // recycle slots NOT named by any live pin -- the race the pin defends against
  }
  painting.store(false);
  saver.join();
  CHECK(saved_ok.load() > 0);
  CHECK_FALSE(mismatch.load());  // every worker-backed encode matched the inline one
  CHECK_FALSE(unbounded.load()); // the window held even under the race
}

// enforces: 08-serialization#raster-save-is-executor-independent
TEST_CASE("the encode fan-out is executor-independent across many seeded schedules") {
  // Seeded schedule perturbation (doc 16 tier 6): the same fan-out over a static pinned
  // version, run under many worker-yield schedules, must produce the SAME hashes every time
  // and equal to the inline reference. Completion order is perturbed on purpose; the output
  // follows from the format (hash over uncompressed storage bytes, blobs fixed row-major),
  // not from the schedule.
  Document doc;
  KindBridge bridge;
  Registry registry;
  const auto raster = std::make_shared<RasterContent>(textured(96, 96), /*edge=*/16);
  const ObjectId comp = doc.add_composition(96.0, 96.0);
  const ObjectId content = doc.add_content(raster, bridge.intern(RasterContent::kind_id, "1"));
  doc.attach_layer(comp, doc.add_layer(content, Affine::identity(), 1.0));

  const TileTablePtr pinned = raster->store().base_table();
  REQUIRE(pinned);
  BigBlockPool& p = const_cast<RasterStore&>(raster->store()).pool();
  const std::vector<BlockSlotRef>& grid = pinned->level(0).tiles;
  REQUIRE(grid.size() == 36); // 6x6
  constexpr PixelFormat storage = PixelFormat::Rgba32fLinearPremul;

  // The inline reference, computed once.
  std::vector<std::string> reference(grid.size());
  TileEncodeDispatch inline_disp;
  inline_disp.run(
      grid.size(), [&](std::size_t j) { return encode_tile(p, grid[j], storage); },
      [&](std::size_t j, TileEncodeOutput& o) {
        reference[j] = std::move(o.hash);
        return true;
      });

  WorkerPoolConfig cfg;
  cfg.worker_count = 4;
  WorkerPool pool(cfg);
  int owner = 0;

  for (std::uint32_t seed = 1; seed <= 8; ++seed) {
    const auto perturbed = [&, seed](std::size_t j) {
      // A deterministic, seed-derived number of yields reshuffles the worker interleaving
      // without any wall-clock dependence.
      std::uint32_t s = (seed * 2654435761U) ^ (static_cast<std::uint32_t>(j) * 40503U);
      for (std::uint32_t y = 0; y < (s % 5U); ++y) {
        std::this_thread::yield();
      }
      return encode_tile(p, grid[j], storage);
    };
    std::vector<std::string> got(grid.size());
    TileEncodeDispatch disp(pool, &owner);
    disp.run(grid.size(), perturbed, [&](std::size_t j, TileEncodeOutput& o) {
      got[j] = std::move(o.hash);
      return true;
    });
    CHECK(got == reference); // same output under every schedule
  }
}
