#pragma once

#include <arbc/arbc_api.h>
#include <arbc/base/expected.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/contract/content.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace arbc {

class Backend; // L2 seam, reached through contract's transitive closure (the
               // fade_content.hpp precedent); borrowed by value here, never named
               // beyond the reference.

// Why a registry mutation could not be performed. Errors are values across the
// plugin boundary (doc 03:177-180) -- never exceptions.
enum class RegistryError {
  EmptyId,     // the kind id was empty
  DuplicateId, // a kind with this id is already registered
};

// The human-facing metadata a kind advertises beside its factory (doc 03
// §Registry: human name, version, capability flags). Deliberately minimal for
// the v1 seam; capability flags arrive with the kinds that first need them.
struct KindMetadata {
  std::string human_name;
  std::string version;
};

// The config handed to a factory when instantiating a kind. For the v1 stage-1
// seam this is an opaque, kind-defined source locator (doc 03:164-171); a
// serialization format (doc 08) gives it structure later. `org.arbc.imageseq`
// interprets it as the directory holding its numbered frames.
using ContentConfig = std::string_view;

// A kind factory: constructs a fresh Content from a config, or returns an error
// value (a missing/corrupt source is a value, never a throw -- doc 03:177-180).
using ContentFactory =
    std::function<expected<std::unique_ptr<Content>, std::string>(ContentConfig)>;

// The JSON-free serialize codec a plugin may register beside its factory
// (`runtime.plugin_operator_registration`, doc 03 §Registry, doc 08 Principle 1).
// The hooks traffic in JSON-object TEXT, never a JSON type, so the JSON library
// never enters a plugin's link surface (doc 17 §The codec line): `arbc_serialize`
// wraps them into its JSON-typed codec table, owns the canonical form (parse on
// save, canonical dump handed to `deserialize` on load), and the core keeps
// writing `kind`/`kind_version`/`inputs`/`composition` itself -- a codec authors
// only its own `params` grammar. Errors are values on the factory's own error
// channel (doc 03:177-180); an unparseable serialize result or a failed
// deserialize becomes a serialize/reader error value core-side, never a throw.
//
// No `LoadContext`/`SaveContext` access in v1: a kind whose codec must reach
// asset I/O stays runtime-side (the `org.arbc.image` worked example, doc
// 17 §The codec line) until `runtime.plugin_codec_asset_context`.
struct KindCodec {
  // The persistent producer version the core writes beside `kind` (doc 08).
  std::string kind_version;
  // The kind's live content -> its `params` as JSON-object text. The core parses
  // and canonicalizes whatever this emits, so plugin formatting never perturbs
  // the byte-exact document form.
  std::function<expected<std::string, std::string>(const Content&)> serialize;
  // The canonical `params` text + the already-reconstructed, CORE-OWNED input
  // edges (doc 08 Principle 6) + the resolved child composition (`ObjectId{}`
  // for every kind but a nesting one) -> a live content. Input arity is
  // validated HERE (the operator_codecs idiom): wrong arity is an error value
  // and the model stays unmutated.
  std::function<expected<std::unique_ptr<Content>, std::string>(
      std::string_view params_text, std::span<const ContentRef> inputs, ObjectId composition)>
      deserialize;
};

// The render-time services a registry-carried operator binder borrows for one
// content (`runtime.plugin_operator_registration` Decision 3): the
// contract-expressible subset of runtime's `BindContext`. Kinds needing more
// (composition resolution, the pinned document root) remain host-bound -- the
// built-in `org.arbc.nested` path; widening this struct later is additive.
// Every field is BORROWED for the life of the binding scope and no longer.
struct OperatorBindServices {
  PullService& pull;
  Backend& backend;
};

// A per-kind attach/detach thunk pair a plugin may register beside its factory,
// mirroring runtime's `OperatorBinder` (plain function pointers, typed match by
// `dynamic_cast` inside the thunk -- the plugin's own TU names the concrete
// type, so the match works even for a type defined wholly inside the module).
// `try_attach` injects `services` IFF `content` is this binder's kind and
// returns true; a non-matching content is left untouched. `detach` clears the
// borrowed pointers a prior matched `try_attach` set (precondition: `content`
// is this binder's kind, so it may `static_cast`).
struct KindBinder {
  bool (*try_attach)(Content& content, const OperatorBindServices& services);
  void (*detach)(Content& content) noexcept;
};

// A per-kind state-slab reachability walker (model.persistent_state_walk_hook,
// issue #5). On a workspace fast-reopen the model's recovery walk collects every
// reachable non-inert content `StateHandle` but cannot descend a kind-owned state
// slab: it holds only the opaque `slot`, and levelization forbids the model naming
// the kind. The runtime replays the collected handles here so the kind's
// DOCUMENT-LEVEL state store rebuilds the slab refcounts a persisted handle keeps
// reachable -- the recovery twin of the writer-owned `StateRefSink` retain/release
// seam. `store` is the kind's own document-level state store, type-erased across
// the registry boundary (mirroring `KindBinder`'s static-thunk idiom): the thunk,
// defined in the kind's TU which names the concrete type, `static_cast`s it.
// `content` is the owning object id and `handle` the persisted, non-inert handle.
// Registered on the SAME atomic `add` as the factory, so a plugin cannot decorate
// another kind's registration post-hoc.
struct KindStateWalker {
  void (*reach)(void* store, ObjectId content, StateHandle handle);
};

// id -> factory + metadata (doc 03 §Registry). Reverse-DNS kind ids are the
// persistent contract a future serialization format references, so they are
// part of the contract from day one even though serialization is deferred.
//
// This is the minimal seam the `extern "C" arbc_plugin_register` entry point
// registers into (doc 03:164-171). The production host-side loader -- explicit
// host registration API, opt-in `ARBC_PLUGIN_PATH` directory scan, error
// plumbing across the boundary -- is `runtime.plugin_loading`'s deliverable
// (M8), built on top of this seam, not part of it.
class ARBC_API Registry {
public:
  Registry() = default;

  // Register a kind. Returns success, or an error value if the id is empty or
  // already registered -- never throws (boundary discipline, doc 03:177-180).
  // The optional codec/binder ride the SAME call as the factory: an entry is
  // atomic, so a plugin cannot decorate another plugin's kind post-hoc
  // (`runtime.plugin_operator_registration` Constraint 4). Both default absent
  // -- a factory-only registration stays exactly what it was: its kind
  // round-trips as a placeholder (doc 08 Principle 2) and is attach-free.
  expected<std::monostate, RegistryError> add(std::string_view id, ContentFactory factory,
                                              KindMetadata metadata = {},
                                              std::optional<KindCodec> codec = std::nullopt,
                                              std::optional<KindBinder> binder = std::nullopt,
                                              std::optional<KindStateWalker> state_walker = std::nullopt);

  // Look up a factory by id; nullptr if absent.
  const ContentFactory* factory(std::string_view id) const;

  // Look up metadata by id; nullptr if absent.
  const KindMetadata* metadata(std::string_view id) const;

  // Look up the kind codec by id; nullptr if the kind is absent or registered
  // factory-only. Read lock-free post-load, exactly as `factory` (doc 03:267-270).
  const KindCodec* codec(std::string_view id) const;

  // Look up the operator binder by id; nullptr if absent or factory-only.
  const KindBinder* binder(std::string_view id) const;

  // Look up the per-kind state-slab walker by id; nullptr if the kind is absent or
  // registered without one (every kind today -- the recovery replay skips a kind
  // with no walker). Read lock-free post-load, exactly as `factory` (doc 03:267-270).
  const KindStateWalker* state_walker(std::string_view id) const;

  // Number of registered kinds.
  std::size_t size() const noexcept { return d_entries.size(); }

  // All registered ids, in registration order -- the entry-enumeration surface:
  // paired with `codec(id)`/`binder(id)` it is how the runtime codec-table
  // assembly appends every registry-carried codec after the built-ins
  // (`runtime.plugin_operator_registration` Decision 5).
  std::vector<std::string_view> ids() const;

private:
  struct Entry {
    std::string id;
    ContentFactory factory;
    KindMetadata metadata;
    std::optional<KindCodec> codec;
    std::optional<KindBinder> binder;
    std::optional<KindStateWalker> state_walker;
  };
  const Entry* find(std::string_view id) const;

  std::vector<Entry> d_entries;
};

} // namespace arbc
