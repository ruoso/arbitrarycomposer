#include <arbc/contract/content.hpp>

#include <utility>

namespace arbc {

// Claim the single settle slot, publish `settlement`, and return whether this
// caller won the race. A second settle (`complete`-after-`complete`,
// `fail`-after-`complete`, or any settle after `take`) loses the CAS and is
// ignored -- the first settlement stands.
bool RenderCompletion::try_settle(expected<RenderResult, RenderError> settlement) {
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

void RenderCompletion::complete(RenderResult result) {
  try_settle(expected<RenderResult, RenderError>(std::move(result)));
}

void RenderCompletion::fail(RenderError error) {
  try_settle(expected<RenderResult, RenderError>(unexpected<RenderError>(error)));
}

bool RenderCompletion::cancelled() const noexcept {
  return d_cancelled.load(std::memory_order_acquire);
}

void RenderCompletion::cancel() noexcept { d_cancelled.store(true, std::memory_order_release); }

bool RenderCompletion::settled() const noexcept {
  const int s = d_state.load(std::memory_order_acquire);
  return s == k_published || s == k_taken;
}

std::optional<expected<RenderResult, RenderError>> RenderCompletion::take() {
  if (d_state.load(std::memory_order_acquire) != k_published) {
    return std::nullopt; // pending, mid-claim, or already taken
  }
  int expected_state = k_published;
  if (!d_state.compare_exchange_strong(expected_state, k_taken, std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
    return std::nullopt; // a concurrent take() drained it first
  }
  std::optional<expected<RenderResult, RenderError>> out = std::move(d_payload);
  d_payload.reset();
  return out;
}

} // namespace arbc
