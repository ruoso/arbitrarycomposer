#pragma once

// The tree's first `AssetSink` (serialize.raster_tile_store Decision 1) -- the exact
// mirror of `FilesystemAssetSource`, and in `runtime` (L5) for the same reason: nothing
// below L5 may both own a `Document` and touch a file.
//
// Names no JSON type, so it rides the runtime PUBLIC headers: a host installs it, and a
// test drives it, without ever seeing nlohmann.

#include <arbc/serialize/save_context.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace arbc {

// An `AssetSink` that writes the resolved URI as a filesystem path (a `file://` prefix
// is stripped, as on the read side; anything else is taken as a plain path). Parent
// directories are created as needed -- the two-hex fan-out means a save is routinely the
// first thing to touch `tiles/3f/`.
//
// WRITE-IF-ABSENT. A name already on disk is left completely alone: content-addressing
// makes presence-by-name sufficient proof that the bytes are right, so re-writing would
// be pure I/O for no change. That is what makes an incremental save incremental.
//
// CRASH-ATOMIC. Bytes go to a temporary name in the TARGET directory and are `rename`d
// into place -- an atomic operation on the same filesystem. This is not belt-and-braces:
// a truncated blob left under a valid hash name would poison every FUTURE save's
// write-if-absent check, which would then skip it forever, and the user's pixels would be
// silently gone. A crash or an out-of-disk mid-write must leave the store either
// unchanged or complete, never plausible-but-wrong.
//
// A SAVE NEVER DELETES (Constraint 6). Another document version, another `.arbc`, or a
// concurrent editor may reference a blob this document no longer does, and an incremental
// save cannot know. Reclaiming unreferenced blobs is an explicit user-driven sweep
// (`serialize.asset_gc`), never a side effect of saving.
class FilesystemAssetSink final : public AssetSink {
public:
  expected<bool, AssetSinkError> put(std::string_view resolved_uri,
                                     std::span<const std::byte> bytes) override;

  bool contains(std::string_view resolved_uri) const override;

  // Behavioral counters (doc 16 tier 4). `blobs_written()` is the incremental-save
  // witness: a re-save after one dab must advance it by exactly the number of touched
  // tiles. `puts()` counts every offer, so `puts() - blobs_written()` is the dedup /
  // already-present count.
  std::uint64_t blobs_written() const noexcept override { return d_written; }
  std::uint64_t puts() const noexcept { return d_puts; }
  std::uint64_t bytes_written() const noexcept { return d_bytes; }

private:
  std::uint64_t d_puts{0};
  std::uint64_t d_written{0};
  std::uint64_t d_bytes{0};
};

} // namespace arbc
