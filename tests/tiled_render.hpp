#pragma once

// One exact frame through the SHIPPED tiled driver, for tests that hold a pinned
// `DocRoot` rather than a `Document` (so `render_offline` is not a drop-in).
//
// This replaces the retired untiled `render_frame` (`compositor.render_path_unification`).
// There is now ONE render path in the tree -- the interactive loop, the sequence exporter
// and `render_offline` all run `render_frame_interactive` -- and the only axis that
// separates them is EXACTNESS: `BestEffort` trades fidelity for frame rate, `Exact` takes
// as long as it needs. This helper is the `Exact` end, matching `render_offline` field for
// field: `Deadline::none()` so every miss renders to completion before the composite,
// `prior_revision == nullopt` so no miss is ever served a stale-revision tile, and
// `pending == nullptr` so an async content is driven inline rather than deferred.
//
// `cache` and `pulls` are the CALLER's, deliberately. An operator layer's own `render`
// pulls through the service it was handed at `bind_operators`, and its input tiles land in
// whatever cache that service holds; building a second cache here would leave the layer
// plan probing one store while its operators filled another. Callers that bind nothing and
// pull nothing pass a local cache and `nullptr`.

#include <arbc/base/time.hpp>
#include <arbc/compositor/compositor.hpp>
#include <arbc/compositor/pull_service.hpp>
#include <arbc/compositor/tile_planning.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/runtime/pull_identity.hpp>

#include <cstdint>
#include <functional>
#include <optional>

namespace arbc::testing {

inline void render_once_exact(const DocRoot& state, const ContentResolver& resolve,
                              const Viewport& viewport, TileCache& cache, Backend& backend,
                              SurfacePool& pool, Surface& target,
                              PullServiceImpl* pulls = nullptr) {
  // The per-object revision contribution the layer plan keys on. It must be the SAME
  // function the pull side was configured with, or every pull misses
  // (`model.per_object_revision` Decision 4) -- both are built from this one walk over
  // the pinned graph, exactly as `render_offline` and `SequenceRenderer` build theirs.
  const auto identity_map = build_pull_identity_map(state, resolve);
  const auto stamp_map = build_pull_stamp_map(state, *identity_map);
  const std::function<std::uint64_t(const Content*)> contribution =
      pull_contribution_of(identity_map, stamp_map);
  render_frame_interactive(state, resolve, viewport, cache, backend, pool, target, Deadline::none(),
                           /*prior_revision=*/std::nullopt,
                           /*pending=*/nullptr, /*counters=*/nullptr, /*dirty=*/nullptr,
                           Time::zero(), /*visible_plans=*/nullptr, /*diagnostics=*/nullptr, pulls,
                           Exactness::Exact, /*wanted=*/nullptr, &contribution);
}

} // namespace arbc::testing
