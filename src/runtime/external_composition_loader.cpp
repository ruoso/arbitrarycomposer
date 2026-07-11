#include <arbc/runtime/external_composition_loader.hpp>
#include <arbc/serialize/codec.hpp> // CodecTable (internal; names nlohmann::json)
#include <arbc/serialize/reader.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace arbc {

ExternalCompositionLoader::ExternalCompositionLoader(Model& into, const Registry& registry,
                                                     const CodecTable& codecs,
                                                     const ContentSink& sink)
    : d_into(&into), d_registry(&registry), d_codecs(&codecs), d_sink(&sink) {}

void ExternalCompositionLoader::seed(std::string resolved_uri, ObjectId composition) {
  d_by_uri.insert_or_assign(std::move(resolved_uri), composition);
}

ObjectId ExternalCompositionLoader::load(LoadContext& ctx, std::string_view reference) {
  // The depth check comes FIRST, and its result is deliberately not cached: a chain that
  // is too deep here might be reachable within the cap elsewhere, so unavailability at
  // the cap is a property of the path, not of the target. Checking before the map is also
  // what keeps a hostile chain from costing an asset request per link past the cap.
  if (d_depth >= k_external_ref_depth_cap) {
    return ObjectId{};
  }

  // The resolution choke point (serialize.reader Constraint 6): base-URI joining and
  // canonicalization happen in `LoadContext`, never here. `resolved_uri` is therefore a
  // canonical identity -- `child.arbc` and `./child.arbc` arrive as one string -- and it
  // is base-INDEPENDENT, which is why the map is keyed on it rather than on the
  // `ResolvedRef` (an index into the owning context's table, Constraint 5).
  const ResolvedRef ref = ctx.resolve(reference);
  const std::string uri = ctx.resolved_uri(ref);
  if (const auto it = d_by_uri.find(uri); it != d_by_uri.end()) {
    return it->second; // dedup, a cycle back-edge, or a remembered unavailability
  }

  // ALLOCATE BEFORE PARSE (Decision 5). `Model::allocate_id()` is a bare monotonic
  // counter bump that installs no record, so this mutates no `DocState` -- and recording
  // it NOW is what cuts the cross-document knot: while the child's bytes are being
  // parsed, a back-edge to this very URI finds the id already here and resolves to the
  // in-flight composition instead of re-loading it.
  const ObjectId child = d_into->allocate_id();
  d_by_uri.emplace(uri, child);

  std::string bytes;
  ctx.load_asset(ref, [&bytes](std::string_view b) { bytes.assign(b.begin(), b.end()); });
  if (bytes.empty()) {
    // No `AssetSource` installed, or the source reported absence. Remember the
    // unavailability so a second reference to the same broken target costs no second
    // request (Decision 6).
    d_by_uri[uri] = ObjectId{};
    return ObjectId{};
  }

  // A document's references resolve relative to THAT document (doc 08 Principle 3), so
  // the child gets its OWN `LoadContext` with its own base URI -- a single context
  // spanning the load tree would resolve `b.arbc`'s siblings against `a.arbc`'s directory,
  // breaking any real project directory. The asset source is shared: it is the host's
  // one way of reaching bytes.
  LoadContext child_ctx{uri};
  child_ctx.set_asset_source(ctx.asset_source());

  ++d_depth;
  const expected<ObjectId, ReaderError> installed =
      load_composition(bytes, *d_registry, *d_codecs, child_ctx, *d_sink, *d_into, child);
  --d_depth;

  if (!installed || !installed->valid()) {
    // Bytes that are not a valid `.arbc`, or that hold no root composition. Unavailable,
    // not a parent-load failure: the `ref` survives, the placeholder renders, and a user
    // who fixes the file and reloads gets their scene back (Decision 6).
    d_by_uri[uri] = ObjectId{};
    return ObjectId{};
  }
  return *installed;
}

} // namespace arbc
