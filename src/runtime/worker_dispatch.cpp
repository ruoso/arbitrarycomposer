#include <arbc/compositor/operator_graph.hpp> // is_operator (the sole leaf/operator test)
#include <arbc/compositor/pull_service.hpp>   // RenderDispatch, direct_dispatch
#include <arbc/contract/content.hpp>          // Content, RenderRequest, RenderCompletion
#include <arbc/runtime/worker_dispatch.hpp>
#include <arbc/runtime/worker_pool.hpp> // WorkerPool, RenderTask

#include <memory>
#include <utility>

namespace arbc {

RenderDispatch worker_backed_dispatch(WorkerPool& pool, const void* owner) {
  // `is_operator` (`operator_graph.hpp:80-85`) is the whole leaf/operator
  // mechanism in the tree and the helper adds no second classification. A null
  // `content` answers `false` and therefore reaches `submit` -- the pool's own
  // null handling is its business, and this seam does not quietly change it.
  //
  // The child edge is read BESIDE it (issue #29), not as a second classification but as
  // the other half of the same one: a nesting content's `inputs()` are a projection of its
  // child composition's membership, so an empty, unloaded or unresolvable child makes
  // `inputs()` empty while the content still holds -- and dereferences -- the frame's
  // borrowed `DocRoot` pin. Dispatching that to a worker races the binding scope's
  // per-frame detach and can outlive the pin itself. `composition_ref()` is the same
  // contract-level structural edge the damage-routing operator memo admits a nesting layer
  // on (`interactive.cpp`), and for the same reason.
  //
  // `owner` is stamped onto every task and is otherwise untouched: the leaf-only rule
  // decides WHAT may be dispatched, the owner tag records WHO dispatched it, and the
  // two do not interact. Operators still render inline on the driver thread.
  return [&pool, owner, inline_render = direct_dispatch()](Content* content,
                                                           const RenderRequest& request,
                                                           std::shared_ptr<RenderCompletion> done) {
    const bool nesting = content != nullptr && content->composition_ref().valid();
    if (is_operator(content) || nesting) {
      inline_render(content, request, std::move(done));
      return;
    }
    pool.submit(RenderTask{content, request, std::move(done), owner});
  };
}

} // namespace arbc
