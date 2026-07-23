#include <arbc/runtime/pending_external_loads.hpp>

#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace arbc {

bool PendingExternalLoads::find(const std::string& uri, ObjectId& child) const {
  const auto it = d_by_uri.find(uri);
  if (it == d_by_uri.end()) {
    return false;
  }
  child = it->second;
  return true;
}

void PendingExternalLoads::record(std::string uri, ObjectId child) {
  d_by_uri.insert_or_assign(std::move(uri), child);
}

void PendingExternalLoads::add_pending(ObjectId child, std::string uri, std::size_t depth) {
  d_pending.insert_or_assign(child, Entry{std::move(uri), depth});
}

bool PendingExternalLoads::take_pending(ObjectId child, std::string& uri, std::size_t& depth) {
  const auto it = d_pending.find(child);
  if (it == d_pending.end()) {
    return false;
  }
  uri = std::move(it->second.uri);
  depth = it->second.depth;
  d_pending.erase(it);
  return true;
}

bool PendingExternalLoads::find_asset(const std::string& uri, ObjectId& fetch) const {
  const auto it = d_asset_by_uri.find(uri);
  if (it == d_asset_by_uri.end()) {
    return false;
  }
  fetch = it->second;
  return true;
}

void PendingExternalLoads::add_pending_asset(ObjectId fetch, std::string uri) {
  d_asset_by_uri.insert_or_assign(uri, fetch);
  d_pending_assets.insert_or_assign(fetch, AssetEntry{std::move(uri), {}});
}

void PendingExternalLoads::add_asset_waiter(ObjectId fetch, ObjectId content) {
  const auto it = d_pending_assets.find(fetch);
  if (it == d_pending_assets.end()) {
    return; // the fetch answered inline, or was never pending: nothing awaits anything
  }
  it->second.awaiting.push_back(content);
}

bool PendingExternalLoads::take_pending_asset(ObjectId fetch, AssetEntry& entry) {
  const auto it = d_pending_assets.find(fetch);
  if (it == d_pending_assets.end()) {
    return false;
  }
  entry = std::move(it->second);
  d_pending_assets.erase(it);
  // The URI leaves the in-flight dedup map with its entry: the fetch is no longer in flight,
  // so a later reference to the same URI must issue an honest new request rather than join a
  // fetch that already answered.
  d_asset_by_uri.erase(entry.uri);
  return true;
}

void PendingExternalLoads::complete(ObjectId child, std::string_view bytes) {
  // The ONLY method an `on_ready` calls, and it touches nothing but this queue: no `Model`,
  // no `Document`, no `LoadContext` (Constraint 4). The bytes are copied here because the
  // view is valid only for the duration of the callback (Constraint 5).
  const std::lock_guard<std::mutex> lock(d_mutex);
  d_ready.push_back(Arrival{child, std::string(bytes)});
  d_ready_count.store(d_ready.size(), std::memory_order_relaxed);
}

bool PendingExternalLoads::take_arrival(ObjectId child, std::string& bytes) {
  const std::lock_guard<std::mutex> lock(d_mutex);
  for (auto it = d_ready.begin(); it != d_ready.end(); ++it) {
    if (it->child != child) {
      continue;
    }
    bytes = std::move(it->bytes);
    d_ready.erase(it);
    d_ready_count.store(d_ready.size(), std::memory_order_relaxed);
    return true;
  }
  return false;
}

std::vector<PendingExternalLoads::Arrival> PendingExternalLoads::take_ready() {
  const std::lock_guard<std::mutex> lock(d_mutex);
  std::vector<Arrival> out;
  out.swap(d_ready);
  d_ready_count.store(0, std::memory_order_relaxed);
  return out;
}

} // namespace arbc
