#pragma once

// INTERNAL serialize-component header (NOT in PUBLIC_HEADERS): it names
// `nlohmann::json`, kept PRIVATE to `arbc_serialize` (serialize.writer Decision 3).
//
// The one kind-agnostic, JSON-shaped `Content` (Decision 4), so `arbc_serialize` is
// its natural home. It stands in for a kind whose plugin the host lacks -- or a kind
// that chose placeholder behavior over lossy parsing of newer `params` (doc 08
// Principle 2 version skew) -- preserving the layer's LEAF content body
// (`kind`/`kind_version`/`params`, and any unknown fields -- Principle 4) verbatim so
// it re-serializes byte-equivalent under canonical formatting. Its `inputs` are held
// as live edges (`d_inputs`), NOT baked into the stored body: the operator graph is
// graph-structural (serialize.sharing), re-derived on save from `inputs()` with
// canonical `$ref` ids (doc 08 Principle 6, Decision 2). It renders input 0 as
// pass-through when it has a bound input (doc 13), else a bounded diagnostic fill. A
// missing plugin never destroys data and never punches a hole.

#include <arbc/arbc_api.h>
#include <arbc/contract/content.hpp>

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace arbc {

// The premultiplied working-space color a no-input placeholder fills: a deliberately
// unnatural half-coverage magenta, so a missing plugin renders a visible diagnostic
// rather than a hole or a crash (doc 08 Principle 6, doc 13). Exposed for the render
// golden. Premultiplied (doc 07): straight magenta (1, 0, 1) at alpha 0.5.
inline constexpr std::array<float, 4> k_placeholder_diagnostic{0.5F, 0.0F, 0.5F, 0.5F};

class ARBC_API PlaceholderContent final : public Content {
public:
  // `body` is the layer's LEAF content body, stored verbatim (all keys EXCEPT
  // `inputs`, which the read routing strips -- it is re-derived from `inputs()` on
  // save, serialize.sharing). `kind_registered` records whether the kind's plugin is
  // present in the `Registry` even though no serialize codec is registered (the read
  // routing's registry witness). `inputs` are the live edges the pass-through render
  // reads -- the read recursion binds them from the resolved `inputs` array
  // (serialize.sharing); empty for a leaf placeholder.
  // `composition` is the resolved child composition the body's core-owned
  // `"composition"` field named -- likewise stripped from the stored body, likewise
  // re-derived on save from `composition_ref()`. A THIRD-PARTY nesting kind this build
  // cannot load therefore still holds its edge, so the writer's walk reaches the child
  // and never drops it: a missing plugin never orphans the composition it embeds
  // (doc 08 Principle 2, serialize.compositions_table Decision 6). `ObjectId{}` for a
  // non-nesting placeholder.
  explicit PlaceholderContent(nlohmann::json body, bool kind_registered = false,
                              std::vector<ContentRef> inputs = {},
                              ObjectId composition = ObjectId{});

  // The verbatim LEAF content body (no `inputs` key) -- what re-serialization emits
  // for this node, canonical on dump; the write recursion appends the `inputs` limb.
  const nlohmann::json& body() const noexcept { return d_body; }
  // Whether the read routing found the kind's plugin registered (a factory present)
  // despite the missing codec -- the `Registry`-consulted witness (Decision 2).
  bool kind_registered() const noexcept { return d_kind_registered; }

  std::optional<Rect> bounds() const override;
  Stability stability() const override;
  std::optional<TimeRange> time_extent() const override;
  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion> done) override;

  // The preserved input edges (doc 08 Principle 6): the resolved `inputs` are surfaced
  // as live edges here so the compositor can serve input 0 under `identity()`. Empty
  // for a leaf placeholder.
  std::span<const ContentRef> inputs() const override;

  // The preserved child-composition edge (doc 08 Principle 7): the core reads it here
  // exactly as it reads a live nesting kind's, so an unknown nesting kind's child stays
  // reachable from the writer's composition walk.
  ObjectId composition_ref() const override;

  // Input-0 pass-through (doc 08 Principle 6, doc 13; Decision 4): return 0 when a
  // bound input is present so the compositor serves input 0 unchanged -- a missing
  // fade plugin degrades to an unfaded clip, not a hole. `nullopt` (diagnostic fill)
  // when there is no input. The placeholder's chosen behavior, not a format mandate.
  std::optional<std::size_t> identity(const RenderRequest& request) const override;

private:
  bool has_passthrough_input() const;

  nlohmann::json d_body;
  bool d_kind_registered;
  std::vector<ContentRef> d_inputs;
  ObjectId d_composition;
};

} // namespace arbc
