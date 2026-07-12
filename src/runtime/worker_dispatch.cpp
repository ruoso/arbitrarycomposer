#include <arbc/compositor/operator_graph.hpp> // is_operator (the sole leaf/operator test)
#include <arbc/compositor/pull_service.hpp>   // RenderDispatch, direct_dispatch
#include <arbc/contract/content.hpp>          // Content, RenderRequest, RenderCompletion
#include <arbc/runtime/worker_dispatch.hpp>
#include <arbc/runtime/worker_pool.hpp> // WorkerPool, RenderTask

#include <memory>
#include <utility>

namespace arbc {

RenderDispatch worker_backed_dispatch(WorkerPool& pool) {
  // `is_operator` (`operator_graph.hpp:80-85`) is the whole leaf/operator
  // mechanism in the tree and the helper adds no second classification. A null
  // `content` answers `false` and therefore reaches `submit` -- the pool's own
  // null handling is its business, and this seam does not quietly change it.
  return [&pool, inline_render = direct_dispatch()](Content* content, const RenderRequest& request,
                                                    std::shared_ptr<RenderCompletion> done) {
    if (is_operator(content)) {
      inline_render(content, request, std::move(done));
      return;
    }
    pool.submit(RenderTask{content, request, std::move(done)});
  };
}

} // namespace arbc
