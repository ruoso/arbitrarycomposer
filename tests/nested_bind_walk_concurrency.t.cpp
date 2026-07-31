// Concurrency / TSan stress for the nested memo's INPUT EDGES (doc 16 tier-6, tsan
// lane). The sibling `nested_concurrency.t.cpp` stresses concurrent *renders* of one
// nested content; this one stresses the other half of the boundary -- the structural
// `Content::inputs()` walks the core runs against a nested content while a SECOND
// binder re-pins it on another thread.
//
// The shape is the real host one. A host that samples a single cell offline
// (`render_offline` on a pinned document) stands up its own pull service and binds the
// whole document, on the UI thread, while the interactive renderer is drawing that
// same document on its own thread. Both bind; the pins differ; `NestedContent`'s
// metadata memo re-keys on the second pin and REBUILDS the vector backing
// `inputs()`. A walker holding the span `inputs()` returned then reads freed storage
// -- the compositor's damage router and aggregate-revision fold both hold one across
// a recursive descent.
//
// The fix is `Content::visit_inputs` (content.hpp): the kind's own storage lock is
// held across the visit, so a re-key cannot free the edges mid-walk. Every core walk
// that can run concurrently with a bind goes through it (`for_each_input`). This test
// pins that: the binder thread churns re-pins while the walker thread routes damage
// and folds aggregate revisions over the same nested content. The assertions are
// OUTCOMES only -- the walks stay well-formed and terminate -- plus TSan's own
// verdict, which is the point of the case. Catch2 macros are main-thread-only, so the
// walker latches its verdict into an atomic (doc 16).
//
// Before the fix this reproduces as:
//   WARNING: ThreadSanitizer: data race
//     Write ... operator delete <- NestedContent::Memo::operator= <- ensure_memo
//                               <- NestedContent::inputs   (binder thread)
//     Read  ... map_damage_up   (walker thread)

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/compositor/operator_graph.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_nested/nested_content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/model/damage.hpp> // Damage (route_operator_damage's result)
#include <arbc/model/model.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using namespace arbc;

// Stateless inline pull, identical in posture to `nested_concurrency.t.cpp`'s: the
// binding needs a `PullService&`, and these walks never issue a render, so it is
// never actually entered. Its only job is to satisfy `attach`.
class InlinePull final : public PullService {
public:
  void pull(ContentRef input, const RenderRequest& request,
            std::shared_ptr<RenderCompletion> done) override {
    if (input == nullptr) {
      done->fail(RenderError::ContentFailed);
      return;
    }
    const std::optional<RenderResult> r = input->render(request, done);
    if (r.has_value()) {
      done->complete(*r);
    }
  }
};

} // namespace

// enforces: 03-layer-plugin-interface#inputs-visit-is-any-thread
TEST_CASE("nested input-edge walks are race-free against a concurrent re-bind") {
  constexpr int k_child_layers = 24;

  Model model;
  SolidContent solid_a{Rgba{0.6F, 0.2F, 0.1F, 0.8F}, Rect{0.0, 0.0, 8.0, 8.0}};
  SolidContent solid_b{Rgba{0.1F, 0.4F, 0.3F, 0.5F}, Rect{0.0, 0.0, 8.0, 8.0}};
  std::unordered_map<ObjectId, Content*> binding;
  ObjectId comp{};
  {
    auto tx = model.transact("scene");
    comp = tx.add_composition(8.0, 8.0);
    // A WIDE child: the hazard is a span held across a descent, so the window scales
    // with the edge count. A two-layer child closes it in nanoseconds and the case
    // passes vacuously even against the unfixed library.
    for (int i = 0; i < k_child_layers; ++i) {
      const ObjectId c = tx.add_content(1);
      tx.attach_layer(comp, tx.add_layer(c, Affine::translation(i * 0.5, i * 0.5)));
      binding[c] = (i % 2 == 0) ? &solid_a : &solid_b;
    }
    tx.commit();
  }

  // TWO distinct pins of the same document, the way two independent binders hold
  // them. The second revision differs by an edit ELSEWHERE in the document, so both
  // snapshots describe the same child membership -- what differs is the REVISION the
  // memo keys on, which is exactly what makes `attach` re-key and rebuild the edge
  // vector. (Identical membership is deliberate: it isolates the storage-lifetime
  // hazard from any question of the edges themselves changing.)
  const DocStatePtr pin_a = model.current();
  {
    auto tx = model.transact("bump");
    tx.add_content(1); // unattached: bumps the revision, touches no composition
    tx.commit();
  }
  const DocStatePtr pin_b = model.current();
  REQUIRE(pin_a->revision() != pin_b->revision());

  CpuBackend backend;
  InlinePull pull;
  NestedContent nested(comp);
  const NestedResolver resolver = [&binding](ObjectId id) -> Content* {
    const auto it = binding.find(id);
    return it != binding.end() ? it->second : nullptr;
  };
  nested.attach(pull, backend, resolver, *pin_a);

  // The walker must see real edges to walk, or the test would pass vacuously.
  REQUIRE(!nested.inputs().empty());

  constexpr int k_rounds = 20000;
  std::atomic<bool> walks_well_formed{true};
  std::atomic<int> walks_done{0};

  // The WALKER: the compositor's two structural `inputs()` descents, the ones the
  // interactive renderer runs every frame -- damage routing (which also folds through
  // `map_input_damage` per edge) and the aggregate-revision fold.
  std::thread walker([&] {
    const OperatorLayer layer{ObjectId{}, &nested};
    const OperatorLayer layers[] = {layer};
    for (int i = 0; i < k_rounds; ++i) {
      const std::vector<Damage> routed = route_operator_damage(
          layers, &solid_a, Rect{0.0, 0.0, 2.0, 2.0}, TimeRange{Time::zero(), Time::zero()});
      // An OUTCOME assertion, not a wall-clock one: solid_a is a reachable input of
      // the nested boundary, so its damage must route up to exactly one operator
      // output, and the mapped rect must be finite. A torn edge read shows up here as
      // a dropped or duplicated route long before it shows up as a crash.
      if (routed.size() != 1) {
        walks_well_formed.store(false, std::memory_order_relaxed);
      }
      const std::uint64_t agg =
          aggregate_revision(&nested, [](const Content*) -> std::uint64_t { return 1; });
      if (agg == 0) {
        walks_well_formed.store(false, std::memory_order_relaxed);
      }
      walks_done.fetch_add(1, std::memory_order_relaxed);
    }
  });

  // The BINDER: a second binder re-pinning the same content, alternating snapshots so
  // every attach re-keys the memo -- and then WALKING the edges it just re-pinned,
  // which is what actually rebuilds the storage. That attach-then-descend order is
  // `bind_operators`' own (`operator_binding.cpp`: a content is attached before the
  // walk recurses into its `inputs()`, so the walk can see through the nesting
  // boundary), and the rebuild happens inside that descent's `ensure_memo` -- not in
  // `attach`, which only marks the memo stale. A binder that re-pinned without
  // walking would free nothing and the case would pass vacuously.
  std::size_t binder_edges = 0;
  for (int i = 0; i < k_rounds; ++i) {
    nested.attach(pull, backend, resolver, (i % 2 == 0) ? *pin_b : *pin_a);
    binder_edges += nested.inputs().size();
  }

  walker.join();

  CHECK(walks_well_formed.load(std::memory_order_relaxed));
  CHECK(walks_done.load(std::memory_order_relaxed) == k_rounds);
  // The binder really did keep seeing the child's two edges across every re-pin.
  CHECK(binder_edges == static_cast<std::size_t>(k_child_layers) * k_rounds);
  // The re-pins really did force recomputes -- otherwise the memo never rebuilt the
  // storage and TSan had nothing to catch.
  CHECK(nested.metadata_recomputes() > 1);

  nested.detach();
}
