#pragma once

// The REAP-side asset seam (serialize.asset_gc Decision 2; doc 08 § The asset directory) --
// the THIRD role beside `AssetSource` (read, `load_context.hpp`) and `AssetSink` (write,
// `save_context.hpp`). It is kept a separate role deliberately: `AssetSink` says of itself
// "A sink NEVER DELETES" (`save_context.hpp:70-73`), and folding delete into it would
// contradict that invariant. A host that stores blobs somewhere other than a POSIX
// directory implements reap independently of write.
//
// Like the write side, this seam is BYTE/FORMAT-oriented and FILESYSTEM-FREE: it names no
// raster type and no `std::filesystem` type, so it rides `arbc::serialize` (L4). The
// concrete `FilesystemAssetReaper`, the JSON mark walk, and the top-level GC driver live in
// `arbc::runtime` (L5) -- the one place that already sees the filesystem, the JSON library,
// and the raster `params.blobs` shape (`arbc/runtime/asset_gc.hpp`).
//
// GC is an EXPLICIT, CALLER-ROOTED, MARK-AND-SWEEP: enumerate every tile blob on disk,
// subtract the union of hashes the caller's preserved documents reference, and delete the
// difference. It is never on the save path -- a save cannot see the references an in-memory
// undo history or another editor holds (doc 08 L50-53). See `unreferenced_tiles` for the
// pure subtraction and `arbc/runtime/asset_gc.hpp` for the driver.

#include <arbc/arbc_api.h>
#include <arbc/base/expected.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace arbc {

// A reap operation that could not happen, as a value (doc 10). A directory that cannot be
// enumerated or a blob that cannot be removed surfaces here, never as a throw. The GC
// driver's fail-safe (Constraint 3) turns either into ZERO further deletions.
struct AssetReaperError {
  enum class Kind {
    EnumerateFailed, // the on-disk tile set could not be listed / a blob could not be sized
    RemoveFailed,    // a blob deletion failed (I/O, permissions)
  };
  Kind kind{Kind::EnumerateFailed};

  friend bool operator==(const AssetReaperError&, const AssetReaperError&) = default;
};

// The abstract reap seam, symmetric to `AssetSource`/`AssetSink`. A concrete store
// enumerates its tile blobs, reports a blob's size, and removes a blob -- and nothing more.
// The mark (which hashes to keep) and the plan (subtract, then delete) live above it, in the
// runtime driver, so this interface stays a pure store operation with no policy.
class ARBC_API AssetReaper {
public:
  virtual ~AssetReaper() = default;

  // Every on-disk tile blob's 32-hex name, under the resolved tiles subtree. A store with no
  // tiles yet is an EMPTY list, not an error. Anything whose basename is not a valid tile
  // hash is not a tile blob and is not listed (Constraint 4).
  virtual expected<std::vector<std::string>, AssetReaperError> list_tile_hashes() const = 0;

  // The byte size of the blob named `hash`. Needed to report `bytes_reclaimed` WITHOUT
  // deleting -- a `dry_run` previews exactly what a real run reclaims (acceptance:
  // dry-run/real reports agree), and the whole delete plan is sized before a single unlink
  // (Decision 3).
  virtual expected<std::uint64_t, AssetReaperError> tile_size(std::string_view hash) const = 0;

  // Delete the blob for `hash`; returns whether a file was actually removed (a re-run finds
  // it already gone -- removing a content-addressed blob is idempotent, Decision 3).
  virtual expected<bool, AssetReaperError> remove_tile(std::string_view hash) = 0;
};

// The pure set subtraction at the heart of the sweep (Decision 3): the blobs present on disk
// that no preserved document references. No I/O, no raster type, no filesystem -- so it lives
// at L4 and is unit-testable in isolation. Order-preserving over `present` for deterministic
// reporting. A `referenced` hash absent from disk is harmless (it simply matches nothing); a
// `present` hash absent from `referenced` is the orphan this returns.
ARBC_API std::vector<std::string>
unreferenced_tiles(const std::unordered_set<std::string>& referenced,
                   std::span<const std::string> present);

} // namespace arbc
