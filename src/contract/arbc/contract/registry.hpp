#pragma once

#include <arbc/base/expected.hpp>
#include <arbc/contract/content.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace arbc {

// Why a registry mutation could not be performed. Errors are values across the
// plugin boundary (doc 03:177-180) -- never exceptions.
enum class RegistryError {
  EmptyId,     // the kind id was empty
  DuplicateId, // a kind with this id is already registered
};

// The human-facing metadata a kind advertises beside its factory (doc 03
// §Registry: human name, version, capability flags). Deliberately minimal for
// the v1 seam; capability flags arrive with the kinds that first need them.
struct KindMetadata {
  std::string human_name;
  std::string version;
};

// The config handed to a factory when instantiating a kind. For the v1 stage-1
// seam this is an opaque, kind-defined source locator (doc 03:164-171); a
// serialization format (doc 08) gives it structure later. `org.arbc.imageseq`
// interprets it as the directory holding its numbered frames.
using ContentConfig = std::string_view;

// A kind factory: constructs a fresh Content from a config, or returns an error
// value (a missing/corrupt source is a value, never a throw -- doc 03:177-180).
using ContentFactory =
    std::function<expected<std::unique_ptr<Content>, std::string>(ContentConfig)>;

// id -> factory + metadata (doc 03 §Registry). Reverse-DNS kind ids are the
// persistent contract a future serialization format references, so they are
// part of the contract from day one even though serialization is deferred.
//
// This is the minimal seam the `extern "C" arbc_plugin_register` entry point
// registers into (doc 03:164-171). The production host-side loader -- explicit
// host registration API, opt-in `ARBC_PLUGIN_PATH` directory scan, error
// plumbing across the boundary -- is `runtime.plugin_loading`'s deliverable
// (M8), built on top of this seam, not part of it.
class Registry {
public:
  Registry() = default;

  // Register a kind. Returns success, or an error value if the id is empty or
  // already registered -- never throws (boundary discipline, doc 03:177-180).
  expected<std::monostate, RegistryError> add(std::string_view id, ContentFactory factory,
                                              KindMetadata metadata = {});

  // Look up a factory by id; nullptr if absent.
  const ContentFactory* factory(std::string_view id) const;

  // Look up metadata by id; nullptr if absent.
  const KindMetadata* metadata(std::string_view id) const;

  // Number of registered kinds.
  std::size_t size() const noexcept { return d_entries.size(); }

  // All registered ids, in registration order.
  std::vector<std::string_view> ids() const;

private:
  struct Entry {
    std::string id;
    ContentFactory factory;
    KindMetadata metadata;
  };
  const Entry* find(std::string_view id) const;

  std::vector<Entry> d_entries;
};

} // namespace arbc
