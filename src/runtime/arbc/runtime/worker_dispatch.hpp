#pragma once

#include <arbc/arbc_api.h>
#include <arbc/compositor/pull_service.hpp> // RenderDispatch (L5->L4, doc 17:56 vs 60)
#include <arbc/runtime/worker_pool.hpp>     // WorkerPool

namespace arbc {

// The one worker-backed `RenderDispatch` in the tree (doc 02 § Threading model,
// "Worker dispatch is leaf-only"; doc 00 § decision record).
//
// A driver that wants parallel miss-filling obtains its dispatch here and
// nowhere else. The helper decides, per render, whether the content may go to a
// worker at all:
//
//   * A LEAF content (empty `inputs()`) is submitted to `pool`. Its `render`
//     touches nothing but its own caller-owned target surface, so it is a
//     self-contained unit of work and fans out freely.
//   * An OPERATOR content (`is_operator`: non-empty `inputs()` -- a fade, a
//     crossfade, a nested composition, doc 13) is rendered INLINE on the calling
//     driver thread. Its `render` re-enters the `PullService` to fetch its inputs
//     (doc 13:69-71), and a `pull` probes and inserts into the `TileCache`
//     (`pull_service.cpp:223,311`) and walks the service's own descent depth
//     (`pull_service.hpp:208-213`) -- both render-thread-confined. Handing such a
//     render to a worker would race that worker's probes and inserts against the
//     driver thread's own and against `poll_refinements`, breaking the
//     single-writer invariant the pool relies on ("workers never touch the
//     cache", `worker_pool.hpp:37-40`). TSan confirmed that race was latent for
//     all three operator kinds (`kinds.nested_runtime_binding`).
//
// This is a policy layered ABOVE the pool, not a property of it: `WorkerPool`
// stays a general `contract`-only executor with its own claims. It is also not a
// property the *content* can declare -- whether a render may leave the driver
// thread depends on what it re-enters (the core's `PullService`), which the core
// owns and a plugin cannot see. The core knows which contents have inputs; it
// does not ask them.
//
// The rule is enforced structurally, not by convention: `scripts/check_worker_dispatch.py`
// fails the build if a `RenderTask` is submitted anywhere outside this helper's
// translation unit.
//
// The second, independent payoff of the leaf-only rule: because leaves do not
// pull, a dispatched task borrows nothing frame-local. It holds a `Content*`
// (owned by the pinned document), a `RenderRequest` (whose `Surface&` is owned by
// a `PendingTile` retained across frames) and a `RenderCompletion` -- never the
// frame's `PullService&` or its `OperatorBindingScope`. So a render may still be
// in flight when the frame's stack unwinds, which is exactly what the interactive
// driver's deadline-park does.
//
// Lifetime: the returned dispatch borrows `pool` by reference and must not
// outlive it. A driver holds the dispatch only for the duration of a frame, and
// either owns the pool as a member or borrows one the host outlives it with
// (`runtime.shared_worker_pool`; `interactive.hpp`'s borrowing constructor).
//
// `owner` is the OPAQUE submitter tag stamped onto every task this dispatch
// submits (`RenderTask::owner`), and it is REQUIRED: a driver passes its own
// `this`, so that when it dies it can name its own outstanding work to the pool
// (`WorkerPool::drain_owner`) without touching a sibling driver's. It is required
// rather than defaulted because a driver that forgot to name itself would share the
// null tag with every other forgetful driver, and one of them dying would drain the
// others' renders out from under them -- a default here buys convenience and pays
// for it with a silent cross-driver bug.
//
// At `worker_count == 0` the pool is the degenerate inline executor
// (`worker_pool.hpp:57-63`), so this dispatch is byte-identical to
// `direct_dispatch()` -- which is why adopting it is pixel-neutral at the shipped
// configuration.
ARBC_API RenderDispatch worker_backed_dispatch(WorkerPool& pool, const void* owner);

} // namespace arbc
