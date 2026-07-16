// arbc/builtin_kinds.hpp -- the L6 umbrella's built-in-kind Registry bootstrap
// (runtime.registry_bootstrap; doc 03 § Registry, doc 17:33/72). Umbrella-owned
// and installed at the include ROOT (<prefix>/include/arbc/builtin_kinds.hpp),
// exactly like arbc/version.hpp and arbc/arbc_api.h: its base dir lives only on
// the umbrella target, so no component can reach up to include it, by
// construction rather than by lint.

#pragma once

#include <arbc/arbc_api.h>
#include <arbc/contract/registry.hpp>

namespace arbc {

// Register the six in-lib kinds -- org.arbc.solid, .tone, .raster, .fade,
// .crossfade, .nested, in that (enumeration) order -- into a host-supplied
// registry, so built-ins and loaded plugins present through ONE `Registry`
// surface instead of two unrelated mechanisms (doc 03 § Registry).
//
// - Factory and metadata ONLY: no bootstrapped entry carries a `KindCodec` or
//   `KindBinder` -- built-in codec/binder transport stays on `runtime`'s
//   explicit tables (doc 08 Principle 1, doc 13) -- so `codec(id)` and
//   `binder(id)` stay nullptr for every bootstrapped id.
// - Skip-on-duplicate and idempotent: an id already present (a host's explicit
//   registration, or a plugin deliberately loaded first) keeps its entry,
//   matching the loader's explicit-registration-first precedence; calling
//   twice neither errors nor grows the registry.
// - Errors are values: every factory returns `unexpected<std::string>` on a
//   bad config and never throws (doc 03:176-183). `org.arbc.fade` and
//   `org.arbc.crossfade` -- operator kinds whose input edges cannot travel
//   `ContentConfig` -- register factories that always return an error value
//   directing construction through document deserialize, so a host still
//   enumerates them and their ids stay occupied under bootstrap-first order.
// - `org.arbc.image` / `org.arbc.imageseq` ship out-of-lib (doc 03 § Reference
//   implementations) and are NOT bootstrapped.
//
// Built-ins enter by direct `Registry::add`; the `extern "C"` plugin entry
// point is never traveled at runtime (doc 17 § Why object libraries).
ARBC_API void register_builtin_kinds(Registry& registry);

} // namespace arbc
