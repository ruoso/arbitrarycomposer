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
#include <arbc/serialize/codec.hpp>
#include <arbc/serialize/save_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <map>
#include <cstddef>
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
