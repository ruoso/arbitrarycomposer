// CI-only module for `runtime.plugin_operator_registration` (docs 03/08/17): the
// third-party-operator proof. Unlike the six dual-build modules (which register
// in-lib kinds), the concrete operator type below lives WHOLLY inside this module
// -- no global binder thunk can match it and no built-in codec can serialize it
// (Decision 7) -- so every assertion that round-trips or binds
// `org.arbc.ci.passthrough` discriminates the registry-carried codec/binder path:
//
//  - the `KindCodec` traffics in `params` JSON-object TEXT, parsed/emitted by the
//    module's own micro-grammar with no JSON library anywhere on its link surface
//    (doc 17 §The codec line; pinned by the containment scan in
//    tests/plugin_operator_registration.t.cpp);
//  - the `KindBinder` is the only way this kind can receive its render services:
//    the typed `dynamic_cast` match lives here, the one TU that names the type.
//
// The operator itself is a one-input identity: it pulls input 0 through the
// injected `PullService` into the caller's target verbatim (the fade E == 1
// branch, fade_content.cpp), so "renders byte-exact-equal to its input" is a
// meaningful, discriminating equality. The factory owns its input module-locally
// (the dual_build InputOwner story -- a BARE factory-built operator legitimately
// has no document inputs, doc 17:165-167); the DOCUMENT path adopts core-owned
// inputs through the codec's `deserialize` span (doc 08 Principle 6).

#include <arbc/contract/content.hpp>
#include <arbc/contract/plugin.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/surface/backend.hpp>

#include "ci_kinds.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr const char* k_kind_id = "org.arbc.ci.passthrough";

// The module-owned input a BARE (factory-built) operator borrows, exactly as the
// fade/crossfade dual-build modules own theirs: destroyed at image unload,
// strictly after every operator the host built from this factory
// (runtime/plugin_host.hpp:154-156). Same color as the golden document's solid
// child, so the conformance factory and the document path render identically.
constexpr arbc::Rgba k_input_color{0.50F, 0.25F, 0.125F, 1.0F};

arbc_ci::InputOwner& module_inputs() {
  static arbc_ci::InputOwner owner;
  return owner;
}

// The module-local operator kind. One input, one opaque `note` param (round-trip
// payload only -- it never affects rendering), identity render of its input via
// the attach-borrowed `PullService`, spatial/temporal descriptors passed through
// from the input unchanged.
class PassthroughContent final : public arbc::Content {
public:
  PassthroughContent(arbc::ContentRef input, std::string note)
      : d_input(input), d_note(std::move(note)), d_inputs{input} {}

  // The attach seam the registry-carried binder drives (mirrors
  // FadeContent::attach/detach): borrow both services, own neither.
  void attach(arbc::PullService& pull, arbc::Backend& backend) {
    d_pull = &pull;
    d_backend = &backend;
  }
  void detach() noexcept {
    d_pull = nullptr;
    d_backend = nullptr;
  }

  const std::string& note() const { return d_note; }

  // Pure pass-through: every descriptor is the input's own.
  std::optional<arbc::Rect> bounds() const override { return d_input->bounds(); }
  arbc::Stability stability() const override { return d_input->stability(); }
  std::optional<arbc::TimeRange> time_extent() const override { return d_input->time_extent(); }

  std::span<const arbc::ContentRef> inputs() const override { return d_inputs; }

  // The output IS input 0's output at every request (doc 13:59-65) -- unlike
  // fade, unconditionally.
  std::optional<std::size_t> identity(const arbc::RenderRequest& /*request*/) const override {
    return 0;
  }

  std::optional<arbc::RenderResult> render(const arbc::RenderRequest& request,
                                           std::shared_ptr<arbc::RenderCompletion> /*done*/) override {
    assert(d_pull != nullptr && d_backend != nullptr &&
           "PassthroughContent rendered before attach");
    // Bit-identical pass-through into the caller's target: the sub-request
    // carries snapshot, exactness and deadline verbatim (doc 05:96-100); only
    // the completion is ours. Same discipline as fade's E == 1 branch: an
    // unsettled (worker-dispatched) pull is a TRANSIENT, not-exact placeholder.
    const arbc::RenderRequest sub{request.region,    request.scale,     request.time,
                                  request.snapshot,  request.target,    request.exactness,
                                  request.deadline};
    auto done = std::make_shared<arbc::RenderCompletion>();
    d_pull->pull(d_input, sub, done);
    if (!done->settled()) {
      done->cancel();
      d_backend->clear(request.target, 0.0F, 0.0F, 0.0F, 0.0F);
      return arbc::RenderResult{request.scale, /*exact=*/false};
    }
    const std::optional<arbc::expected<arbc::RenderResult, arbc::RenderError>> settled =
        done->take();
    if (!settled.has_value() || !settled->has_value()) {
      d_backend->clear(request.target, 0.0F, 0.0F, 0.0F, 0.0F);
      return arbc::RenderResult{request.scale, true};
    }
    // The input's own result verbatim -- scale, exactness, achieved_time (a
    // Static input reports none, keeping this content's Static report honest)
    // and any provided surface all pass through.
    return **settled;
  }

private:
  arbc::ContentRef d_input;
  std::string d_note;
  std::array<arbc::ContentRef, 1> d_inputs; // stable storage backing inputs()
  arbc::PullService* d_pull{nullptr};
  arbc::Backend* d_backend{nullptr};
};

// --- Factory: config IS the note (opaque, kind-defined -- doc 03:203-207). The
//     module's params grammar quotes the note verbatim, so characters that would
//     need escaping are a config error VALUE, never a throw.
arbc::expected<std::unique_ptr<arbc::Content>, std::string>
make_passthrough(arbc::ContentConfig config) {
  const std::string_view note{config};
  if (note.find('"') != std::string_view::npos || note.find('\\') != std::string_view::npos) {
    return arbc::unexpected<std::string>(
        "org.arbc.ci.passthrough: note must not contain '\"' or '\\'");
  }
  return std::unique_ptr<arbc::Content>(std::make_unique<PassthroughContent>(
      module_inputs().make<arbc::SolidContent>(k_input_color), std::string(note)));
}

// --- KindCodec hooks: JSON-object TEXT both ways (Decision 2). The core owns
//     canonical form; this module authored both sides of its grammar, so a
//     find-the-key scan over the canonical (compact, key-sorted) dump is enough
//     -- and unknown keys are naturally ignored, which is what lets the core's
//     residual diff preserve them (doc 08 Principle 4).

arbc::expected<std::string, std::string> serialize_passthrough(const arbc::Content& content) {
  const auto* passthrough = dynamic_cast<const PassthroughContent*>(&content);
  if (passthrough == nullptr) {
    return arbc::unexpected<std::string>(
        "org.arbc.ci.passthrough: content is not a passthrough");
  }
  return std::string("{\"note\":\"") + passthrough->note() + "\"}";
}

arbc::expected<std::unique_ptr<arbc::Content>, std::string>
deserialize_passthrough(std::string_view params_text, std::span<const arbc::ContentRef> inputs,
                        arbc::ObjectId /*composition*/) {
  // Arity first (the operator_codecs idiom): wrong arity is an error value and
  // the model stays unmutated -- the codec, not a table field, owns this check.
  if (inputs.size() != 1 || inputs[0] == nullptr) {
    return arbc::unexpected<std::string>("org.arbc.ci.passthrough: expected exactly one input");
  }
  constexpr std::string_view k_note_key = "\"note\":\"";
  const std::size_t at = params_text.find(k_note_key);
  if (at == std::string_view::npos) {
    return arbc::unexpected<std::string>("org.arbc.ci.passthrough: params carry no \"note\"");
  }
  const std::size_t begin = at + k_note_key.size();
  const std::size_t end = params_text.find('"', begin);
  if (end == std::string_view::npos) {
    return arbc::unexpected<std::string>("org.arbc.ci.passthrough: unterminated \"note\"");
  }
  return std::unique_ptr<arbc::Content>(std::make_unique<PassthroughContent>(
      inputs[0], std::string(params_text.substr(begin, end - begin))));
}

// --- KindBinder thunks: the typed match lives HERE, the one TU that names the
//     concrete type (the codec_fade.cpp shape) -- which is exactly why only the
//     registry-carried binder can attach this kind.

bool try_attach_passthrough(arbc::Content& content, const arbc::OperatorBindServices& services) {
  if (auto* passthrough = dynamic_cast<PassthroughContent*>(&content)) {
    passthrough->attach(services.pull, services.backend);
    return true;
  }
  return false;
}

void detach_passthrough(arbc::Content& content) noexcept {
  static_cast<PassthroughContent&>(content).detach();
}

} // namespace

extern "C" ARBC_PLUGIN_EXPORT void arbc_plugin_register(arbc::Registry& registry) {
  // Factory + codec + binder in the ONE atomic add (Constraint 4), through the
  // unchanged extern "C" entry point (Decision 1).
  (void)registry.add(
      k_kind_id, [](arbc::ContentConfig config) { return make_passthrough(config); },
      arbc_ci::metadata("CI Passthrough"),
      arbc::KindCodec{"1", serialize_passthrough, deserialize_passthrough},
      arbc::KindBinder{try_attach_passthrough, detach_passthrough});
}
