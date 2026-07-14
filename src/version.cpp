#include <arbc/version.hpp>

namespace arbc {

// The run-time half of the version API (packaging.version_api, doc 10 § Versioning and
// the version API). Deliberately out-of-line, neither `inline` nor `constexpr` and not
// defined in the header: these report the version of the libarbc actually linked or
// loaded, compiled from whatever arbc/version.hpp THIS library was built against, while
// the header's constexpr compiled_version() reports whatever header the CONSUMER was
// built against. Defining them in the header would collapse the two into one number and
// destroy the only thing the pair exists for -- observable header/library skew.
Version linked_version() {
  return Version{ARBC_VERSION_MAJOR, ARBC_VERSION_MINOR, ARBC_VERSION_PATCH};
}

const char* linked_version_string() { return ARBC_VERSION_STRING; }

} // namespace arbc
