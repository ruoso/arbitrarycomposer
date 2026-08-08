// The mark-and-sweep GC driver (serialize.asset_gc). THE CONCRETE HALF (L5): the filesystem
// reaper, the JSON mark walk, and the sweep -- the only place that sees `std::filesystem`, the
// JSON library, and the raster `params.blobs` shape at once. The abstract seam and the pure
// subtraction are L4 (`arbc/serialize/asset_reaper.hpp`), mirroring the write side's
// `AssetSink` (L4) / `FilesystemAssetSink` (L5) split.
//
// ERRORS ARE VALUES all the way down (doc 10), mirroring `filesystem_asset_sink.cpp`: the
// non-throwing `std::filesystem` overloads, an `ifstream` whose failbit is never armed, and
// the non-throwing `nlohmann::json::parse`. No exception escapes into the caller -- a hostile
// or broken `.arbc` on a "clean up" must never take the process down (the loader's discipline,
// serialize.reader).

#include <arbc/base/ids.hpp>
#include <arbc/runtime/asset_gc.hpp>
#include <arbc/serialize/load_context.hpp> // resolve_uri (resolve the tiles base like a save/load)
#include <arbc/serialize/tile_blob.hpp>    // is_tile_hash, tile_blob_uri

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace arbc {
namespace {

using json = nlohmann::json;

// The authored base URI of the blob store, as the raster codec has it
// (`codec_raster.cpp:68`). It goes through the same `resolve_uri` seam nested projects use,
// which is what keeps a project directory relocatable (doc 08:34-36).
constexpr const char* k_tiles_base = "assets/tiles/";

// The base an OWNED image blob lives under (issue #30): a host that copies imported or pasted
// bytes into the project so it is self-contained writes them here, and the document's
// `params.source` authors exactly this prefix. Referenced (not owned) images keep their
// original URI, which points outside this subtree and is therefore never enumerated at all --
// "source-limited" and "project-owned" separate themselves by location, with no flag to keep
// in sync.
constexpr const char* k_images_base = "assets/images/";

// The read/write sides strip exactly this and take everything else as a plain path
// (`filesystem_asset_sink.cpp:22-25`). The three must agree, or a blob written under one
// spelling is unreadable -- and unreapable -- under another.
std::string_view strip_file_scheme(std::string_view uri) {
  constexpr std::string_view k_file = "file://";
  return uri.starts_with(k_file) ? uri.substr(k_file.size()) : uri;
}

ReaderError mark_error(ReaderError::Kind kind, std::string path) {
  return ReaderError{kind, std::move(path), ObjectId{}};
}

GcError from_mark(ReaderError e) {
  GcError g;
  g.kind = GcError::Kind::MarkFailed;
  g.mark = std::move(e);
  return g;
}

GcError from_reaper(const AssetReaperError& e) {
  GcError g;
  g.reap = e;
  g.kind = (e.kind == AssetReaperError::Kind::RemoveFailed) ? GcError::Kind::RemoveFailed
                                                            : GcError::Kind::EnumerateFailed;
  return g;
}

// One authored URI in the form the sweep compares against: the `file://` scheme stripped (the
// three sides must agree on the spelling, above) and the path lexically normalized, so
// `assets/images/./x.png` and `assets/images/x.png` are one key. Generic separators, because a
// URI's separator is `/` on every platform even where the local path's is not.
std::string normalize_asset_uri(std::string_view uri) {
  const std::filesystem::path p{std::string(strip_file_scheme(uri))};
  return p.lexically_normal().generic_string();
}

// Harvest `params.blobs` hashes and `params.source` URIs from every content body reachable in
// the JSON tree (Constraint 6, issue #30). Recurses every object and array, so a body nested
// inside a `compositions` table, an operator `inputs` list, or an unknown kind's preserved
// interior is reached wherever it sits. Returns the first fail-safe violation, else `nullopt`.
std::optional<ReaderError> harvest_node(const json& node, GcRoots& out) {
  if (node.is_object()) {
    if (const auto pit = node.find("params"); pit != node.end() && pit->is_object()) {
      if (const auto bit = pit->find("blobs"); bit != pit->end()) {
        if (!bit->is_array()) {
          return mark_error(ReaderError::Kind::MalformedField, "/params/blobs");
        }
        for (const json& entry : *bit) {
          if (!entry.is_string()) {
            return mark_error(ReaderError::Kind::MalformedField, "/params/blobs");
          }
          const std::string hash = entry.get<std::string>();
          if (!is_tile_hash(hash)) {
            return mark_error(ReaderError::Kind::MalformedField, "/params/blobs");
          }
          out.tiles.insert(hash);
        }
      }
      // The image codec's asset reference (`codec_image.cpp`), and the same shape any future
      // asset-referencing kind uses. Keyed on the KEY, not on `kind == "org.arbc.image"`, so
      // an unknown kind's source -- round-tripped through a placeholder -- roots its blob too.
      if (const auto sit = pit->find("source"); sit != pit->end()) {
        if (!sit->is_string()) {
          return mark_error(ReaderError::Kind::MalformedField, "/params/source");
        }
        const std::string source = sit->get<std::string>();
        if (!source.empty()) {
          out.assets.insert(normalize_asset_uri(source));
        }
      }
    }
    for (const auto& item : node.items()) {
      if (std::optional<ReaderError> e = harvest_node(item.value(), out)) {
        return e;
      }
    }
  } else if (node.is_array()) {
    for (const json& v : node) {
      if (std::optional<ReaderError> e = harvest_node(v, out)) {
        return e;
      }
    }
  }
  return std::nullopt;
}

// Read a whole file as bytes; `nullopt` on any failure (a fail-safe unreadable root).
std::optional<std::string> read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (in.bad()) {
    return std::nullopt;
  }
  return text;
}

} // namespace

// ---- The mark walk ----------------------------------------------------------------------

expected<GcRoots, ReaderError> collect_referenced_assets(std::string_view document_json) {
  const json doc = json::parse(document_json, nullptr, /*allow_exceptions=*/false);
  if (doc.is_discarded()) {
    return unexpected(mark_error(ReaderError::Kind::MalformedJson, "/"));
  }
  GcRoots roots;
  if (std::optional<ReaderError> e = harvest_node(doc, roots)) {
    return unexpected(*e);
  }
  return roots;
}

expected<std::unordered_set<std::string>, ReaderError>
collect_referenced_tiles(std::string_view document_json) {
  // The tiles projection of the walk above -- one traversal, one fail-safe, no second copy of
  // the harvest to drift.
  expected<GcRoots, ReaderError> roots = collect_referenced_assets(document_json);
  if (!roots) {
    return unexpected(roots.error());
  }
  return std::move(roots->tiles);
}

// ---- The filesystem reaper --------------------------------------------------------------

FilesystemAssetReaper::FilesystemAssetReaper(std::string tiles_base_uri,
                                             std::vector<AssetBase> asset_bases)
    : d_base_uri(std::move(tiles_base_uri)), d_root(std::string(strip_file_scheme(d_base_uri))) {
  d_asset_bases.reserve(asset_bases.size());
  for (AssetBase& base : asset_bases) {
    d_asset_bases.push_back(
        ResolvedBase{std::move(base.authored),
                     std::filesystem::path{std::string(strip_file_scheme(base.resolved_uri))}});
  }
}

expected<std::vector<std::string>, AssetReaperError>
FilesystemAssetReaper::list_tile_hashes() const {
  std::vector<std::string> out;

  std::error_code ec;
  const bool present = std::filesystem::exists(d_root, ec);
  if (ec) {
    return unexpected(AssetReaperError{AssetReaperError::Kind::EnumerateFailed});
  }
  if (!present) {
    return out; // no tiles subtree yet: an empty store, not an error
  }

  // The tree's FIRST production directory walk (`filesystem_asset_sink.t.cpp:95` is the only
  // prior use, and it is a test). It carries its own error-as-value handling: the
  // `std::error_code` constructor and `increment`, never a throwing `directory_iterator`.
  std::filesystem::recursive_directory_iterator it(d_root, ec);
  if (ec) {
    return unexpected(AssetReaperError{AssetReaperError::Kind::EnumerateFailed});
  }
  const std::filesystem::recursive_directory_iterator end;
  for (; it != end; it.increment(ec)) {
    if (ec) {
      return unexpected(AssetReaperError{AssetReaperError::Kind::EnumerateFailed});
    }
    std::error_code fec;
    if (!it->is_regular_file(fec) || fec) {
      continue; // the two-hex fan-out directories, and anything else that is not a blob file
    }
    const std::string name = it->path().filename().string();
    if (is_tile_hash(name)) {
      out.push_back(name); // Constraint 4: only a valid tile-hash basename is a tile blob
    }
  }
  return out;
}

expected<std::uint64_t, AssetReaperError>
FilesystemAssetReaper::tile_size(std::string_view hash) const {
  const std::string uri = tile_blob_uri(d_base_uri, hash);
  const std::filesystem::path path{std::string(strip_file_scheme(uri))};
  std::error_code ec;
  const std::uintmax_t size = std::filesystem::file_size(path, ec);
  if (ec) {
    return unexpected(AssetReaperError{AssetReaperError::Kind::EnumerateFailed});
  }
  return static_cast<std::uint64_t>(size);
}

expected<bool, AssetReaperError> FilesystemAssetReaper::remove_tile(std::string_view hash) {
  const std::string uri = tile_blob_uri(d_base_uri, hash);
  const std::filesystem::path path{std::string(strip_file_scheme(uri))};

  // A bare `remove` per blob (Decision 3): unlike a WRITE it needs no temp+rename, because
  // removing a content-addressed blob is already atomic and idempotent -- a re-run simply
  // finds it gone.
  std::error_code ec;
  const bool removed = std::filesystem::remove(path, ec);
  if (ec) {
    return unexpected(AssetReaperError{AssetReaperError::Kind::RemoveFailed});
  }
  if (removed) {
    // Prune the emptied two-hex fan-out directory. `remove` on a NON-empty directory fails
    // with `ec` set, which we deliberately ignore -- a sibling blob keeps the directory.
    std::error_code prune_ec;
    std::filesystem::remove(path.parent_path(), prune_ec);
  }
  return removed;
}

std::optional<std::filesystem::path> FilesystemAssetReaper::asset_path(std::string_view uri) const {
  for (const ResolvedBase& base : d_asset_bases) {
    if (!uri.starts_with(base.authored)) {
      continue;
    }
    const std::string tail{uri.substr(base.authored.size())};
    if (tail.empty()) {
      return std::nullopt;
    }
    // The one irreversible operation in this file gets the one path check: a key that
    // normalizes its way OUT of its own base names something this reaper was never given, and
    // no `..` reaches `remove`.
    // Non-const so the return moves into the optional rather than copying the path
    // (performance-no-automatic-move).
    std::filesystem::path candidate = (base.root / tail).lexically_normal();
    const std::filesystem::path root = base.root.lexically_normal();
    const auto relative = candidate.lexically_relative(root);
    if (relative.empty() || *relative.begin() == "..") {
      return std::nullopt;
    }
    return candidate; // implicitly moved: a non-const local returned as an optional
  }
  return std::nullopt;
}

expected<std::vector<std::string>, AssetReaperError>
FilesystemAssetReaper::list_asset_uris() const {
  std::vector<std::string> out;
  for (const ResolvedBase& base : d_asset_bases) {
    std::error_code ec;
    const bool present = std::filesystem::exists(base.root, ec);
    if (ec) {
      return unexpected(AssetReaperError{AssetReaperError::Kind::EnumerateFailed});
    }
    if (!present) {
      continue; // no such subtree yet: an empty store, not an error (as with tiles)
    }
    std::filesystem::recursive_directory_iterator it(base.root, ec);
    if (ec) {
      return unexpected(AssetReaperError{AssetReaperError::Kind::EnumerateFailed});
    }
    const std::filesystem::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
      if (ec) {
        return unexpected(AssetReaperError{AssetReaperError::Kind::EnumerateFailed});
      }
      std::error_code fec;
      if (!it->is_regular_file(fec) || fec) {
        continue; // directories, and anything else that is not a blob file
      }
      // The key is what a DOCUMENT would author for this file, not where it sits on this
      // machine, so the plan never carries an absolute path and a project stays relocatable.
      const std::filesystem::path relative = it->path().lexically_relative(base.root);
      out.push_back(base.authored + relative.generic_string());
    }
  }
  return out;
}

expected<std::uint64_t, AssetReaperError>
FilesystemAssetReaper::asset_size(std::string_view uri) const {
  const std::optional<std::filesystem::path> path = asset_path(uri);
  if (!path) {
    return unexpected(AssetReaperError{AssetReaperError::Kind::EnumerateFailed});
  }
  std::error_code ec;
  const std::uintmax_t size = std::filesystem::file_size(*path, ec);
  if (ec) {
    return unexpected(AssetReaperError{AssetReaperError::Kind::EnumerateFailed});
  }
  return static_cast<std::uint64_t>(size);
}

expected<bool, AssetReaperError> FilesystemAssetReaper::remove_asset(std::string_view uri) {
  const std::optional<std::filesystem::path> path = asset_path(uri);
  if (!path) {
    return unexpected(AssetReaperError{AssetReaperError::Kind::RemoveFailed});
  }
  std::error_code ec;
  const bool removed = std::filesystem::remove(*path, ec);
  if (ec) {
    return unexpected(AssetReaperError{AssetReaperError::Kind::RemoveFailed});
  }
  if (removed) {
    // Prune an emptied SUBdirectory exactly as the tile fan-out is pruned -- but never the
    // base itself, which the host created and a later save writes into again. `remove` on a
    // non-empty directory fails with `ec` set, which is deliberately ignored.
    const std::filesystem::path parent = path->parent_path().lexically_normal();
    const bool is_base = std::any_of(
        d_asset_bases.begin(), d_asset_bases.end(),
        [&parent](const ResolvedBase& base) { return base.root.lexically_normal() == parent; });
    if (!is_base) {
      std::error_code prune_ec;
      std::filesystem::remove(parent, prune_ec);
    }
  }
  return removed;
}

// ---- The sweep --------------------------------------------------------------------------

expected<GcReport, GcError> sweep_tile_store(const std::unordered_set<std::string>& referenced,
                                             AssetReaper& reaper, bool dry_run) {
  const expected<std::vector<std::string>, AssetReaperError> present = reaper.list_tile_hashes();
  if (!present) {
    return unexpected(from_reaper(present.error()));
  }

  GcReport report;
  report.scanned = present->size();
  report.referenced = referenced.size();

  // MARK BEFORE SWEEP (Decision 3): the complete delete set as a pure subtraction, before a
  // single unlink.
  const std::vector<std::string> dead = unreferenced_tiles(referenced, *present);

  // Size the WHOLE plan first, for dry_run and real alike, so a preview reports exactly what a
  // commit reclaims.
  std::uint64_t bytes = 0;
  for (const std::string& hash : dead) {
    const expected<std::uint64_t, AssetReaperError> size = reaper.tile_size(hash);
    if (!size) {
      return unexpected(from_reaper(size.error())); // fail-safe: nothing deleted yet
    }
    bytes += *size;
  }
  report.deleted = dead.size();
  report.bytes_reclaimed = bytes;

  if (dry_run) {
    return report; // preview only: the on-disk set is untouched
  }

  for (const std::string& hash : dead) {
    const expected<bool, AssetReaperError> removed = reaper.remove_tile(hash);
    if (!removed) {
      return unexpected(from_reaper(removed.error())); // a strict subset of orphans removed
    }
  }
  return report;
}

expected<GcReport, GcError> sweep_asset_store(const GcRoots& roots, AssetReaper& reaper,
                                              bool dry_run) {
  // The tile half first, unchanged and whole -- it validates its own plan before unlinking, so
  // a named-asset enumeration failure below can only ever mean "the tiles were reclaimed and
  // the images were not", never a half-applied plan within either store.
  expected<GcReport, GcError> report = sweep_tile_store(roots.tiles, reaper, dry_run);
  if (!report) {
    return report;
  }

  const expected<std::vector<std::string>, AssetReaperError> present = reaper.list_asset_uris();
  if (!present) {
    return unexpected(from_reaper(present.error()));
  }
  report->scanned += present->size();
  report->referenced += roots.assets.size();

  // MARK BEFORE SWEEP, as above: the complete delete set as a pure subtraction, sized in full,
  // before a single unlink.
  const std::vector<std::string> dead = unreferenced_assets(roots.assets, *present);
  std::uint64_t bytes = 0;
  for (const std::string& uri : dead) {
    const expected<std::uint64_t, AssetReaperError> size = reaper.asset_size(uri);
    if (!size) {
      return unexpected(from_reaper(size.error())); // fail-safe: no named asset deleted yet
    }
    bytes += *size;
  }
  report->deleted += dead.size();
  report->bytes_reclaimed += bytes;

  if (dry_run) {
    return report;
  }
  for (const std::string& uri : dead) {
    const expected<bool, AssetReaperError> removed = reaper.remove_asset(uri);
    if (!removed) {
      return unexpected(from_reaper(removed.error()));
    }
  }
  return report;
}

// ---- The convenience directory entry ----------------------------------------------------

expected<GcReport, GcError> gc_project_directory(const std::filesystem::path& project_dir,
                                                 bool dry_run) {
  GcRoots referenced;

  std::error_code ec;
  const bool present = std::filesystem::exists(project_dir, ec);
  if (ec || !present) {
    return unexpected(
        from_mark(mark_error(ReaderError::Kind::MalformedJson, project_dir.string())));
  }
  std::filesystem::directory_iterator it(project_dir, ec);
  if (ec) {
    return unexpected(
        from_mark(mark_error(ReaderError::Kind::MalformedJson, project_dir.string())));
  }
  const std::filesystem::directory_iterator end;
  for (; it != end; it.increment(ec)) {
    if (ec) {
      return unexpected(
          from_mark(mark_error(ReaderError::Kind::MalformedJson, project_dir.string())));
    }
    std::error_code fec;
    if (!it->is_regular_file(fec) || fec) {
      continue;
    }
    if (it->path().extension() != ".arbc") {
      continue; // only the JSON graphs are roots; the sibling assets/ is not scanned here
    }
    const std::optional<std::string> text = read_file(it->path());
    if (!text) {
      return unexpected(
          from_mark(mark_error(ReaderError::Kind::MalformedJson, it->path().string())));
    }
    const expected<GcRoots, ReaderError> refs = collect_referenced_assets(*text);
    if (!refs) {
      return unexpected(from_mark(refs.error())); // fail-safe: one bad root deletes nothing
    }
    // Constraint 5: the UNION across documents, in both key spaces.
    referenced.tiles.insert(refs->tiles.begin(), refs->tiles.end());
    referenced.assets.insert(refs->assets.begin(), refs->assets.end());
  }

  // Resolve both bases against the directory exactly as a save/load resolves them, so the
  // reaper points at the same `assets/tiles/` a `FilesystemAssetSink` wrote into and the same
  // `assets/images/` a host copies owned image bytes into.
  const std::string base_doc = (project_dir / "project.arbc").string();
  const std::string tiles_base = resolve_uri(base_doc, k_tiles_base);
  FilesystemAssetReaper reaper(
      tiles_base,
      {FilesystemAssetReaper::AssetBase{k_images_base, resolve_uri(base_doc, k_images_base)}});
  return sweep_asset_store(referenced, reaper, dry_run);
}

} // namespace arbc
