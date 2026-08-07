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
// TWO KEY SPACES (issue #30). The store holds content-addressed TILE blobs, keyed by hash off
// `params.blobs`, and NAMED asset blobs -- an image the host copied into the project so the
// project is self-contained -- keyed by the authored URI off `params.source`. Both halves of
// the GC see both: the mark walk harvests both shapes and the reaper enumerates both subtrees.
// Landing them together was not optional. The reaper alone would see every owned image as an
// orphan and delete it; the mark alone would root URIs nothing ever sweeps. One leaks, the
// other loses data, and only the pair is correct.
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
#include <optional>
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
  // One NAMED-asset subtree the reaper sweeps beside the tiles (issue #30). A base rather than
  // a hard-coded `assets/images/` because the reference model is the SHAPE, not the kind: any
  // future blob-bearing kind that authors a `params.source` under a project-owned directory is
  // swept by naming its base here, with no change to this class.
  struct AssetBase {
    std::string authored;     // the prefix a document authors, e.g. `assets/images/`
    std::string resolved_uri; // the same base resolved against the project (file:// tolerated)
  };

  // `tiles_base_uri` is the resolved tiles base (e.g. `<project>/assets/tiles/`); a `file://`
  // prefix is stripped exactly as the sink/source strip it. With no `asset_bases` this is the
  // tiles-only reaper it has always been.
  explicit FilesystemAssetReaper(std::string tiles_base_uri,
                                 std::vector<AssetBase> asset_bases = {});

  expected<std::vector<std::string>, AssetReaperError> list_tile_hashes() const override;
  expected<std::uint64_t, AssetReaperError> tile_size(std::string_view hash) const override;
  expected<bool, AssetReaperError> remove_tile(std::string_view hash) override;

  // The named-asset half: enumerate every regular file under each configured base and report
  // it as `<authored><path relative to the base>`, size one, remove one. A key that resolves
  // to no configured base -- or that escapes its base through `..` -- is an error value and
  // never a path this touches: `remove` is the one irreversible operation in this file.
  expected<std::vector<std::string>, AssetReaperError> list_asset_uris() const override;
  expected<std::uint64_t, AssetReaperError> asset_size(std::string_view uri) const override;
  expected<bool, AssetReaperError> remove_asset(std::string_view uri) override;

private:
  // The resolved path of `uri`, or `nullopt` when it names no configured base or escapes one.
  std::optional<std::filesystem::path> asset_path(std::string_view uri) const;

  struct ResolvedBase {
    std::string authored;
    std::filesystem::path root;
  };

  std::string d_base_uri;       // the resolved tiles base, for `tile_blob_uri` derivation
  std::filesystem::path d_root; // the stripped path, for enumeration
  std::vector<ResolvedBase> d_asset_bases; // the named-asset subtrees, if any
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

// Everything one document roots, in the TWO key spaces the store has (issue #30): the
// content-addressed tile hashes off `params.blobs`, and the authored asset URIs off
// `params.source` -- which is where the image codec keeps its asset reference
// (`codec_image.cpp`) and therefore the only handle a document has on an owned image blob.
// Harvesting only the first was why an owned image was neither rooted nor reclaimed.
struct GcRoots {
  std::unordered_set<std::string> tiles;  // 32-hex blob names
  std::unordered_set<std::string> assets; // authored URIs, `file://` stripped and normalized
};

// THE MARK WALK over both key spaces, same traversal and same fail-safe as
// `collect_referenced_tiles` (which is this, with the assets half discarded). A `params.source`
// that is present but not a string is a `MalformedField` value here even though the image codec
// treats it LENIENTLY on load: leniency on a read means "round-trip the key verbatim", while
// leniency on a delete plan would mean guessing about a blob's liveness, and this module's rule
// is over-preservation on any doubt -- returning an error deletes nothing at all.
ARBC_API expected<GcRoots, ReaderError> collect_referenced_assets(std::string_view document_json);

// THE SWEEP (Decision 3). Enumerate the on-disk set, subtract `referenced` in FULL, size the
// whole plan, and only THEN unlink -- so a mark bug can never race a delete and a partial run
// removes only a strict subset of genuine orphans, leaving every referenced blob intact.
// `dry_run` computes and reports the identical plan without deleting. `referenced` is the
// UNION across every document the caller preserves (Constraint 5).
ARBC_API expected<GcReport, GcError>
sweep_tile_store(const std::unordered_set<std::string>& referenced, AssetReaper& reaper,
                 bool dry_run);

// THE SWEEP OVER BOTH KEY SPACES (issue #30): the tile half above, then the named-asset half,
// each enumerated and subtracted in full before either unlinks. The report's counters are the
// SUM across the two stores, because what a user asked for is "how much did Clean-Up reclaim".
//
// The two halves are one call on purpose. The reaper enumerating `assets/images/` and the mark
// walk harvesting `params.source` are useless apart and DANGEROUS apart: a sweep that lists
// owned images without rooting them sees every one of them as an orphan and deletes the lot.
// Landing them behind one entry point is what makes that pairing structural.
ARBC_API expected<GcReport, GcError> sweep_asset_store(const GcRoots& roots, AssetReaper& reaper,
                                                       bool dry_run);

// The convenience entry: scan `project_dir` for `*.arbc`, union their marks, resolve the
// default `assets/tiles/` and `assets/images/` bases against the directory the same way a
// save/load does, and sweep that shared store. Every document in one project directory shares one
// tile store, so this is the direct answer to doc 08's cross-`.arbc` hazard: a blob any of them
// references survives. A document NOT in `project_dir` is not a root -- its unique blobs are
// reclaimed, which is the caller-completeness contract (Constraint 5).
ARBC_API expected<GcReport, GcError> gc_project_directory(const std::filesystem::path& project_dir,
                                                          bool dry_run);

} // namespace arbc
