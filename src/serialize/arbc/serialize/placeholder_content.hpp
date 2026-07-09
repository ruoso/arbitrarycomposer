#pragma once

// INTERNAL serialize-component header (NOT in PUBLIC_HEADERS): it names
// `nlohmann::json`, kept PRIVATE to `arbc_serialize` (serialize.writer Decision 3).
//
// The one kind-agnostic, JSON-shaped `Content` (Decision 4), so `arbc_serialize` is
// its natural home. It stands in for a kind whose plugin the host lacks -- or a kind
// that chose placeholder behavior over lossy parsing of newer `params` (doc 08
// Principle 2 version skew) -- preserving the layer's content body
// (`kind`/`kind_version`/`params`/`inputs`, and any unknown fields -- Principle 4)
// verbatim so it re-serializes byte-equivalent under canonical formatting, and
// renders input 0 as pass-through when it has a bound input (doc 08 Principle 6,
// doc 13), else a bounded diagnostic fill. A missing plugin never destroys data and
// never punches a hole.

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

class PlaceholderContent final : public Content {
public:
  // `body` is the layer's content body, stored verbatim (all keys). `kind_registered`
  // records whether the kind's plugin is present in the `Registry` even though no
  // serialize codec is registered (the read routing's registry witness). `inputs`
  // are the live edges the pass-through render reads -- empty on the read path until
  // serialize.sharing binds them (Decision 6); a unit test supplies a synthetic one.
  explicit PlaceholderContent(nlohmann::json body, bool kind_registered = false,
                              std::vector<ContentRef> inputs = {});

  // The verbatim content body -- what re-serialization emits (canonical on dump).
  const nlohmann::json& body() const noexcept { return d_body; }
  // Whether the read routing found the kind's plugin registered (a factory present)
  // despite the missing codec -- the `Registry`-consulted witness (Decision 2).
  bool kind_registered() const noexcept { return d_kind_registered; }

  std::optional<Rect> bounds() const override;
  Stability stability() const override;
  std::optional<TimeRange> time_extent() const override;
  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion> done) override;

  // The preserved input edges (doc 08 Principle 6): the verbatim `inputs` are
  // surfaced as live edges here so the compositor can serve input 0 under
  // `identity()`. Empty until serialize.sharing resolves the `inputs` array.
  std::span<const ContentRef> inputs() const override;

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
};

} // namespace arbc
