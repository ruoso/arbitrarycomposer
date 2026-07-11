#pragma once

// INTERNAL serialize-component header (deliberately NOT in the component's
// PUBLIC_HEADERS FILE_SET): it names `nlohmann::json`, which is linked PRIVATE so
// it never rides libarbc's public interface (serialize.writer Decision 3). Only
// serialize-component translation units (reader.cpp; serialize.kind_params next)
// include it.

#include <arbc/base/expected.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/serialize/load_context.hpp>
#include <arbc/serialize/reader.hpp>

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <span>

namespace arbc {

// The structured content-reconstruction seam the `ContentFactory`/`ContentConfig`
// note anticipates (contract/registry.hpp:33): a kind turns its serialized content
// body -- `{kind, kind_version, params}` -- plus the already-reconstructed input
// edges (`inputs`) and the `LoadContext` (base-URI resolution + async asset
// loading) back into a live `Content`, or an unknown kind into a placeholder that
// preserves the body verbatim (doc 08 Principles 1-2, 6).
//
// serialize.reader landed this SIGNATURE SEAM; serialize.kind_params filled the
// per-kind bodies and wired the registry routing; serialize.sharing grows the
// `inputs` parameter (Decision 4): the read recursion builds a node's input
// children FIRST and threads their live, non-owning `Content*`s down, so a
// known-kind operator adopts its inputs at construction (e.g.
// `FadeContent(params, inputs[0])`), symmetric with the write side walking
// `Content::inputs()`. A leaf kind receives an empty span.
//
// serialize.compositions_table grows `composition` (Decision 5): the resolved
// `ObjectId` of the child composition this content's body named through the
// core-owned `"composition"` field (doc 08 Principle 7), already allocated by the
// reader BEFORE the composition's own layers are built -- which is what lets a
// Droste cycle terminate. A nesting kind builds itself around it
// (`NestedContent(child)`); every other kind receives `ObjectId{}` and ignores it.
// Asymmetric with `SerializeFn` on purpose: on save the core re-derives the id from
// `Content::composition_ref()` and appends it after the codec returns, exactly as it
// re-derives `inputs` -- so a codec neither writes nor reads it.
using DeserializeFn = std::function<expected<std::unique_ptr<Content>, ReaderError>(
    const nlohmann::json& params, std::span<const ContentRef> inputs, ObjectId composition,
    LoadContext& ctx)>;

} // namespace arbc
