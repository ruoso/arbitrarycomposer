#pragma once

// The WRITE-side asset seam (serialize.raster_tile_store Decision 1; doc 08 § The
// asset directory, doc 00 decision record) -- the exact mirror of `LoadContext` /
// `AssetSource`.
//
// The read half has always been complete: `AssetSource::request(resolved_uri,
// on_ready)`, and a `LoadContext` that resolves relative URIs against the document's
// base. The write half did not exist at all -- `SerializeFn` was `(const Content&) ->
// json` with no context, and `serialize_document` returned a string having touched no
// filesystem. That was survivable only while every asset was an IMPORT with a URI to
// re-emit (`org.arbc.image`). Painted pixels have no source file (doc 08 Principle 8),
// so the content-addressed tile store has bytes it must actually put somewhere.
//
// Doc 08 Principle 3's rule is symmetric and this is the half that says so: THE CORE
// WRITES ASSET BYTES; THE KIND ONLY ENCODES THEM. A codec hands finished bytes to the
// sink under a relative URI and never opens, creates, or renames a file. That is what
// keeps the format testable without a disk and hostable somewhere other than a POSIX
// directory -- and it is why the codec does not simply RETURN its blobs, which would
// buffer every tile of the document in memory (1.4 GB for doc 08's reference
// composition) before a byte reached storage, relocating the dense-flatten cost
// Principle 8 exists to avoid rather than removing it.
//
// Names no JSON type and no zstd type, so it rides the component's PUBLIC headers: a
// host installs a sink, and a test drives one, without ever seeing nlohmann.

#include <arbc/arbc_api.h>
#include <arbc/base/expected.hpp>
#include <arbc/media/pixel_format.hpp> // PixelFormat (the storage format, Decision 4)

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace arbc {

// The `arbc.storage_format` token for a permitted storage format (Decision 4).
// `Rgba8Srgb` is not a permitted storage format and has no token.
ARBC_API const char* storage_format_token(PixelFormat storage);

// The inverse: `"rgba16f"` / `"rgba32f"`, and `nullopt` for anything else -- which the
// reader turns into a clean `ReaderError` rather than silently falling back to a default
// and storing the user's pixels at a precision they did not author.
ARBC_API std::optional<PixelFormat> storage_format_from_token(std::string_view token);

// A sink write that could not happen. Errors are values (doc 10): a full disk, an
// unwritable directory, or no sink installed at all surfaces here and rides back out
// through `SerializeError`, never as a throw and never as a silent success.
struct AssetSinkError {
  enum class Kind {
    NoSink,      // no `AssetSink` installed, and the document has bytes to store
    WriteFailed, // the sink could not durably write the bytes (I/O, permissions, space)
  };
  Kind kind{Kind::NoSink};

  friend bool operator==(const AssetSinkError&, const AssetSinkError&) = default;
};

// The write-side mirror of `AssetSource` (Decision 1).
//
// WRITE-IF-ABSENT is the contract, not an optimization: the store is content-addressed,
// so the same name always means the same bytes, and "already present" is therefore
// sufficient proof that the write is unnecessary. `put` reports which happened, and
// that report -- surfaced as `blobs_written()` -- is what turns doc 08 Principle 8's
// incremental save from an aspiration into an observable behavioral counter rather
// than a wall-clock claim (doc 16 tier 4: "wall-clock tests lie in CI; counters don't").
//
// A sink NEVER DELETES. Another document version, another `.arbc`, or a concurrent
// editor may reference a blob this document no longer does, and an incremental save
// cannot know. Reclaiming unreferenced blobs is an explicit user-driven sweep
// (`serialize.asset_gc`), never a side effect of saving.
class ARBC_API AssetSink {
public:
  virtual ~AssetSink() = default;

  // Store `bytes` under `resolved_uri` if absent. Returns `true` when bytes were
  // actually written, `false` when the name was already present (and the bytes were
  // therefore not re-written). A write that cannot be made durable is an
  // `AssetSinkError`, never a partially-written blob under a valid name.
  virtual expected<bool, AssetSinkError> put(std::string_view resolved_uri,
                                             std::span<const std::byte> bytes) = 0;

  // Is `resolved_uri` already present? A pure probe -- no bytes, no write.
  //
  // `put` alone is enough to make the WRITES incremental, but not the WORK: a codec that
  // must hand `put` the bytes before learning they were unnecessary has already paid to
  // produce them, and for a tile store that means compressing the entire document on
  // every save. That is precisely the "incremental save" that is a lie -- the right blob
  // count over a linear-in-document-size CPU cost. So the sink can be ASKED first, and an
  // untouched tile costs neither a hash (the memo) nor a compression (this).
  virtual bool contains(std::string_view resolved_uri) const = 0;

  // Behavioral counter (doc 16 tier 4): distinct names newly written by this sink.
  // A re-save after one dab must advance it by exactly the number of touched tiles.
  virtual std::uint64_t blobs_written() const noexcept = 0;
};

// The write-side mirror of `LoadContext` (Decision 1): the document's base URI, the
// installed sink, and the document-scoped storage format.
//
// THE STORAGE FORMAT IS DOCUMENT-SCOPED, NOT LAYER-SCOPED (Decision 4). Doc 08's prose
// says "document-carried" while its JSON example once showed a `format` inside a
// raster layer's `params`; the two are not equivalent and dedup forces the choice. The
// content hash is over STORAGE-FORMAT bytes, so two layers storing at different
// formats hash the same pixels to different names and the cross-layer dedup Principle 8
// is built on quietly stops working. One storage format per asset directory is what
// makes the store a store. It rides the `arbc` meta block, defaults to `rgba16f`, and
// stays authored rather than inferred -- the lossy/lossless call is the user's.
// (Note the name: a composition's `format` is its WORKING space, doc 07. Different
// concept, different key.)
class ARBC_API SaveContext {
public:
  SaveContext() = default;
  explicit SaveContext(std::string base_uri);

  // The document's own URI: what every relative asset reference resolves against.
  const std::string& base_uri() const noexcept { return d_base_uri; }

  void set_asset_sink(AssetSink* sink) noexcept { d_asset_sink = sink; }
  AssetSink* asset_sink() const noexcept { return d_asset_sink; }

  // The permitted values are `Rgba16fLinearPremul` (the default: lossy from an
  // `rgba32f` working space, ample for 8-bit-origin content) and
  // `Rgba32fLinearPremul` (lossless). The reader rejects any other spelling.
  void set_storage_format(PixelFormat format) noexcept { d_storage_format = format; }
  PixelFormat storage_format() const noexcept { return d_storage_format; }

  // PARAMS-ONLY mode: run the codecs for their `params` KEY SET, not for their bytes.
  //
  // This exists for exactly one caller, and it is not the save path. The unknown-field
  // machinery recovers a known kind's unpreserved `params` interiors by re-running that
  // kind's OWN `SerializeFn` at LOAD time and keeping `params_in - params_out`
  // (`codec.hpp`, serialize.unknown_field_preservation Decision 4). With the seam
  // widened, that re-run needs a `SaveContext` -- and handing it a real one would make
  // every load re-encode and re-write every tile it had just read back.
  //
  // In params-only mode `store_asset` succeeds and stores nothing, because the blobs
  // it would write are the ones we just loaded: they are already on disk, under exactly
  // these names, by construction. An asset-writing codec skips its encode entirely and
  // emits only the names. So the residual diff keeps working for asset-bearing kinds --
  // an unknown `params` key on a raster layer survives a round-trip (doc 08 Principle 4)
  // -- and costs nothing.
  void set_params_only(bool params_only) noexcept { d_params_only = params_only; }
  bool params_only() const noexcept { return d_params_only; }

  // Resolve `relative_uri` against `base_uri()` and write-if-absent through the sink.
  // Returns whether bytes were written (always `false` in params-only mode, which
  // stores nothing). With no sink installed and not in params-only mode, this is
  // `AssetSinkError::Kind::NoSink` -- a save that would DROP PIXELS is an error, never
  // a silent success.
  expected<bool, AssetSinkError> store_asset(std::string_view relative_uri,
                                             std::span<const std::byte> bytes);

  // Is the asset at `relative_uri` already stored? Lets a codec skip PRODUCING bytes it
  // would not have written -- which is what makes the encode incremental and not just the
  // write. Always `true` in params-only mode (the blobs are the ones we just loaded), and
  // `false` with no sink (so the codec proceeds to `store_asset` and gets the honest
  // `NoSink` error rather than silently emitting names for blobs that are nowhere).
  bool has_asset(std::string_view relative_uri) const;

private:
  std::string d_base_uri;
  AssetSink* d_asset_sink{nullptr};
  PixelFormat d_storage_format{PixelFormat::Rgba16fLinearPremul};
  bool d_params_only{false};
};

} // namespace arbc
