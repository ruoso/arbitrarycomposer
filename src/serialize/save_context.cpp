#include <arbc/serialize/load_context.hpp> // normalize_uri / resolve_uri (one seam, both directions)
#include <arbc/serialize/save_context.hpp>

#include <utility>

namespace arbc {

const char* storage_format_token(PixelFormat storage) {
  switch (storage) {
  case PixelFormat::Rgba16fLinearPremul:
    return "rgba16f";
  case PixelFormat::Rgba32fLinearPremul:
    return "rgba32f";
  case PixelFormat::Rgba8Srgb:
    break;
  }
  return "rgba16f"; // not a permitted storage format; the default is the honest answer
}

std::optional<PixelFormat> storage_format_from_token(std::string_view token) {
  if (token == "rgba16f") {
    return PixelFormat::Rgba16fLinearPremul;
  }
  if (token == "rgba32f") {
    return PixelFormat::Rgba32fLinearPremul;
  }
  return std::nullopt;
}

SaveContext::SaveContext(std::string base_uri) : d_base_uri(normalize_uri(base_uri)) {}

expected<bool, AssetSinkError> SaveContext::store_asset(std::string_view relative_uri,
                                                        std::span<const std::byte> bytes) {
  // Params-only (the reader's residual-diff re-serialize): the codec is being run for
  // its `params` KEY SET, not for its bytes, and the blobs it would write are the ones
  // the load just read -- already on disk, under exactly these names, by construction.
  // Nothing to store, and no sink required.
  if (d_params_only) {
    return false;
  }
  if (d_asset_sink == nullptr) {
    // A save that would DROP PIXELS is an error, never a silent success (Constraint 5).
    // Sink-less call sites keep working because a document with no asset-bearing content
    // never reaches here.
    return unexpected(AssetSinkError{AssetSinkError::Kind::NoSink});
  }
  return d_asset_sink->put(resolve_uri(d_base_uri, relative_uri), bytes);
}

bool SaveContext::has_asset(std::string_view relative_uri) const {
  if (d_params_only) {
    return true; // by construction: these are the blobs the load just read
  }
  if (d_asset_sink == nullptr) {
    return false; // let the codec reach `store_asset` and get the honest NoSink error
  }
  return d_asset_sink->contains(resolve_uri(d_base_uri, relative_uri));
}

} // namespace arbc
