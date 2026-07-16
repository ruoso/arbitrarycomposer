#pragma once

// EXPLICIT, CALLER-ROOTED MARK-AND-SWEEP GC over the on-disk tile store (serialize.asset_gc).
// The back half of the asset-directory data-safety story: `FilesystemAssetSink` is
// write-if-absent and NEVER DELETES (a save cannot prove a blob dead -- another document
// version, another `.arbc`, or a concurrent editor may reference it, doc 08 L50-53), so the
// asset directory only ever grows. This is the reclamation the user drives explicitly.
//
// THE L4/L5 SPLIT (Decision 2), mirroring the write side exactly. The abstract `AssetReaper`
// seam and the pure `unreferenced_tiles` subtraction are in `arbc::serialize` (L4,
// `asset_reaper.hpp`). This runtime (L5) header owns the concrete `FilesystemAssetReaper`,
// the JSON mark walk, and the driver -- the one place that sees the filesystem, the JSON
// library, and the raster `params.blobs` shape. Like every runtime PUBLIC header it names NO
// JSON type (Constraint 7): the mark walk takes a document's TEXT, not an `nlohmann::json`.
//
// THE SAFETY CONTRACT IS THE CALLER'S (Constraint 5). GC reclaims every blob no named root
// references; the caller must name every document (and every in-memory-open document's
// current serialized state) that must survive. Over-preservation is always safe. Deleting a
// blob a still-RESIDENT document holds is repaired by the sink's write-if-absent on the next
// save (Decision 6) -- the pool tile is the source of truth, not the blob; only a CLOSED,
// unnamed document's unique blobs are truly at risk, which is why GC is explicit.

#include <arbc/arbc_api.h>
#include <arbc/base/expected.hpp>
#include <arbc/serialize/asset_reaper.hpp>
#include <arbc/serialize/reader.hpp> // ReaderError (the mark walk's fail-safe value)

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace arbc {

// The concrete filesystem reaper (Decision 2), the exact mirror of `FilesystemAssetSink`. It
// enumerates the resolved `assets/tiles/` subtree with the NON-THROWING `std::filesystem`
// overloads (errors are values), filters basenames by `is_tile_hash`, sizes and removes point
// blobs, and prunes an emptied two-hex fan-out directory. It reuses `tile_blob_uri` for the
// per-blob path so its derivation is byte-identical to the sink's.
class ARBC_API FilesystemAssetReaper final : public AssetReaper {
public:
  // `tiles_base_uri` is the resolved tiles base (e.g. `<project>/assets/tiles/`); a `file://`
  // prefix is stripped exactly as the sink/source strip it.
  explicit FilesystemAssetReaper(std::string tiles_base_uri);

  expected<std::vector<std::string>, AssetReaperError> list_tile_hashes() const override;
  expected<std::uint64_t, AssetReaperError> tile_size(std::string_view hash) const override;
  expected<bool, AssetReaperError> remove_tile(std::string_view hash) override;

private:
  std::string d_base_uri;       // the resolved tiles base, for `tile_blob_uri` derivation
  std::filesystem::path d_root; // the stripped path, for enumeration
};

// The counters a sweep reports (doc 16 tier 4: "wall-clock tests lie in CI; counters don't").
// Never a directory size or a timing. `deleted`/`bytes_reclaimed` are what a real run removed
// -- or, in `dry_run`, what one WOULD remove.
struct GcReport {
  std::uint64_t scanned{0};         // tile blobs on disk
  std::uint64_t referenced{0};      // hashes the preserved documents reference
  std::uint64_t deleted{0};         // blobs deleted (would-be, in dry_run)
  std::uint64_t bytes_reclaimed{0}; // their total size

  friend bool operator==(const GcReport&, const GcReport&) = default;
};

// A GC failure, as a value. The mark walk and the reaper have distinct failure modes, so this
// carries whichever fired -- and either way GC has deleted NOTHING (fail-safe, Constraint 3),
// because a `MarkFailed` returns before the sweep and an `EnumerateFailed` returns before the
// first unlink.
struct GcError {
  enum class Kind {
    MarkFailed,      // a root document could not be parsed, or a blobs entry was not a hash
    EnumerateFailed, // the on-disk tile set could not be listed / sized
    RemoveFailed,    // a blob deletion failed (a partial run: a strict subset of orphans gone)
  };
  Kind kind{Kind::MarkFailed};
  ReaderError mark{};      // meaningful iff kind == MarkFailed
  AssetReaperError reap{}; // meaningful iff kind == Enumerate/RemoveFailed
};

// THE MARK WALK (Constraint 6). A generic JSON traversal of one `.arbc` document's TEXT,
// keyed on the reference SHAPE, not on a kind type: it harvests every `params.blobs` hash
// from every content body it reaches -- root-composition layers, non-root compositions, and
// operator input children alike, wherever they nest -- so a raster layer inside a nested
// composition is not missed. Keying on the presence of a `blobs` array (rather than on
// `kind == "org.arbc.raster"`) means an unknown kind's tile references, round-tripped through
// a placeholder, are ALSO preserved: conservative by construction.
//
// FAIL-SAFE (Constraint 3): unparseable text, a non-array `blobs`, or an entry that is not a
// valid `is_tile_hash` string is a `ReaderError` value -- and the driver that calls this then
// deletes nothing. Over-preservation on any doubt.
ARBC_API expected<std::unordered_set<std::string>, ReaderError>
collect_referenced_tiles(std::string_view document_json);

// THE SWEEP (Decision 3). Enumerate the on-disk set, subtract `referenced` in FULL, size the
// whole plan, and only THEN unlink -- so a mark bug can never race a delete and a partial run
// removes only a strict subset of genuine orphans, leaving every referenced blob intact.
// `dry_run` computes and reports the identical plan without deleting. `referenced` is the
// UNION across every document the caller preserves (Constraint 5).
ARBC_API expected<GcReport, GcError>
sweep_tile_store(const std::unordered_set<std::string>& referenced, AssetReaper& reaper,
                 bool dry_run);

// The convenience entry: scan `project_dir` for `*.arbc`, union their marks, resolve the
// default `assets/tiles/` base against the directory the same way a save/load does, and sweep
// that shared store. Every document in one project directory shares one tile store, so this is
// the direct answer to doc 08's cross-`.arbc` hazard: a blob any of them references survives.
// A document NOT in `project_dir` is not a root -- its unique blobs are reclaimed, which is
// the caller-completeness contract (Constraint 5).
ARBC_API expected<GcReport, GcError> gc_project_directory(const std::filesystem::path& project_dir,
                                                          bool dry_run);

} // namespace arbc
