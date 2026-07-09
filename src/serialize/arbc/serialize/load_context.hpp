#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace arbc {

// A stable identity for a resolved reference within one load (doc 08 Principle 3:
// cross-file sharing deduplicates by resolved identity). Two references that
// resolve to the same URI share one `ResolvedRef`; distinct URIs get distinct
// ones. It is an index into the owning `LoadContext`'s resolution table --
// comparable, never a raw string -- so doc-05 shared-content semantics survive
// persistence keyed on resolved-URI identity, matching doc 14:74-83 ("identity is
// guaranteed only where the format references it").
struct ResolvedRef {
  std::size_t index{0};

  friend bool operator==(const ResolvedRef&, const ResolvedRef&) = default;
};

// The async asset-loading hook a kind calls through to fetch the bytes behind a
// resolved reference without blocking the output thread (doc 08:64-66, doc
// 14:213-217). serialize.reader defines the INTERFACE (v1); a filled-in loader
// lands with the kinds that first need it (org.arbc.raster, org.arbc.nested). While
// none is installed on a `LoadContext`, `load_asset` reports the asset as
// unavailable rather than blocking (serialize.reader Decision 4).
class AssetSource {
public:
  virtual ~AssetSource() = default;

  // Begin loading the asset at `resolved_uri`; `on_ready` is invoked with the
  // loaded bytes (empty on failure/absence) once they are available. Non-blocking.
  virtual void request(std::string_view resolved_uri,
                       std::function<void(std::string_view bytes)> on_ready) = 0;
};

// The single resolution / loading choke point a load threads through every kind
// (doc 08:64-66,74-79; serialize.reader Decision 4 + Constraint 6): base-URI
// resolution (v1 resolves relative paths against the document's base URI; the
// scheme dispatch for http / content stores is a stubbed extension point per doc
// 08 Principle 3), an async asset-loading hook, and a resolved-identity dedup
// cache so two references to one resolved URI share a single identity. Owned by
// `arbc::serialize` (Level 4, doc 17:58). Single-writer, not thread-safe: a load
// runs on one thread.
class LoadContext {
public:
  explicit LoadContext(std::string base_uri) : d_base_uri(std::move(base_uri)) {}

  // Resolve `reference` against the base URI and dedup by resolved identity: the
  // same reference (or any reference resolving to the same URI) returns the same
  // `ResolvedRef`; a distinct target returns a distinct one. A reference carrying
  // a scheme ("scheme://...") or an absolute path is taken verbatim (the scheme
  // hook is the later extension point); a relative path is joined onto the base
  // URI's directory.
  ResolvedRef resolve(std::string_view reference);

  // The resolved URI behind a `ResolvedRef` `resolve` handed out.
  const std::string& resolved_uri(ResolvedRef ref) const { return d_resolved[ref.index]; }

  // The document base every relative reference resolves against.
  const std::string& base_uri() const noexcept { return d_base_uri; }

  // Number of distinct resolved identities cached (the dedup witness).
  std::size_t resolved_count() const noexcept { return d_resolved.size(); }

  // Install / read the async asset-loading hook (null == none installed; then
  // `load_asset` reports unavailability). WRITER-THREAD ONLY.
  void set_asset_source(AssetSource* source) noexcept { d_asset_source = source; }
  AssetSource* asset_source() const noexcept { return d_asset_source; }

  // Load the asset behind `ref` through the installed `AssetSource`; when none is
  // installed the continuation fires immediately with empty bytes (unavailable).
  // The seam kinds drive; v1 only forwards.
  void load_asset(ResolvedRef ref, std::function<void(std::string_view bytes)> on_ready);

private:
  std::string resolve_uri(std::string_view reference) const;

  std::string d_base_uri;
  std::vector<std::string> d_resolved;                   // index -> resolved URI
  std::unordered_map<std::string, std::size_t> d_by_uri; // resolved URI -> index (dedup)
  AssetSource* d_asset_source{nullptr};
};

} // namespace arbc
