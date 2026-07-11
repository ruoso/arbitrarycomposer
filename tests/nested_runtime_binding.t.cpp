// Production runtime binding for org.arbc.nested (kinds.nested_runtime_binding).
// Before this seam nothing in src/runtime/ so much as mentioned NestedContent: every
// one of its ~40 attach() call sites was a test hand-rolling its own resolver map and
// hand-pinning the document, so a nested layer in an exported sequence rendered
// through a null PullService and a nested-of-tones scene drained through ExportMonitor
// mixed silence. This test builds Documents holding an org.arbc.nested content and
// drives them through the REAL offline (SequenceRenderer) and export (ExportMonitor)
// drivers with NO manual attach, asserting byte-exactness against the hand-attached
// oracle, transitive binding through the nested boundary, the memo-stability counter
// the per-frame re-bind must not disturb, and RAII teardown.
//
// CROSS-COMPONENT: it drives the real CpuBackend composite through the runtime
// drivers, so -- like nested_goldens.t.cpp and fade_runtime_binding.t.cpp -- it lives
// in tests/ and links the umbrella `arbc` rather than in src/runtime/t/ (a runtime-
// component test may not include kind_nested/backend_cpu, doc 17 / check_levels.py).

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/compositor/compositor.hpp>
#include <arbc/compositor/pull_service.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_fade/fade_content.hpp>
#include <arbc/kind_nested/nested_content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/kind_tone/tone_content.hpp>
#include <arbc/media/audio_block.hpp>
#include <arbc/media/audio_format.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/model.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/export_monitor.hpp>
#include <arbc/runtime/offline_sequence.hpp>
#include <arbc/runtime/operator_binding.hpp>
#include <arbc/runtime/pull_identity.hpp>
#include <arbc/surface/backend.hpp>
#include <arbc/surface/surface_pool.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

using namespace arbc;

namespace {

constexpr int k_dim = 8;
constexpr std::uint32_t k_rate = 48000;
constexpr std::uint32_t k_frames = 16;

std::int64_t flicks_per_frame() {
  return Time::flicks_per_second / static_cast<std::int64_t>(k_rate);
}

TimeRange audio_window() {
  return TimeRange{Time::zero(), Time{static_cast<std::int64_t>(k_frames) * flicks_per_frame()}};
}

// Fade in over [-1000, 1000), so t = 0 is E = 0.5 -- the same half-blend fade_goldens
// pins. The window straddles zero because the compositor's `render_frame` (the oracle
// below) composes at the walking skeleton's hard-coded Time::zero() (compositor.cpp:64),
// so t = 0 is the instant at which the driver's frame and the oracle are comparable --
// and it must be an instant where the fade actually blends, not one where it is inert.
FadeParams half_fade_params() {
  return FadeParams{FadeShape::Linear, FadeWindow{Time{-1000}, Time{1000}}, std::nullopt};
}

std::vector<std::byte> to_bytes(std::span<const std::byte> s) { return {s.begin(), s.end()}; }

void require_equal(const std::vector<std::byte>& got, const std::vector<std::byte>& want) {
  REQUIRE(got.size() == want.size());
  REQUIRE(std::memcmp(got.data(), want.data(), want.size()) == 0);
}

// A nested content over a child composition of two org.arbc.solid layers, embedded at
// the parent's global root (the offline driver's frame walk). Nothing here attaches:
// that is the production wiring under test.
struct VisualScene {
  Document doc;
  std::shared_ptr<SolidContent> solid_a =
      std::make_shared<SolidContent>(Rgba{0.60F, 0.20F, 0.10F, 0.80F}, Rect{0.0, 0.0, 8.0, 8.0});
  std::shared_ptr<SolidContent> solid_b =
      std::make_shared<SolidContent>(Rgba{0.10F, 0.40F, 0.30F, 0.50F}, Rect{0.0, 0.0, 8.0, 8.0});
  std::shared_ptr<NestedContent> nested;
  ObjectId child{};
  ObjectId layer_a{};

  VisualScene() {
    child = doc.add_composition(8.0, 8.0);
    layer_a = doc.add_layer(doc.add_content(solid_a), Affine::identity());
    const ObjectId layer_b = doc.add_layer(doc.add_content(solid_b), Affine::translation(1.0, 1.0));
    doc.attach_layer(child, layer_a);
    doc.attach_layer(child, layer_b);
    nested = std::make_shared<NestedContent>(child);
    doc.add_layer(doc.add_content(nested), Affine::identity());
  }
};

// The same scene with the bottom child layer wrapped in an org.arbc.fade -- a content
// living INSIDE the nested child composition, the witness that the bind walk sees
// through the nested boundary (Constraint 5). `solid_a` is NOT minted: it is the
// fade's input edge, reached only through `FadeContent::inputs()`.
struct FadeChildScene {
  Document doc;
  std::shared_ptr<SolidContent> solid_a =
      std::make_shared<SolidContent>(Rgba{0.60F, 0.20F, 0.10F, 0.80F}, Rect{0.0, 0.0, 8.0, 8.0});
  std::shared_ptr<SolidContent> solid_b =
      std::make_shared<SolidContent>(Rgba{0.10F, 0.40F, 0.30F, 0.50F}, Rect{0.0, 0.0, 8.0, 8.0});
  std::shared_ptr<FadeContent> fade;
  std::shared_ptr<NestedContent> nested;
  ObjectId child{};

  FadeChildScene() {
    child = doc.add_composition(8.0, 8.0);
    fade = std::make_shared<FadeContent>(solid_a.get(), half_fade_params());
    const ObjectId lf = doc.add_layer(doc.add_content(fade), Affine::identity());
    const ObjectId lb = doc.add_layer(doc.add_content(solid_b), Affine::translation(1.0, 1.0));
    doc.attach_layer(child, lf);
    doc.attach_layer(child, lb);
    nested = std::make_shared<NestedContent>(child);
    doc.add_layer(doc.add_content(nested), Affine::identity());
  }
};

// A child composition in which EVERY member layer is culled, one layer per cull the
// nesting boundary owns: invisible, zero-opacity, a content id the document does not
// hold, and a degenerate (non-invertible) placement. None of the four has any pixels
// to contribute and none ever will, so a bound nested must answer each with the empty
// placeholder -- without pulling across the boundary for it.
struct CulledScene {
  Document doc;
  std::shared_ptr<SolidContent> solid =
      std::make_shared<SolidContent>(Rgba{0.60F, 0.20F, 0.10F, 0.80F}, Rect{0.0, 0.0, 8.0, 8.0});
  std::shared_ptr<NestedContent> nested;
  ObjectId child{};

  CulledScene() {
    child = doc.add_composition(8.0, 8.0);
    const ObjectId content = doc.add_content(solid);
    const ObjectId hidden = doc.add_layer(content, Affine::identity());
    const ObjectId transparent = doc.add_layer(content, Affine::identity(), 0.0);
    // An id no content in the document answers to: the not-yet-loaded / async layer
    // `Document::resolve` reports as nullptr (doc 05:50).
    const ObjectId unresolved = doc.add_layer(ObjectId{9999}, Affine::identity());
    // Zero linear part: no inverse exists, so no device->local map does either (doc 04).
    const ObjectId degenerate = doc.add_layer(content, Affine{0.0, 0.0, 0.0, 0.0, 0.0, 0.0});
    {
      Model::Transaction txn = doc.transact();
      txn.set_visible(hidden, false);
      REQUIRE(txn.commit().has_value());
    }
    for (const ObjectId layer : {hidden, transparent, unresolved, degenerate}) {
      doc.attach_layer(child, layer);
    }
    nested = std::make_shared<NestedContent>(child);
    doc.add_layer(doc.add_content(nested), Affine::identity());
  }
};

// A nested-of-tones scene: a child composition of two org.arbc.tone layers, embedded
// in the parent composition an ExportMonitor drains.
struct AudioScene {
  Document doc;
  std::shared_ptr<ToneContent> tone_a = std::make_shared<ToneContent>(440, 0.5F);
  std::shared_ptr<ToneContent> tone_b = std::make_shared<ToneContent>(660, 0.25F);
  std::shared_ptr<NestedContent> nested;
  ObjectId child{};
  ObjectId parent{};

  AudioScene() {
    child = doc.add_composition(0.0, 0.0);
    doc.attach_layer(child, doc.add_layer(doc.add_content(tone_a), Affine::identity()));
    doc.attach_layer(child, doc.add_layer(doc.add_content(tone_b), Affine::identity()));
    nested = std::make_shared<NestedContent>(child);
    parent = doc.add_composition(0.0, 0.0);
    doc.attach_layer(parent, doc.add_layer(doc.add_content(nested), Affine::identity()));
  }
};

// The live PullService config the drivers build (`offline_sequence.cpp`): a reverse
// Content* -> ObjectId map so each input's tiles key under its identity, and the one
// pinned revision as every node's contribution.
PullConfig live_config(const DocRoot& pin, const ContentResolver& resolve) {
  PullConfig config;
  config.id_of = make_pull_identity_of(pin, resolve);
  const std::uint64_t revision = pin.revision();
  config.contribution = [revision](const Content*) { return revision; };
  return config;
}

// The MANUALLY-ATTACHED reference: hand-pin the document, hand-build the resolver, and
// hand-attach every operator content to a live PullServiceImpl -- exactly what every
// pre-existing nested test does -- then render the frame through the compositor at the
// geometry the offline driver's Viewport{8, 8, identity} frame renders. The driver's
// PRODUCTION-bound frame must reproduce these bytes exactly: binding changes WHO calls
// attach, never WHAT nested computes.
//
// It renders the WHOLE FRAME rather than nested alone because the compositor's frame
// walk is GLOBAL (`for_each_layer`, compositor.cpp:114): it draws every layer record in
// the document, so a child composition's member layers are drawn at top level AND again
// inside nested's composed output. The visual frame walk is not composition-scoped the
// way ExportMonitor's audio mix is (`for_each_layer_in`) -- pre-existing and orthogonal
// to binding (tech debt `compositor.root_composition_frame_walk`). Both sides of this
// comparison carry it, so what the comparison isolates is exactly the binding.
using AttachAll =
    std::function<void(PullService&, Backend&, const ContentResolver&, const DocRoot&)>;

std::vector<std::byte> render_reference(Document& doc, const AttachAll& attach,
                                        const std::function<void()>& detach) {
  CpuBackend backend;
  const DocStatePtr pin = doc.pin();
  const ContentResolver resolve = [&doc](ObjectId id) { return doc.resolve(id); };
  TileCache cache(64U * 1024 * 1024);
  PullServiceImpl service(cache, backend, direct_dispatch(), live_config(*pin, resolve));
  attach(service, backend, resolve, *pin);

  SurfacePool pool(backend);
  const auto target = backend.make_surface(k_dim, k_dim, pin->working_space());
  REQUIRE(target.has_value());
  render_frame(*pin, resolve, Viewport{k_dim, k_dim, Affine::identity()}, backend, pool, **target);
  std::vector<std::byte> bytes = to_bytes((**target).cpu_bytes());
  detach(); // leave the reference scene's contents clean
  return bytes;
}

// The direct-mix oracle for the audio path: one tone rendered straight into a block.
std::vector<float> render_tone(ToneContent& tone) {
  std::vector<float> buf(static_cast<std::size_t>(k_frames) * 2U, 0.0F);
  AudioBlock block{buf.data(), k_frames, ChannelLayout::Stereo, k_rate};
  const AudioRequest req{audio_window(), k_rate,           ChannelLayout::Stereo,
                         block,          Exactness::Exact, StateHandle{}};
  auto done = std::make_shared<AudioCompletion>();
  const std::optional<AudioResult> r = tone.audio()->render_audio(req, done);
  REQUIRE(r.has_value());
  return buf;
}

// Counts the pulls a bound nested content issues for its child layers, honoring each
// inline (the abstract PullService contract, content.hpp:333) so the render still
// completes. Used to drive the boundary-budget counter from the RUNTIME binding rather
// than from a hand-attached fixture.
class CountingPull final : public PullService {
public:
  void pull(ContentRef input, const RenderRequest& request,
            std::shared_ptr<RenderCompletion> done) override {
    ++d_pulls;
    d_all_exact = d_all_exact && request.exactness == Exactness::Exact;
    if (input == nullptr) {
      done->fail(RenderError::ContentFailed);
      return;
    }
    const std::optional<RenderResult> r = input->render(request, done);
    if (r.has_value()) {
      done->complete(*r);
    }
  }

  unsigned pulls() const { return d_pulls; }
  bool all_exact() const { return d_all_exact; }

private:
  unsigned d_pulls{0};
  bool d_all_exact{true};
};

} // namespace

// enforces: 16-sdlc-and-quality#byte-exact-goldens
// enforces: 05-recursive-composition#nested-runtime-bound
// enforces: 05-recursive-composition#nested-renders-through-synthetic-viewport
TEST_CASE("org.arbc.nested renders byte-exact through the offline driver with production binding") {
  VisualScene reference;
  const std::vector<std::byte> expected = render_reference(
      reference.doc,
      [&](PullService& p, Backend& b, const ContentResolver& r, const DocRoot& d) {
        reference.nested->attach(p, b, r, d);
      },
      [&] { reference.nested->detach(); });

  VisualScene scene;
  CpuBackend backend;
  // No manual attach anywhere: the driver's `bind_operators` is the only thing that
  // hands nested its PullService, Backend, resolver and pin. Without this task's
  // wiring, nested's render would trip its attach assertion.
  SequenceRenderer renderer(scene.doc, Viewport{k_dim, k_dim, Affine::identity()}, backend);
  const auto frame = renderer.render_frame_at(Time::zero());
  REQUIRE(frame.has_value());
  require_equal(to_bytes((**frame).cpu_bytes()), expected);
  // Nested obtained every child layer through the live PullServiceImpl (a bypass would
  // assert or yield different bytes).
  CHECK(renderer.counters().requests_issued() >= 1U);
}

// enforces: 05-recursive-composition#nested-runtime-bound
// enforces: 16-sdlc-and-quality#byte-exact-goldens
TEST_CASE("org.arbc.nested renders byte-exact through the parallel offline driver, race-free") {
  VisualScene reference;
  const std::vector<std::byte> expected = render_reference(
      reference.doc,
      [&](PullService& p, Backend& b, const ContentResolver& r, const DocRoot& d) {
        reference.nested->attach(p, b, r, d);
      },
      [&] { reference.nested->detach(); });

  VisualScene scene;
  CpuBackend backend;
  WorkerPoolConfig pool;
  pool.worker_count = 4;
  // Parallel: nested is bound once on the driver thread before any worker dispatch, and
  // its borrowed pointers (including the `std::function` resolver) are read-only on
  // workers thereafter (Constraint 8). The TSan lane is what proves no worker observes a
  // torn resolver mid-attach.
  SequenceRenderer renderer(scene.doc, Viewport{k_dim, k_dim, Affine::identity()}, backend, pool);
  const auto frame = renderer.render_frame_at(Time::zero());
  REQUIRE(frame.has_value());
  require_equal(to_bytes((**frame).cpu_bytes()), expected);
}

// enforces: 05-recursive-composition#nested-runtime-bound
// enforces: 12-audio#nested-mixes-child-audio-through-pull
TEST_CASE("a nested-of-tones scene drained through ExportMonitor mixes byte-exact, not silence") {
  AudioScene scene;
  // No manual attach: ExportMonitor's ctor binds the whole pinned document to its live
  // audio pull. Today (before this wiring) nested's audio facet reads a null
  // PullService and this block comes back silent.
  ExportMonitor monitor(scene.doc, scene.parent, AudioFormat{k_rate, ChannelLayout::Stereo});
  std::vector<float> got(static_cast<std::size_t>(k_frames) * 2U, 0.0F);
  AudioBlock block{got.data(), k_frames, ChannelLayout::Stereo, k_rate};
  monitor.render_block_at(audio_window(), block);

  // Oracle: the same two tones mixed directly, in bottom-to-top order at unit gain.
  const std::vector<float> a = render_tone(*scene.tone_a);
  const std::vector<float> b = render_tone(*scene.tone_b);
  std::vector<float> want(a.size(), 0.0F);
  for (std::size_t i = 0; i < a.size(); ++i) {
    want[i] = a[i] + b[i];
  }
  REQUIRE(got.size() == want.size());
  REQUIRE(std::memcmp(got.data(), want.data(), want.size() * sizeof(float)) == 0);
  // Not the silent path the null-PullService bug produced.
  bool any_nonzero = false;
  for (const float s : got) {
    any_nonzero = any_nonzero || s != 0.0F;
  }
  CHECK(any_nonzero);
}

// enforces: 05-recursive-composition#nested-runtime-bound
TEST_CASE("the bind walk reaches a fade living inside a nested child composition") {
  FadeChildScene reference;
  // The oracle hand-attaches BOTH operator contents -- the nested content and the fade
  // living inside its child composition. That is precisely the work `bind_operators`
  // must do transitively on the scene below: miss the fade and its render asserts.
  const std::vector<std::byte> expected = render_reference(
      reference.doc,
      [&](PullService& p, Backend& b, const ContentResolver& r, const DocRoot& d) {
        reference.nested->attach(p, b, r, d);
        reference.fade->attach(p, b);
      },
      [&] {
        reference.nested->detach();
        reference.fade->detach();
      });

  FadeChildScene scene;
  CpuBackend backend;
  TileCache cache(64U * 1024 * 1024);
  const DocStatePtr pin = scene.doc.pin();
  const ContentResolver resolve = [&scene](ObjectId id) { return scene.doc.resolve(id); };
  PullServiceImpl service(cache, backend, direct_dispatch(), live_config(*pin, resolve));
  register_builtin_operator_binders();

  CHECK_FALSE(scene.nested->attached());
  CHECK_FALSE(scene.fade->attached());
  {
    const OperatorBindingScope scope = bind_operators(scene.doc, service, backend, pin);
    // Both registered-kind contents are bound: the nested content, and the fade that
    // lives inside its child composition. The walk attaches a content BEFORE it
    // enumerates that content's inputs() -- reverse the two and nested's inputs() is
    // still empty (it needs the resolver and pin to read child membership), so the fade
    // would silently never bind.
    CHECK(scope.size() == 2U);
    CHECK(scene.nested->attached());
    CHECK(scene.fade->attached());
  }
  CHECK_FALSE(scene.nested->attached());
  CHECK_FALSE(scene.fade->attached());

  // And through the production driver the fade renders its blend (E = 0.5 at t = 0),
  // byte-identical to the hand-attached twin.
  SequenceRenderer renderer(scene.doc, Viewport{k_dim, k_dim, Affine::identity()}, backend);
  const auto frame = renderer.render_frame_at(Time::zero());
  REQUIRE(frame.has_value());
  require_equal(to_bytes((**frame).cpu_bytes()), expected);
}

// enforces: 05-recursive-composition#nested-runtime-bound
TEST_CASE("releasing the binding scope clears nested's borrowed services, idempotently") {
  VisualScene scene;
  CpuBackend backend;
  TileCache cache(64U * 1024 * 1024);
  const DocStatePtr pin = scene.doc.pin();
  const ContentResolver resolve = [&scene](ObjectId id) { return scene.doc.resolve(id); };
  PullServiceImpl service(cache, backend, direct_dispatch(), live_config(*pin, resolve));
  register_builtin_operator_binders();

  OperatorBindingScope scope = bind_operators(scene.doc, service, backend, pin);
  CHECK(scope.size() == 1U);
  CHECK(scene.nested->attached());
  const std::optional<Rect> bounds_while_bound = scene.nested->bounds();

  scope.release();
  CHECK(scope.size() == 0U);
  CHECK_FALSE(scene.nested->attached());
  // A metadata query after teardown answers from the memo rather than dereferencing the
  // released snapshot (Constraint 7): the memo is a pure function of the child's
  // aggregate revision, so it survives the detach intact.
  CHECK(scene.nested->bounds() == bounds_while_bound);

  scope.release(); // idempotent
  CHECK(scope.size() == 0U);
  CHECK_FALSE(scene.nested->attached());
}

// enforces: 05-recursive-composition#nested-runtime-bound
// enforces: 05-recursive-composition#nested-metadata-memoized-on-aggregate-revision
TEST_CASE("the driver's per-frame re-bind does not re-key nested's metadata memo") {
  VisualScene scene;
  CpuBackend backend;
  SequenceRenderer renderer(scene.doc, Viewport{k_dim, k_dim, Affine::identity()}, backend);
  REQUIRE(renderer.render_frame_at(Time::zero()).has_value());
  const std::uint64_t after_first = scene.nested->metadata_recomputes();
  CHECK(after_first >= 1U); // the memo was keyed at least once for this pin

  // SequenceRenderer binds PER FRAME against a pin taken ONCE for the whole export. An
  // unconditional re-key in attach() would make this counter grow linearly with frame
  // count, breaking the memoization claim precisely on the production path.
  for (const std::int64_t t : {std::int64_t{100}, std::int64_t{200}, std::int64_t{300},
                               std::int64_t{400}}) {
    REQUIRE(renderer.render_frame_at(Time{t}).has_value());
  }
  CHECK(scene.nested->metadata_recomputes() == after_first); // delta 0 across N frames

  // An edit re-pins, and a NEWER snapshot still re-keys exactly once (doc 05:15-16).
  scene.doc.set_layer_transform(scene.layer_a, Affine::translation(2.0, 2.0));
  SequenceRenderer repinned(scene.doc, Viewport{k_dim, k_dim, Affine::identity()}, backend);
  REQUIRE(repinned.render_frame_at(Time::zero()).has_value());
  CHECK(scene.nested->metadata_recomputes() == after_first + 1U);
}

// enforces: 05-recursive-composition#nested-boundary-budget-flows-through
// enforces: 05-recursive-composition#nested-runtime-bound
TEST_CASE("a runtime-bound depth-1 nested frame issues exactly one pull per child layer") {
  VisualScene scene;
  CpuBackend backend;
  CountingPull counting;
  register_builtin_operator_binders();

  const OperatorBindingScope scope =
      bind_operators(scene.doc, counting, backend, scene.doc.pin());
  REQUIRE(scope.size() == 1U);

  const auto target = backend.make_surface(k_dim, k_dim, k_working_rgba32f);
  REQUIRE(target.has_value());
  const RenderRequest req{Rect::from_size(static_cast<double>(k_dim), static_cast<double>(k_dim)),
                          1.0,
                          Time::zero(),
                          StateHandle{},
                          **target,
                          Exactness::Exact,
                          Deadline::none()};
  auto done = std::make_shared<RenderCompletion>();
  REQUIRE(scene.nested->render(req, done).has_value());

  // One pull per visible child layer, no more: the recursive case costs what the
  // equivalent flat scene costs, driven from the runtime binding rather than a
  // hand-attached fixture.
  CHECK(counting.pulls() == 2U);
  // The outer request's exactness rides each child pull verbatim -- never re-derived.
  CHECK(counting.all_exact());
}

// enforces: 05-recursive-composition#nested-boundary-budget-flows-through
// enforces: 05-recursive-composition#nested-runtime-bound
TEST_CASE("a runtime-bound nested culls its invisible, unresolved and degenerate child layers") {
  CulledScene scene;
  CpuBackend backend;
  CountingPull counting;
  register_builtin_operator_binders();

  const OperatorBindingScope scope = bind_operators(scene.doc, counting, backend, scene.doc.pin());
  REQUIRE(scope.size() == 1U);

  const auto target = backend.make_surface(k_dim, k_dim, k_working_rgba32f);
  REQUIRE(target.has_value());
  const RenderRequest req{Rect::from_size(static_cast<double>(k_dim), static_cast<double>(k_dim)),
                          1.0,
                          Time::zero(),
                          StateHandle{},
                          **target,
                          Exactness::Exact,
                          Deadline::none()};
  auto done = std::make_shared<RenderCompletion>();
  const std::optional<RenderResult> result = scene.nested->render(req, done);
  REQUIRE(result.has_value());

  // A cull is a FINAL answer -- no later pass improves on "nothing" -- so the composed
  // output is exact even though not one of the four layers drew a pixel. (Only a
  // DEFERRED pull is the transient case, and none was issued here.)
  CHECK(result->exact);
  // And a culled layer costs nothing: no pull crosses the boundary for any of the
  // four, so the recursive case does not pay for work the equivalent flat scene skips.
  CHECK(counting.pulls() == 0U);
  // Nothing was composited over the cleared target: it stays fully transparent.
  bool all_zero = true;
  for (const std::byte b : (**target).cpu_bytes()) {
    all_zero = all_zero && b == std::byte{0};
  }
  CHECK(all_zero);
}

// enforces: 05-recursive-composition#nested-runtime-bound
TEST_CASE("a moved binding scope carries the bindings and the pin, tearing down exactly once") {
  VisualScene scene;
  CpuBackend backend;
  TileCache cache(64U * 1024 * 1024);
  const DocStatePtr pin = scene.doc.pin();
  const ContentResolver resolve = [&scene](ObjectId id) { return scene.doc.resolve(id); };
  PullServiceImpl service(cache, backend, direct_dispatch(), live_config(*pin, resolve));
  register_builtin_operator_binders();

  OperatorBindingScope scope = bind_operators(scene.doc, service, backend, pin);
  REQUIRE(scope.size() == 1U);
  {
    // A driver that stores its frame's scope (rather than letting the temporary die at
    // the end of the full expression) moves it. The move must carry BOTH halves: the
    // bound contents AND the pin they borrow a `const DocRoot&` into -- leave the pin
    // behind and the husk's destructor drops the snapshot while nested still points at
    // it (Constraint 4).
    OperatorBindingScope moved(std::move(scope));
    CHECK(moved.size() == 1U);
    CHECK(scene.nested->attached());
    // The moved-from husk owns nothing, so it detaches nothing -- otherwise the
    // binding would be torn down twice, once here and once by `moved` below.
    // NOLINTNEXTLINE(bugprone-use-after-move) the moved-from state IS the contract here
    CHECK(scope.size() == 0U);
    scope.release();
    CHECK(scene.nested->attached());
  }
  // ...and the destructor of the scope that DID own the binding tore it down.
  CHECK_FALSE(scene.nested->attached());
}

// enforces: 05-recursive-composition#nested-runtime-bound
TEST_CASE("the widened binder is inert for a document holding no nested content") {
  auto solid =
      std::make_shared<SolidContent>(Rgba{0.60F, 0.20F, 0.10F, 0.80F}, Rect{0.0, 0.0, 8.0, 8.0});
  Document doc;
  doc.add_layer(doc.add_content(solid), Affine::identity());

  CpuBackend backend;
  TileCache cache(64U * 1024 * 1024);
  const DocStatePtr pin = doc.pin();
  const ContentResolver resolve = [&doc](ObjectId id) { return doc.resolve(id); };
  PullServiceImpl service(cache, backend, direct_dispatch(), live_config(*pin, resolve));
  register_builtin_operator_binders();
  // No registered-kind content in the graph: nothing binds, and the widened payload
  // changes nothing for a plain leaf document.
  const OperatorBindingScope scope = bind_operators(doc, service, backend, pin);
  CHECK(scope.size() == 0U);

  // The reference: the solid rendered directly at the driver's frame geometry.
  const auto reference = backend.make_surface(k_dim, k_dim, k_working_rgba32f);
  REQUIRE(reference.has_value());
  const RenderRequest req{Rect::from_size(static_cast<double>(k_dim), static_cast<double>(k_dim)),
                          1.0,
                          Time::zero(),
                          StateHandle{},
                          **reference,
                          Exactness::Exact,
                          Deadline::none()};
  auto done = std::make_shared<RenderCompletion>();
  REQUIRE(solid->render(req, done).has_value());

  SequenceRenderer renderer(doc, Viewport{k_dim, k_dim, Affine::identity()}, backend);
  const auto frame = renderer.render_frame_at(Time::zero());
  REQUIRE(frame.has_value());
  require_equal(to_bytes((**frame).cpu_bytes()), to_bytes((**reference).cpu_bytes()));
}

