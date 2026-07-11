#include <arbc/serialize/load_context.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace arbc {
namespace {

// A reference carries a URI scheme when a "://" separator follows a non-empty run
// of scheme characters ([A-Za-z0-9+-.]) -- e.g. "http://host/x" or
// "content://store/id". v1 leaves the scheme dispatch as a stub (doc 08
// Principle 3): a schemed or absolute reference resolves verbatim, and the http /
// content-store handlers are the later extension point.
bool has_scheme(std::string_view s) {
  const std::size_t sep = s.find("://");
  if (sep == std::string_view::npos || sep == 0) {
    return false;
  }
  for (std::size_t i = 0; i < sep; ++i) {
    const char c = s[i];
    const bool scheme_char = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                             (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
    if (!scheme_char) {
      return false;
    }
  }
  return true;
}

} // namespace

std::string normalize_uri(std::string_view uri) {
  // A SCHEMED reference is left verbatim: scheme dispatch is the stubbed extension
  // point (doc 08 Principle 3), and normalizing an authority-bearing URI textually is
  // exactly the guesswork the stub exists to avoid.
  if (has_scheme(uri)) {
    return std::string(uri);
  }
  const bool absolute = !uri.empty() && uri.front() == '/';
  std::vector<std::string_view> segments;
  std::size_t at = 0;
  while (at <= uri.size()) {
    const std::size_t slash = uri.find('/', at);
    const std::size_t end = (slash == std::string_view::npos) ? uri.size() : slash;
    const std::string_view segment = uri.substr(at, end - at);
    if (segment == "..") {
      if (!segments.empty() && segments.back() != "..") {
        segments.pop_back();
      } else if (!absolute) {
        segments.push_back(segment); // nothing to pop past a relative root: keep it
      }
    } else if (!segment.empty() && segment != ".") {
      segments.push_back(segment);
    }
    if (slash == std::string_view::npos) {
      break;
    }
    at = slash + 1;
  }
  std::string out(absolute ? "/" : "");
  for (std::size_t i = 0; i < segments.size(); ++i) {
    if (i != 0) {
      out.push_back('/');
    }
    out.append(segments[i]);
  }
  return out;
}

std::string LoadContext::resolve_uri(std::string_view reference) const {
  // An absolute path is already rooted; a relative one joins onto the base URI's
  // directory (everything up to and including its last '/'). The join is then
  // normalized, so the resolved URI is a canonical identity rather than a spelling.
  if (has_scheme(reference) || (!reference.empty() && reference.front() == '/')) {
    return normalize_uri(reference);
  }
  const std::size_t slash = d_base_uri.find_last_of('/');
  std::string resolved =
      (slash == std::string::npos) ? std::string() : d_base_uri.substr(0, slash + 1);
  resolved.append(reference);
  return normalize_uri(resolved);
}

ResolvedRef LoadContext::resolve(std::string_view reference) {
  const std::string uri = resolve_uri(reference);
  const auto it = d_by_uri.find(uri);
  if (it != d_by_uri.end()) {
    return ResolvedRef{it->second}; // dedup: same resolved identity
  }
  const std::size_t index = d_resolved.size();
  d_resolved.push_back(uri);
  d_by_uri.emplace(std::move(uri), index);
  return ResolvedRef{index};
}

void LoadContext::load_asset(ResolvedRef ref, std::function<void(std::string_view)> on_ready) {
  if (d_asset_source != nullptr) {
    d_asset_source->request(d_resolved[ref.index], std::move(on_ready));
    return;
  }
  on_ready(std::string_view{}); // no source installed: the asset is unavailable
}

} // namespace arbc
