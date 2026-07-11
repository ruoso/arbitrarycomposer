#pragma once

// runtime.nested_external_ref: the loader that turns a nested content's kind-owned
// `params.ref` URI into a real child composition (doc 05:47-61, doc 08 Principle 3).
//
// Doc 05 says an external child is "the same mechanism PLUS A LOADER; from the
// compositor's perspective there is no difference between an inline child and an external
// one". This is that loader, and Decision 1 is that sentence taken at its word: the child
// is parsed and installed into the HOST document's own `Model`, as an ordinary
// composition, and the `NestedContent` that names it holds a plain child `ObjectId`
// exactly as an in-document one does. "External" is PROVENANCE, not a different runtime
// representation -- so render, audio, aggregate revision, damage routing and tile caching
// are untouched, and `NestedContent::attach`'s single pinned `DocRoot` still resolves the
// child inside it.
//
// Names no JSON type (`CodecTable` is forward-declared, exactly as `document_serialize.hpp`
// does), so it rides the runtime PUBLIC headers.

#include <arbc/base/ids.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/model/model.hpp>
#include <arbc/serialize/load_context.hpp>
#include <arbc/serialize/reader.hpp> // ContentSink

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>

namespace arbc {

class CodecTable; // serialize-internal (names nlohmann::json): forward-declared only

// The load-time recursion bound (Decision 5). Dedup already bounds every CYCLE -- a
// back-edge resolves to an in-flight composition and re-enters nothing -- but it cannot
// bound a hostile ACYCLIC chain of ten thousand distinct files, which a fuzz corpus or a
// malicious project directory can produce and which would overflow the C++ stack.
// `08-serialization#loader-never-faults-on-hostile-input` forbids that, so the chain is
// capped. Exceeding the cap makes the reference unavailable -- a null child and a
// preserved `ref`, like a missing file (doc 05:50-52), never a parent-load failure.
inline constexpr std::size_t k_external_ref_depth_cap = 64;

// Resolves a nested content's authored `params.ref` to a child composition in `into`,
// loading the referenced `.arbc` through the `LoadContext`'s `AssetSource` and
// deduplicating by RESOLVED identity so two references to one file yield ONE child
// composition -- which is what makes doc-05's shared-content semantics (one content
// identity, one set of warm tile caches) survive persistence.
//
// Owned by one load, driven on one thread: `LoadContext` is "single-writer, not thread
// safe: a load runs on one thread" (`load_context.hpp:47-48`), and this inherits that. It
// is not a shared service and is not stored on the `Document`.
class ExternalCompositionLoader {
public:
  // `into`, `registry`, `codecs` and `sink` are the host load's own -- the child's
  // contents are sunk into the HOST document through the same sink, so they are ordinary
  // resolvable contents of it (Decision 1). All four must outlive this loader.
  ExternalCompositionLoader(Model& into, const Registry& registry, const CodecTable& codecs,
                            const ContentSink& sink);

  // Record `resolved_uri -> composition` without loading anything. The host load seeds
  // its OWN base URI -> its own root composition id here (Constraint 6), so a document
  // that references ITSELF dedups to its own root: a cross-document Droste becomes the
  // in-document Droste, exactly, and needs no separate code path.
  void seed(std::string resolved_uri, ObjectId composition);

  // Resolve `reference` -- authored, relative to `ctx`'s base URI -- and return the child
  // composition's `ObjectId` in `into`. `ObjectId{}` means UNAVAILABLE: no `AssetSource`
  // installed, a missing or unreadable file, bytes that do not parse as an `.arbc`, or a
  // depth-cap overrun. Unavailable is NOT an error (Decision 6): the embedding content
  // keeps its `ref`, renders the placeholder, and the parent load succeeds -- a project
  // must not become unopenable because a widget file moved.
  //
  // Termination is ALLOCATE-BEFORE-PARSE (Decision 5): on first encountering a resolved
  // URI this takes an `ObjectId` from `Model::allocate_id()`, records `URI -> id` in its
  // map, and only THEN parses the child's bytes -- handing that id to `load_composition`
  // as the seeded root. A back-edge (`b.arbc` -> `a.arbc`) therefore hits the map and gets
  // `a`'s IN-FLIGHT root immediately, so the recursion is finite by construction. This is
  // `serialize.compositions_table`'s in-document knot-cut, lifted across documents.
  //
  // Each loaded document gets its OWN `LoadContext` (its own base URI), because a child's
  // relative refs resolve against THE CHILD (doc 08 Principle 3) -- while every context
  // shares this one loader's dedup map, which is keyed on the resolved URI STRING because
  // a `ResolvedRef` is an index into its owning context's table and is not comparable
  // across contexts (`load_context.hpp:19-23`, Constraint 5).
  // The dedup witness is the `AssetSource`'s own request counter, not one kept here: what
  // doc 08 Principle 3 promises is that two references to one file cost ONE FETCH, and the
  // honest place to count fetches is the thing that performs them (doc 16:54-62).
  ObjectId load(LoadContext& ctx, std::string_view reference);

private:
  Model* d_into;
  const Registry* d_registry;
  const CodecTable* d_codecs;
  const ContentSink* d_sink;
  // resolved URI -> child root composition. An entry mapping to an INVALID id is a
  // remembered unavailability (a missing file, unparseable bytes), so a second reference
  // to the same broken target costs no second asset request. A depth-cap overrun is NOT
  // cached: it is a property of WHERE the reference was reached, not of the target.
  std::unordered_map<std::string, ObjectId> d_by_uri;
  std::size_t d_depth{0};
};

} // namespace arbc
