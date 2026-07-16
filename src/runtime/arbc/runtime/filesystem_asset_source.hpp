#pragma once

// runtime.nested_external_ref: the tree's FIRST `AssetSource` implementation.
//
// `serialize.reader` shipped the `AssetSource` interface and left it deliberately
// unfilled, predicting its own trigger: "a filled-in loader lands with the kinds that
// first need it (org.arbc.raster, org.arbc.nested)" (`load_context.hpp:25-30`). This is
// that loader, for nested. It belongs in `runtime` (L5) because doc 17:60 says so in as
// many words -- runtime's contents are "`Document` (arenas + model + registry +
// LOADERS)" -- and because nothing below L5 may both own a `Document` and touch a file.
//
// Names no JSON type, so it rides the runtime PUBLIC headers: a host installs it, and a
// test drives it, without ever seeing nlohmann.

#include <arbc/arbc_api.h>
#include <arbc/serialize/load_context.hpp>

#include <cstddef>
#include <functional>
#include <string_view>

namespace arbc {

// An `AssetSource` that reads the resolved URI as a filesystem path (a `file://` prefix
// is stripped; anything else is taken as a plain path, since v1 resolves relative paths
// and leaves scheme dispatch as the stubbed extension point -- doc 08 Principle 3).
//
// It fires `on_ready` INLINE, before `request` returns, which the `AssetSource` contract
// permits: non-blocking does not mean must-defer, and a local file read is not a blocking
// wait on a network. That is what lets an external `.arbc` be driven to completion inside
// the load, which is all M8's "external nested projects load" owes. A source that DEFERS
// -- whose `on_ready` fires after the load has returned, installing the child on a later
// revision and damaging the embedding content so the placeholder is replaced live -- is
// `runtime.async_external_load`.
//
// ANY failure -- an absent file, a directory, a permission error, a read fault -- yields
// EMPTY bytes rather than an error channel of its own. The contract already spells absence
// that way (`request`'s "empty on failure/absence"), and the embedding kind already turns
// empty bytes into the doc-05 placeholder. A missing widget file must never make a project
// unopenable (doc 08 Principle 3, as amended).
//
// Single-threaded, like the load it serves (`load_context.hpp:47-48`).
class ARBC_API FilesystemAssetSource final : public AssetSource {
public:
  void request(std::string_view resolved_uri,
               std::function<void(std::string_view bytes)> on_ready) override;

  // Behavioral counters (doc 16:54-62), the dedup witness: how many times the source was
  // asked for bytes at all, and how many of those found a readable file. Two references
  // resolving to one URI must cost exactly ONE request -- that is doc 08 Principle 3's
  // "deduplicates through the loader by resolved identity", asserted as a count rather
  // than as a timing.
  std::size_t requests() const noexcept { return d_requests; }
  std::size_t hits() const noexcept { return d_hits; }

private:
  std::size_t d_requests{0};
  std::size_t d_hits{0};
};

} // namespace arbc
