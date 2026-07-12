#pragma once

// Test doubles shared by the runtime's editable-binding tests
// (`editable_binding.t.cpp`, `editable_sink_multiplex.t.cpp`): a counting
// `Editable` content and its non-editable control. They keep the runtime's binding
// tests kind-AGNOSTIC -- the protocol is pinned against a fake facet, so no
// concrete kind enters `runtime`'s dependency closure (doc 17: no
// `runtime -> kind_raster` edge). `tests/raster_runtime_binding.t.cpp` is the same
// wiring proven against the real `org.arbc.raster`.

#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/model/model.hpp>
#include <arbc/model/records.hpp>

#include <atomic>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <optional>

namespace arbc_test {

// A minimal editable content: versions are integers, each with a refcount -- the
// same state lifecycle shape as raster's tile-table store (retain on publish,
// release at record reclaim), shrunk to something a test can count exactly.
//
// Slot indices start at 0 in EVERY instance, so two of these in one document hold
// COLLIDING handles: content A's version {0} and content B's version {0} are
// indistinguishable by value. Only the owning `ObjectId` the state seams now carry
// tells them apart -- which is exactly the property the multiplex must get right,
// and a naive multiplex would not.
//
// THREAD-SAFE, because doc 14:168-181 now says it must be: `retain`/`state_cost`/
// `restore` arrive on the writer and `release` arrives on the drain thread, and since
// `runtime.housekeeping_document_wiring` gave every `Document` a background
// housekeeping thread those are not the same thread. The refcount map is mutex-guarded
// and the behavioral counters are atomic -- the same discipline `RasterStore` already
// keeps (raster_content.hpp:216-219), shrunk to a double. A `FakeEditable` still holds
// only a mutex, so it is a component-test double, not a stress-test one: the
// stress test drives `RasterContent` (see `tests/stress_document_housekeeping.t.cpp`).
class FakeEditable final : public arbc::Content, public arbc::Editable {
public:
  // Per-version byte cost the journal's budget should pick up through the
  // registered cost fn (any non-zero value; the assertion is that it is CONSULTED).
  static constexpr std::size_t k_state_cost = 4096;

  // --- Content ---
  std::optional<arbc::Rect> bounds() const override { return std::nullopt; }
  arbc::Stability stability() const override { return arbc::Stability::Static; }
  std::optional<arbc::TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<arbc::RenderResult> render(const arbc::RenderRequest&,
                                           std::shared_ptr<arbc::RenderCompletion>) override {
    return arbc::RenderResult{};
  }
  arbc::Editable* editable() override { return this; }

  // --- Editable ---
  arbc::StateHandle capture() override { return d_base; }
  void restore(arbc::StateHandle state) override {
    d_base = state;
    restores.fetch_add(1, std::memory_order_relaxed);
  }
  std::size_t state_cost(arbc::StateHandle state) const override {
    costs.fetch_add(1, std::memory_order_relaxed);
    return state.has_state() ? k_state_cost : 0;
  }
  void retain(arbc::StateHandle state) override {
    retains.fetch_add(1, std::memory_order_relaxed);
    const std::lock_guard<std::mutex> lock(d_mutex);
    ++d_refcount[state.slot];
  }
  // The DRAIN-THREAD half of the contract (doc 14:168-181).
  void release(arbc::StateHandle state) override {
    releases.fetch_add(1, std::memory_order_relaxed);
    const std::lock_guard<std::mutex> lock(d_mutex);
    --d_refcount[state.slot];
  }

  // Mint a new version and assign it to `self` under transactional discipline --
  // the shape of `RasterContent::paint` (doc 03:152-158), minus the pixels.
  void edit(arbc::Model::Transaction& txn, arbc::ObjectId self) {
    const arbc::StateHandle after{d_next_slot++};
    d_base = after;
    txn.set_content_state(self, after);
  }

  arbc::StateHandle base() const { return d_base; }

  // This content's net refcount on one version slot: +1 per retain, -1 per release.
  // Zero for a version no longer pinned, and (crucially) zero rather than negative for
  // one released exactly once per retain.
  int refcount_of(arbc::SlotIndex slot) const {
    const std::lock_guard<std::mutex> lock(d_mutex);
    const auto it = d_refcount.find(slot);
    return it == d_refcount.end() ? 0 : it->second;
  }

  // Behavioral counters (doc 16: counters, never wall-clock). Atomic: `releases` is
  // bumped on the drain thread while the writer bumps the others.
  std::atomic<int> retains{0};
  std::atomic<int> releases{0};
  std::atomic<int> restores{0};
  mutable std::atomic<int> costs{0};

  // Every facet call this content saw, in total: the witness that a seam call for
  // SOME OTHER content never landed here.
  int touches() const {
    return retains.load(std::memory_order_relaxed) + releases.load(std::memory_order_relaxed) +
           restores.load(std::memory_order_relaxed) + costs.load(std::memory_order_relaxed);
  }

private:
  arbc::StateHandle d_base{};
  arbc::SlotIndex d_next_slot{0};

  mutable std::mutex d_mutex; // guards d_refcount (writer retains vs drain releases)
  std::map<arbc::SlotIndex, int> d_refcount;
};

// A leaf content with no editable facet (the `org.arbc.tone`/`org.arbc.solid`
// shape): the inert-path control.
class InertContent final : public arbc::Content {
public:
  std::optional<arbc::Rect> bounds() const override { return std::nullopt; }
  arbc::Stability stability() const override { return arbc::Stability::Static; }
  std::optional<arbc::TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<arbc::RenderResult> render(const arbc::RenderRequest&,
                                           std::shared_ptr<arbc::RenderCompletion>) override {
    return arbc::RenderResult{};
  }
};

} // namespace arbc_test
