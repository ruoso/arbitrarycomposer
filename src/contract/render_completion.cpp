#include <arbc/contract/content.hpp>

#include <utility>

namespace arbc {

// Claim the single settle slot, publish `settlement`, and return whether this
// caller won the race. A second settle (`complete`-after-`complete`,
// `fail`-after-`complete`, or any settle after `take`) loses the CAS and is
// ignored -- the first settlement stands.
template <class Result>
bool Completion<Result>::try_settle(expected<Result, RenderError> settlement) {
  int expected_state = k_pending;
  if (!d_state.compare_exchange_strong(expected_state, k_claimed, std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
    return false; // already claimed/published/taken: ignore
  }
  d_payload.emplace(std::move(settlement));
  // Publish the payload: a `take()` that acquires `k_published` sees the write.
  d_state.store(k_published, std::memory_order_release);
  return true;
}

template <class Result> void Completion<Result>::complete(Result result) {
  try_settle(expected<Result, RenderError>(std::move(result)));
}

template <class Result> void Completion<Result>::fail(RenderError error) {
  try_settle(expected<Result, RenderError>(unexpected<RenderError>(error)));
}

template <class Result> bool Completion<Result>::cancelled() const noexcept {
  return d_cancelled.load(std::memory_order_acquire);
}

template <class Result> void Completion<Result>::cancel() noexcept {
  d_cancelled.store(true, std::memory_order_release);
}

template <class Result> bool Completion<Result>::settled() const noexcept {
  const int s = d_state.load(std::memory_order_acquire);
  return s == k_published || s == k_taken;
}

template <class Result> std::optional<expected<Result, RenderError>> Completion<Result>::take() {
  if (d_state.load(std::memory_order_acquire) != k_published) {
    return std::nullopt; // pending, mid-claim, or already taken
  }
  int expected_state = k_published;
  if (!d_state.compare_exchange_strong(expected_state, k_taken, std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
    return std::nullopt; // a concurrent take() drained it first
  }
  // Move the payload out. GCC's static analysis emits a spurious
  // maybe-uninitialized diagnostic for the shared_ptr members of
  // RenderResult inside the optional<expected<...>> here; suppress it.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
  std::optional<expected<Result, RenderError>> out = std::move(d_payload);
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
  d_payload.reset();
  return out;
}

// One template body, both facets (doc 12 Decision 3): the render facet's
// `RenderCompletion` and the audio facet's `AudioCompletion` are the only two
// instantiations, emitted once here so the concurrency-critical logic compiles
// and is TSan-covered exactly once.
template class Completion<RenderResult>;
template class Completion<AudioResult>;

} // namespace arbc
