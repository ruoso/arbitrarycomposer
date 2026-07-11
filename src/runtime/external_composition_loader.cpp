#include <arbc/runtime/external_composition_loader.hpp>
#include <arbc/serialize/codec.hpp> // CodecTable (internal; names nlohmann::json)
#include <arbc/serialize/reader.hpp>

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace arbc {

ExternalCompositionLoader::ExternalCompositionLoader(
    Model& into, const Registry& registry, const CodecTable& codecs, const ContentSink& sink,
    const std::shared_ptr<PendingExternalLoads>& state)
    : d_into(&into), d_registry(&registry), d_codecs(&codecs), d_sink(&sink), d_state(state) {}

void ExternalCompositionLoader::seed(std::string resolved_uri, ObjectId composition) {
  d_state->record(std::move(resolved_uri), composition);
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
  if (ObjectId known; d_state->find(uri, known)) {
    // A dedup, a cycle back-edge, a remembered unavailability -- or a reference to a URI
    // whose bytes are still IN FLIGHT, which resolves to the id already minted for it and
    // issues no second request. That last case is what makes a DEFERRING cycle terminate.
    return known;
  }

  // ALLOCATE BEFORE FETCH (Constraint 8), which is `nested_external_ref`'s
  // allocate-before-PARSE rule extended one step: `Model::allocate_id()` is a bare monotonic
  // counter bump that installs no record, so this mutates no `DocState` -- and recording it
  // BEFORE `request()` is issued is what cuts the cross-document knot even when the bytes are
  // late. While the child's bytes are in flight, a back-edge to this very URI finds the id
  // already here and resolves to the in-flight composition instead of re-fetching it.
  const ObjectId child = d_into->allocate_id();
  d_state->record(uri, child);
  d_state->add_pending(child, uri, d_depth);

  // The callback is DURABLE: it may fire on any thread, and it may fire after the `Document`
  // that started the fetch is gone (Constraint 6). So it captures a `weak_ptr` -- never a
  // `Document`, never a `Model`, never this loader, all three of which are stack-scoped -- and
  // does exactly one thing: copy the bytes into the completion queue (Constraint 4).
  const std::weak_ptr<PendingExternalLoads> weak = d_state;
  ctx.load_asset(ref, [weak, child](std::string_view bytes) {
    if (const std::shared_ptr<PendingExternalLoads> state = weak.lock()) {
      state->complete(child, bytes);
    }
  });

  // PENDING vs UNAVAILABLE is "did `on_ready` fire?", never "are the bytes empty?"
  // (Constraint 2). Asking the queue -- rather than a captured stack flag -- is what makes
  // the question race-free against a source that answers from another thread while
  // `request()` is still returning: the queue's mutex orders the two.
  std::string bytes;
  if (!d_state->take_arrival(child, bytes)) {
    // Not yet. The embedding content binds this VALID id, whose composition record simply
    // does not exist yet -- already the doc-05 placeholder -- and the parent load completes
    // at revision 0 without waiting (Constraint 3). `settle` installs it when the bytes land.
    return child;
  }

  // The source answered inside `request()`: the synchronous path, unchanged.
  std::string pending_uri;
  std::size_t depth = 0;
  static_cast<void>(d_state->take_pending(child, pending_uri, depth));
  return install(uri, bytes, d_depth, {});
}

ObjectId ExternalCompositionLoader::settle(ObjectId child, std::string_view bytes,
                                           std::span<const Damage> damage) {
  std::string uri;
  std::size_t depth = 0;
  if (!d_state->take_pending(child, uri, depth)) {
    return ObjectId{}; // already settled (a duplicate arrival), or never pending
  }
  return install(uri, bytes, depth, damage);
}

ObjectId ExternalCompositionLoader::install(const std::string& uri, std::string_view bytes,
                                            std::size_t depth, std::span<const Damage> damage) {
  if (bytes.empty()) {
    // The source ANSWERED, and answered absence: a missing file. Remember the unavailability
    // so a second reference to the same broken target costs no second request (Decision 6).
    d_state->record(uri, ObjectId{});
    return ObjectId{};
  }

  // A document's references resolve relative to THAT document (doc 08 Principle 3), so
  // the child gets its OWN `LoadContext` with its own base URI -- a single context
  // spanning the load tree would resolve `b.arbc`'s siblings against `a.arbc`'s directory,
  // breaking any real project directory. The asset source is shared: it is the host's
  // one way of reaching bytes, and a settle reaches it through the durable state because the
  // `LoadContext` that first carried it is long gone.
  LoadContext child_ctx{uri};
  child_ctx.set_asset_source(d_state->source());

  ObjectId child{};
  static_cast<void>(d_state->find(uri, child));

  // The child's OWN references are reached one link deeper. On the synchronous path that is
  // the live counter; on the deferred path it is the depth restored from the pending entry --
  // which is what keeps a chain that defers at every link capped at the same 64 links
  // (Decision 5). Save and restore rather than increment, so both paths share this line.
  const std::size_t outer = d_depth;
  d_depth = depth + 1;
  const expected<ObjectId, ReaderError> installed =
      load_composition(bytes, *d_registry, *d_codecs, child_ctx, *d_sink, *d_into, child, damage);
  d_depth = outer;

  if (!installed || !installed->valid()) {
    // Bytes that are not a valid `.arbc`, or that hold no root composition. Unavailable,
    // not a parent-load failure: the `ref` survives, the placeholder renders, and a user
    // who fixes the file and reloads gets their scene back (Decision 6).
    d_state->record(uri, ObjectId{});
    return ObjectId{};
  }
  return *installed;
}

} // namespace arbc
