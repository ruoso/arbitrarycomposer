#pragma once

#include <arbc/arbc_api.h>
#include <arbc/contract/content.hpp>

#include <optional>

namespace arbc {

// Premultiplied RGBA in the working space (doc 07).
struct Rgba {
  float r{0.0F};
  float g{0.0F};
  float b{0.0F};
  float a{0.0F};
};

// Reference kind org.arbc.solid (doc 03): minimal synchronous content, the
// "hello world" of the contract. Optionally bounded so placement and
// culling are exercised; unbounded matches the doc 01 table.
class ARBC_API SolidContent final : public Content {
public:
  explicit SolidContent(Rgba premultiplied_color, std::optional<Rect> bounds = std::nullopt);

  std::optional<Rect> bounds() const override;
  Stability stability() const override;
  std::optional<TimeRange> time_extent() const override;
  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion> done) override;

  // The premultiplied working-space fill color (doc 07). Exposed read-only so the
  // runtime built-in codec (`runtime.document_serialize`) can serialize the kind's
  // `params`; the field is immutable after construction.
  Rgba color() const noexcept { return d_color; }

  static constexpr const char* kind_id = "org.arbc.solid";

private:
  Rgba d_color;
  std::optional<Rect> d_bounds;
};

} // namespace arbc
