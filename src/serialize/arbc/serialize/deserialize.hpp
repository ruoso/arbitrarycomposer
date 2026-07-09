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

namespace arbc {

// The structured content-reconstruction seam the `ContentFactory`/`ContentConfig`
// note anticipates (contract/registry.hpp:33): a kind turns its serialized content
// body -- `{kind, kind_version, params}` -- plus the `LoadContext` (base-URI
// resolution + async asset loading) back into a live `Content`, or an unknown kind
// into a placeholder that preserves the body verbatim (doc 08 Principles 1-2).
//
// serialize.reader lands this SIGNATURE SEAM; serialize.kind_params (`!reader`)
// fills the per-kind bodies and wires the registry routing, exactly mirroring the
// writer's emit-side deferral of the same content body. The canonical writer emits
// no content body today, so this v1 reader has nothing to route into it yet -- the
// type exists so the content half plugs in without reshaping the loader
// (serialize.reader Decision 5).
using DeserializeFn = std::function<expected<std::unique_ptr<Content>, ReaderError>(
    const nlohmann::json& params, LoadContext& ctx)>;

} // namespace arbc
