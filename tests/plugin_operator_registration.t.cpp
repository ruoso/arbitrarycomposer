// runtime.plugin_operator_registration end-to-end proof (docs 03/08/17): a
// third-party operator kind -- factory + JSON-free text codec + operator binder,
// all registered through the unchanged `extern "C" arbc_plugin_register` entry
// point -- is loadable into a `Document` and not merely constructible. The
// concrete operator type (`org.arbc.ci.passthrough`,
// tests/ci_plugins/passthrough_ci_plugin.cpp) lives WHOLLY inside the CI module,
// so no built-in codec can serialize it and no global binder thunk can match it
// (Decision 7): every green assertion below discriminates the registry-carried
// path. Inline raw-string goldens, byte-exact CHECKs -- the serialize golden
// convention (operator_codecs_golden.t.cpp).

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/expected.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/model.hpp>
#include <arbc/model/records.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/document_serialize.hpp>
#include <arbc/runtime/operator_binding.hpp>
#include <arbc/runtime/plugin_host.hpp>
#include <arbc/serialize/codec.hpp> // CodecTable (to hold builtin_codecs(registry) by value)
#include <arbc/testing/contract_tests.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using namespace arbc;

constexpr const char* k_kind_id = "org.arbc.ci.passthrough";
constexpr int k_target_edge = 4;
constexpr double k_target_extent = 4.0;

// The canonical bytes for the document under test (sorted keys, 2-space indent,
// trailing newline, shortest-round-trip numbers): one layer-root
// `org.arbc.ci.passthrough` operator over a CORE-OWNED `inputs` edge to a
// built-in solid child (doc 08 Principle 6) -- the child is inline (referenced
// once), the operator's `params` carry the module's one `note` key, and the
// core owns `kind`/`kind_version`/`inputs` placement (Constraint 9).
constexpr const char* k_golden = R"json({
  "arbc": {
    "format": 1
  },
  "composition": {
    "canvas": [
      0,
      0,
      8,
      8
    ],
    "layers": [
      {
        "inputs": [
          {
            "kind": "org.arbc.solid",
            "kind_version": "1",
            "params": {
              "color": [
                0.5,
                0.25,
                0.125,
                1.0
              ]
            }
          }
        ],
        "kind": "org.arbc.ci.passthrough",
        "kind_version": "1",
        "opacity": 1.0,
        "params": {
          "note": "ci"
        },
        "transform": [
          1.0,
          0.0,
          0.0,
          1.0,
          0.0,
          0.0
        ],
        "visible": true
      }
    ]
  }
}
)json";

// The same document with a hand-authored UNKNOWN field inside the plugin's
// `params` (doc 08 Principle 4): the module's codec ignores it, and the core's
// load-time residual diff -- run through the adapter's re-serialization of the
// plugin's own text -- is what carries it back out byte-exactly.
constexpr const char* k_golden_unknown_params = R"json({
  "arbc": {
    "format": 1
  },
  "composition": {
    "canvas": [
      0,
      0,
      8,
      8
    ],
    "layers": [
      {
        "inputs": [
          {
            "kind": "org.arbc.solid",
            "kind_version": "1",
            "params": {
              "color": [
                0.5,
                0.25,
                0.125,
                1.0
              ]
            }
          }
        ],
        "kind": "org.arbc.ci.passthrough",
        "kind_version": "1",
        "opacity": 1.0,
        "params": {
          "ci_unknown": true,
          "note": "ci"
        },
        "transform": [
          1.0,
          0.0,
          0.0,
          1.0,
          0.0,
          0.0
        ],
        "visible": true
      }
    ]
  }
}
)json";

// The arity error-path fixture: the one-input operator authored with NO inputs.
// The module's `deserialize` validates arity itself (the operator_codecs idiom)
// and answers with an error VALUE the adapter maps to a `ReaderError`.
constexpr const char* k_golden_zero_inputs = R"json({
  "arbc": {
    "format": 1
  },
  "composition": {
    "canvas": [
      0,
      0,
      8,
      8
    ],
    "layers": [
      {
        "kind": "org.arbc.ci.passthrough",
        "kind_version": "1",
        "opacity": 1.0,
        "params": {
          "note": "ci"
        },
        "transform": [
          1.0,
          0.0,
          0.0,
          1.0,
          0.0,
          0.0
        ],
        "visible": true
      }
    ]
  }
}
)json";

// The color the golden's solid child (and the module's factory-owned input)
// carries -- the identity-render twin below is constructed from the same value.
constexpr Rgba k_input_color{0.50F, 0.25F, 0.125F, 1.0F};

// The host-image PullService double the operator borrows through the
// registry-carried binder (the dual_build.t.cpp shape): a plugin-image operator
// calling `pull` dispatches back into the host.
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

// The single composition's layer-bound content ids, bottom-to-top, off a pinned
// root (the operator_codecs_golden.t.cpp helper).
std::vector<ObjectId> layer_contents(const DocStatePtr& state) {
  std::vector<ObjectId> out;
  ObjectId comp_id;
  const CompositionRecord* comp = nullptr;
  if (!state->find_first_composition(comp_id, comp)) {
    return out;
  }
  state->for_each_layer_in(comp_id, [&](ObjectId lid) {
    const LayerRecord* lr = state->find_layer(lid);
    if (lr != nullptr) {
      out.push_back(lr->content);
    }
  });
  return out;
}

// The loaded document's single layer-root content.
Content* loaded_root(const Document& doc) {
  const std::vector<ObjectId> roots = layer_contents(doc.pin());
  REQUIRE(roots.size() == 1);
  Content* root = doc.resolve(roots[0]);
  REQUIRE(root != nullptr);
  return root;
}

// Settle one render along whichever of the two legal paths the content answers
// on (inline value, or nullopt + a settled RenderCompletion).
std::optional<RenderResult> settle(std::optional<RenderResult> inline_result,
                                   const std::shared_ptr<RenderCompletion>& done) {
  if (inline_result.has_value()) {
    return inline_result;
  }
  std::optional<expected<RenderResult, RenderError>> settled = done->take();
  if (!settled.has_value() || !settled->has_value()) {
    return std::nullopt;
  }
  return **settled;
}

// Byte-exact render equality (the dual_build.t.cpp shape): `a` and `b` driven
// through the same backend and the same request produce byte-identical pixels
// and equal RenderResult fields. No tolerance (doc 16).
void require_render_equivalent(CpuBackend& backend, Content& a, Content& b, Time time) {
  auto a_target = backend.make_surface(k_target_edge, k_target_edge, k_working_rgba32f);
  auto b_target = backend.make_surface(k_target_edge, k_target_edge, k_working_rgba32f);
  REQUIRE(a_target.has_value());
  REQUIRE(b_target.has_value());

  const Rect region = Rect::from_size(k_target_extent, k_target_extent);
  auto a_done = std::make_shared<RenderCompletion>();
  auto b_done = std::make_shared<RenderCompletion>();
  const RenderRequest a_request{
      region, 1.0, time, StateHandle{}, **a_target, Exactness::Exact, Deadline::none()};
  const RenderRequest b_request{
      region, 1.0, time, StateHandle{}, **b_target, Exactness::Exact, Deadline::none()};

  const std::optional<RenderResult> a_result = settle(a.render(a_request, a_done), a_done);
  const std::optional<RenderResult> b_result = settle(b.render(b_request, b_done), b_done);
  REQUIRE(a_result.has_value());
  REQUIRE(b_result.has_value());
  REQUIRE(a_result->achieved_scale == b_result->achieved_scale);
  REQUIRE(a_result->exact == b_result->exact);
  REQUIRE(a_result->achieved_time.has_value() == b_result->achieved_time.has_value());
  REQUIRE(a_result->provided.has_value() == b_result->provided.has_value());

  const std::span<std::byte> a_pixels = (*a_target)->cpu_bytes();
  const std::span<std::byte> b_pixels = (*b_target)->cpu_bytes();
  REQUIRE(a_pixels.size() == b_pixels.size());
  REQUIRE_FALSE(a_pixels.empty());
  REQUIRE(std::memcmp(a_pixels.data(), b_pixels.data(), a_pixels.size()) == 0);
}

// Load the passthrough CI module into a fresh host (the widened surface travels
// the UNCHANGED entry point, Decision 1) and pin the registration's shape.
void require_passthrough_loaded(PluginHost& host) {
  REQUIRE(host.load_plugin(ARBC_CI_PLUGIN_PASSTHROUGH_FILE).has_value());
  REQUIRE(host.registry().size() == 1);
  REQUIRE(host.registry().factory(k_kind_id) != nullptr);
}

std::string read_binary(const char* path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

// enforces: 08-serialization#plugin-codec-round-trips-through-document
// enforces: 03-layer-plugin-interface#plugin-registers-through-extern-c-entry
// enforces: 03-layer-plugin-interface#registry-resolves-kind-id-to-factory-and-metadata
TEST_CASE("a plugin-registered operator kind round-trips through a document byte-exactly") {
  PluginHost host;
  require_passthrough_loaded(host);
  const Registry& registry = host.registry();

  // The one entry carries all three registered facets, resolved by the same id
  // the factory resolves under.
  REQUIRE(registry.codec(k_kind_id) != nullptr);
  CHECK(registry.codec(k_kind_id)->kind_version == "1");
  REQUIRE(registry.binder(k_kind_id) != nullptr);
  const KindMetadata* meta = registry.metadata(k_kind_id);
  REQUIRE(meta != nullptr);
  CHECK(meta->human_name == "CI Passthrough");

  // Load: the reader builds the solid child first and the module's codec adopts
  // it through the CORE-OWNED inputs span (doc 08 Principle 6).
  KindBridge bridge;
  Document doc;
  REQUIRE(arbc::load_document(k_golden, doc, bridge, registry).has_value());
  CHECK(doc.registry() == &registry); // the load hands the registry to the document (Decision 4)

  Content* root = loaded_root(doc);
  REQUIRE(root->inputs().size() == 1);
  REQUIRE(root->inputs()[0] != nullptr);

  // Resave through the registry-widened table: byte-exact -- the core
  // canonicalized whatever the plugin's text hook emitted (Constraint 9).
  const CodecTable codecs = builtin_codecs(registry);
  const expected<std::string, SerializeError> resaved = save_document(doc, bridge, codecs);
  REQUIRE(resaved.has_value());
  CHECK(*resaved == std::string(k_golden));
}

// enforces: 03-layer-plugin-interface#plugin-binder-attaches-render-services
TEST_CASE("bind_operators attaches a plugin operator through the registry-carried binder") {
  PluginHost host;
  require_passthrough_loaded(host);

  KindBridge bridge;
  Document doc;
  REQUIRE(arbc::load_document(k_golden, doc, bridge, host.registry()).has_value());
  Content* root = loaded_root(doc);
  REQUIRE(root->inputs().size() == 1);

  register_builtin_operator_binders();
  CpuBackend backend;
  InlinePull pull;

  // Discriminating half: with the document's registry cleared, NO binder -- the
  // global built-in trio included -- can match the module-local operator type.
  doc.set_registry(nullptr);
  {
    const OperatorBindingScope unbound = bind_operators(doc, pull, backend, doc.pin());
    CHECK(unbound.size() == 0);
  }
  doc.set_registry(&host.registry());

  // The registry-carried binder is consulted after the global built-ins and
  // attaches exactly the one operator (the solid child matches nothing).
  OperatorBindingScope scope = bind_operators(doc, pull, backend, doc.pin());
  CHECK(scope.size() == 1);

  // Identity passthrough: the bound operator's render is byte-exact-equal to
  // rendering its input directly.
  SolidContent twin{k_input_color};
  require_render_equivalent(backend, *root, twin, Time{0});

  // Scope teardown detaches (Constraint 3): released, nothing stays bound.
  scope.release();
  CHECK(scope.size() == 0);
}

// enforces: 08-serialization#unknown-kind-round-trips-verbatim
// enforces: 08-serialization#placeholder-renders-input-0-passthrough
TEST_CASE("the same document without the plugin degrades to a verbatim placeholder") {
  // No plugin loaded: an empty registry witnesses "kind absent" (doc 08 P2).
  const Registry registry;
  KindBridge bridge;
  Document doc;
  REQUIRE(arbc::load_document(k_golden, doc, bridge, registry).has_value());

  // The placeholder binds the reconstructed solid child as its live
  // pass-through input and reports input-0 identity, so the compositor serves
  // the child directly -- the degradation this task must preserve unchanged.
  Content* root = loaded_root(doc);
  REQUIRE(root->inputs().size() == 1);
  CpuBackend backend;
  auto target = backend.make_surface(k_target_edge, k_target_edge, k_working_rgba32f);
  REQUIRE(target.has_value());
  const RenderRequest request{Rect::from_size(k_target_extent, k_target_extent),
                              1.0,
                              Time{0},
                              StateHandle{},
                              **target,
                              Exactness::Exact,
                              Deadline::none()};
  CHECK(root->identity(request) == std::optional<std::size_t>{0});

  // Verbatim round-trip: kind, kind_version and params survive byte-exactly
  // through the placeholder's stored body; `inputs` is re-derived by the core.
  const CodecTable codecs = builtin_codecs(registry);
  const expected<std::string, SerializeError> resaved = save_document(doc, bridge, codecs);
  REQUIRE(resaved.has_value());
  CHECK(*resaved == std::string(k_golden));
}

// enforces: 08-serialization#known-kind-params-unknowns-preserved
TEST_CASE("an unknown field inside the plugin's params survives a load/save") {
  PluginHost host;
  require_passthrough_loaded(host);

  KindBridge bridge;
  Document doc;
  REQUIRE(arbc::load_document(k_golden_unknown_params, doc, bridge, host.registry()).has_value());

  // The module's micro-grammar ignored `ci_unknown`; the core's residual diff --
  // computed by re-running the plugin's serialize TEXT hook through the adapter
  // at load time -- carries it back out byte-exactly (doc 08 Principle 4).
  const CodecTable codecs = builtin_codecs(host.registry());
  const expected<std::string, SerializeError> resaved = save_document(doc, bridge, codecs);
  REQUIRE(resaved.has_value());
  CHECK(*resaved == std::string(k_golden_unknown_params));
}

// The error paths stay values on both hooks (Constraint 7).
TEST_CASE("plugin codec failures are error values, never throws") {
  SECTION("wrong input arity is a ReaderError value, model unmutated") {
    PluginHost host;
    require_passthrough_loaded(host);
    KindBridge bridge;
    Document doc;
    expected<std::monostate, ReaderError> loaded{std::monostate{}};
    REQUIRE_NOTHROW(loaded =
                        arbc::load_document(k_golden_zero_inputs, doc, bridge, host.registry()));
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().kind == ReaderError::Kind::MalformedField);
    CHECK(doc.pin()->revision() == 0); // the model is unmutated (the operator_codecs idiom)
  }

  SECTION("a serialize hook emitting non-JSON text is a SerializeError value") {
    // Registered directly (no dlopen needed): the adapter's parse discipline is
    // the subject, and `Registry::add` is the same seam the entry point uses.
    // The document's content is a stock solid stamped with the test kind's
    // interned token -- the codec routes on the token's kind string, and the
    // hook never inspects the content. Three misbehaviors, one discipline: an
    // error-value hook, unparseable text, and parseable-but-non-object text are
    // all `CodecFailed` values.
    struct Hook {
      const char* text; // nullptr: the hook answers with an error value instead
    };
    for (const Hook hook : {Hook{nullptr}, Hook{"!! not json !!"}, Hook{"[1,2]"}}) {
      Registry registry;
      KindCodec codec;
      codec.kind_version = "1";
      codec.serialize = [text = hook.text](const Content&) -> expected<std::string, std::string> {
        if (text == nullptr) {
          return arbc::unexpected<std::string>("org.arbc.ci.badcodec: serialize failed");
        }
        return std::string(text);
      };
      codec.deserialize = [](std::string_view, std::span<const ContentRef>,
                             ObjectId) -> expected<std::unique_ptr<Content>, std::string> {
        return arbc::unexpected<std::string>("unused");
      };
      REQUIRE(registry
                  .add(
                      "org.arbc.ci.badcodec",
                      [](ContentConfig) -> expected<std::unique_ptr<Content>, std::string> {
                        return arbc::unexpected<std::string>("unused");
                      },
                      KindMetadata{"Bad Codec", "1"}, std::move(codec))
                  .has_value());

      KindBridge bridge;
      Document doc;
      const ObjectId comp = doc.add_composition(8.0, 8.0);
      const ObjectId content = doc.add_content(std::make_shared<SolidContent>(k_input_color),
                                               bridge.intern("org.arbc.ci.badcodec", "1"));
      doc.attach_layer(comp, doc.add_layer(content, Affine::identity(), 1.0));

      const CodecTable codecs = builtin_codecs(registry);
      expected<std::string, SerializeError> saved{std::string{}};
      REQUIRE_NOTHROW(saved = save_document(doc, bridge, codecs));
      REQUIRE_FALSE(saved.has_value());
      CHECK(saved.error().kind == SerializeError::Kind::CodecFailed);
    }
  }

  SECTION("a well-formed text hook saves through the adapter (the document was intact)") {
    Registry registry;
    KindCodec codec;
    codec.kind_version = "1";
    codec.serialize = [](const Content&) -> expected<std::string, std::string> {
      return std::string("{\"ok\":true}");
    };
    codec.deserialize = [](std::string_view, std::span<const ContentRef>,
                           ObjectId) -> expected<std::unique_ptr<Content>, std::string> {
      return arbc::unexpected<std::string>("unused");
    };
    REQUIRE(registry
                .add(
                    "org.arbc.ci.okcodec",
                    [](ContentConfig) -> expected<std::unique_ptr<Content>, std::string> {
                      return arbc::unexpected<std::string>("unused");
                    },
                    KindMetadata{"Ok Codec", "1"}, std::move(codec))
                .has_value());

    KindBridge bridge;
    Document doc;
    const ObjectId comp = doc.add_composition(8.0, 8.0);
    const ObjectId content = doc.add_content(std::make_shared<SolidContent>(k_input_color),
                                             bridge.intern("org.arbc.ci.okcodec", "1"));
    doc.attach_layer(comp, doc.add_layer(content, Affine::identity(), 1.0));

    const CodecTable codecs = builtin_codecs(registry);
    const expected<std::string, SerializeError> saved = save_document(doc, bridge, codecs);
    REQUIRE(saved.has_value());
    CHECK(saved->find("\"kind\": \"org.arbc.ci.okcodec\"") != std::string::npos);
    CHECK(saved->find("\"ok\": true") != std::string::npos); // core-canonicalized params
  }
}

// The conformance run over a factory obtained across dlopen, attached through
// the registry-carried binder -- the dual_build.t.cpp adaptation, one kind.
TEST_CASE("the boundary-obtained passthrough factory passes the contract conformance suite") {
  PluginHost host;
  require_passthrough_loaded(host);
  const ContentFactory& factory = *host.registry().factory(k_kind_id);
  const KindBinder* binder = host.registry().binder(k_kind_id);
  REQUIRE(binder != nullptr);

  // The host doubles outlive every content the suite builds (all die inside
  // contract_tests, strictly before `host`).
  CpuBackend backend;
  InlinePull pull;
  const OperatorBindServices services{pull, backend};

  testing::Options options; // the operator options the dual_build driver uses
  options.snapshot_sensitive = false;
  options.operator_graph = true;

  arbc::contract_tests(
      [&]() -> std::unique_ptr<Content> {
        expected<std::unique_ptr<Content>, std::string> made = factory("conformance");
        REQUIRE(made.has_value());
        std::unique_ptr<Content> content = std::move(*made);
        REQUIRE(binder->try_attach(*content, services));
        return content;
      },
      options);
}

// The module's own boundary discipline: every hook degrades as a VALUE (doc
// 03:177-180), and a pull that cannot answer this frame degrades to the same
// transient transparent placeholder the built-in operators use.
TEST_CASE("the passthrough module's factory, hooks and render degrade as values") {
  PluginHost host;
  require_passthrough_loaded(host);
  const Registry& registry = host.registry();
  const ContentFactory& factory = *registry.factory(k_kind_id);
  const KindCodec* codec = registry.codec(k_kind_id);
  const KindBinder* binder = registry.binder(k_kind_id);
  REQUIRE(codec != nullptr);
  REQUIRE(binder != nullptr);

  SECTION("a config the params grammar cannot quote is a factory error value") {
    expected<std::unique_ptr<Content>, std::string> made{nullptr};
    REQUIRE_NOTHROW(made = factory("bad \" note"));
    REQUIRE_FALSE(made.has_value());
    REQUIRE_NOTHROW(made = factory("bad \\ note"));
    REQUIRE_FALSE(made.has_value());
  }

  SECTION("the serialize hook rejects a foreign content as a value") {
    SolidContent solid{k_input_color};
    CHECK_FALSE(codec->serialize(solid).has_value());
  }

  SECTION("the deserialize hook validates its own params text as values") {
    SolidContent input{k_input_color};
    const ContentRef inputs[] = {&input};
    CHECK_FALSE(codec->deserialize("{}", inputs, ObjectId{}).has_value());
    CHECK_FALSE(codec->deserialize("{\"note\":\"x", inputs, ObjectId{}).has_value());
    // ...and the well-formed text round-trips through the raw hook pair.
    const expected<std::unique_ptr<Content>, std::string> made =
        codec->deserialize("{\"note\":\"hook\"}", inputs, ObjectId{});
    REQUIRE(made.has_value());
    const expected<std::string, std::string> text = codec->serialize(**made);
    REQUIRE(text.has_value());
    CHECK(*text == "{\"note\":\"hook\"}");
  }

  SECTION("an unsettled or failed pull degrades to a transparent placeholder") {
    // The two non-inline pull outcomes the built-in operators already handle:
    // a worker-dispatched (never-settling) pull is a TRANSIENT, not-exact
    // answer; a failed pull is an exact transparent cull.
    class NeverSettlePull final : public PullService {
      void pull(ContentRef, const RenderRequest&, std::shared_ptr<RenderCompletion>) override {}
    };
    class FailingPull final : public PullService {
      void pull(ContentRef, const RenderRequest&, std::shared_ptr<RenderCompletion> done) override {
        done->fail(RenderError::ContentFailed);
      }
    };
    CpuBackend backend;
    expected<std::unique_ptr<Content>, std::string> made = factory("probe");
    REQUIRE(made.has_value());
    const std::unique_ptr<Content> content = std::move(*made);

    const auto render_with = [&](PullService& pull, bool expect_exact) {
      REQUIRE(binder->try_attach(*content, OperatorBindServices{pull, backend}));
      auto target = backend.make_surface(k_target_edge, k_target_edge, k_working_rgba32f);
      REQUIRE(target.has_value());
      auto done = std::make_shared<RenderCompletion>();
      const RenderRequest request{Rect::from_size(k_target_extent, k_target_extent),
                                  1.0,
                                  Time{0},
                                  StateHandle{},
                                  **target,
                                  Exactness::BestEffort,
                                  Deadline::none()};
      const std::optional<RenderResult> result = content->render(request, done);
      REQUIRE(result.has_value());
      CHECK(result->exact == expect_exact);
      CHECK_FALSE(result->achieved_time.has_value());
      binder->detach(*content);
    };
    NeverSettlePull never;
    render_with(never, /*expect_exact=*/false);
    FailingPull failing;
    render_with(failing, /*expect_exact=*/true);
  }
}

// enforces: 17-internal-components#plugin-codec-registration-is-json-free
TEST_CASE("the widened registration surface keeps JSON out of the plugin's link surface") {
  // The containment scan (the image/imageseq claims' shape): the JSON library's
  // namespace appears as ASCII in any artifact whose translation units
  // instantiated it. The passthrough module registers a factory, a codec and a
  // binder and must carry NO trace of it -- the text-typed `KindCodec` is the
  // whole point (doc 17 §The codec line, Decision 2)...
  REQUIRE(read_binary(ARBC_CI_PLUGIN_PASSTHROUGH_FILE).find("nlohmann") == std::string::npos);
  // ...and the needle is real, not a typo'd vacuity: the core, whose serialize
  // component legally owns the dependency (doc 17:52-56), does carry it.
  REQUIRE(read_binary(ARBC_LIBARBC_FILE).find("nlohmann") != std::string::npos);
}
