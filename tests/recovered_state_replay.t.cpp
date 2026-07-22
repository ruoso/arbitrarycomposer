// End-to-end proof of the per-kind state-slab walk hook (issue #5,
// model.persistent_state_walk_hook): a workspace document persists a non-inert
// content StateHandle, is reopened via the fast map path, and the runtime replay
// routes the recovered handle to the owning kind's registered walker -- called with
// the right document-level store, owning ObjectId, and persisted handle. This
// proves the SEAM fires with correct routing; the durable slab recovery a real
// kind's walker performs is the separate kinds.raster_workspace_backing follow-on.

#include <arbc/contract/registry.hpp>
#include <arbc/model/model.hpp>
#include <arbc/model/records.hpp>
#include <arbc/pool/workspace_file.hpp>
#include <arbc/runtime/recovered_state_replay.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if ARBC_HAS_WORKSPACE_FILES
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif
#endif

namespace {

using arbc::ObjectId;
using arbc::StateHandle;

#if ARBC_HAS_WORKSPACE_FILES

// A temp workspace-file path, cleaned up on teardown (the document_workspace_
// checkpoint.t.cpp recipe): mkstemp/unlink on POSIX, GetTempFileNameA/DeleteFileA
// on Windows.
class TempPath {
public:
  TempPath() {
#if defined(_WIN32)
    char dir[MAX_PATH];
    const DWORD n = ::GetTempPathA(MAX_PATH, dir);
    char buf[MAX_PATH];
    if (n != 0 && n < static_cast<DWORD>(MAX_PATH) && ::GetTempFileNameA(dir, "rsr", 0, buf) != 0) {
      d_path = buf;
    }
#else
    char tmpl[] = "/tmp/arbc_rsr_XXXXXX";
    const int fd = ::mkstemp(tmpl);
    if (fd >= 0) {
      ::close(fd);
    }
    d_path = tmpl;
#endif
  }
  ~TempPath() {
#if defined(_WIN32)
    ::DeleteFileA(d_path.c_str());
#else
    ::unlink(d_path.c_str());
#endif
  }
  TempPath(const TempPath&) = delete;
  TempPath& operator=(const TempPath&) = delete;
  const std::string& str() const noexcept { return d_path; }

private:
  std::string d_path;
};

// The walker is a plain function pointer (KindStateWalker mirrors KindBinder's
// static-thunk idiom), so it records what it was handed into file-static state the
// test reads back. Reset per case.
struct WalkCall {
  void* store;
  ObjectId content;
  StateHandle handle;
};
std::vector<WalkCall> g_calls;

void record_reach(void* store, ObjectId content, StateHandle handle) {
  g_calls.push_back(WalkCall{store, content, handle});
}

// Create a workspace document that stamps one non-inert StateHandle on a content of
// kind `token` (plus an inert sibling), checkpoints, and cleanly closes. Reopening
// the same path then recovers the durable root. Returns the reopened Model and the
// content that carries state.
struct Reopened {
  std::unique_ptr<arbc::Model> model;
  ObjectId with_state;
};

Reopened make_and_reopen(const TempPath& path, std::uint64_t token, StateHandle handle) {
  ObjectId with_state;
  {
    auto created = arbc::Model::create(path.str());
    REQUIRE(created.has_value());
    arbc::Model& model = **created;
    {
      auto txn = model.transact();
      with_state = txn.add_content(token);
      (void)txn.add_content(token); // a sibling that stays inert -> must not be collected
      REQUIRE(txn.commit().has_value());
    }
    {
      auto txn = model.transact();
      txn.set_content_state(with_state, handle);
      REQUIRE(txn.commit().has_value());
    }
    REQUIRE(model.checkpoint().has_value());
  } // clean close: the durable root survives, reopenable on the same path

  auto reopened = arbc::Model::open(path.str());
  REQUIRE(reopened.has_value());
  return Reopened{std::move(*reopened), with_state};
}

#endif // ARBC_HAS_WORKSPACE_FILES

} // namespace

#if ARBC_HAS_WORKSPACE_FILES

TEST_CASE("the recovery replay routes each recovered handle to its kind's walker") {
  g_calls.clear();

  constexpr std::uint64_t k_token = 0xBEEFu; // this document's kind token for the test kind
  constexpr std::string_view k_kind_id = "test.persistent";
  int store_object = 0; // stands in for the kind's document-level state store

  arbc::Registry registry;
  REQUIRE(registry
              .add(k_kind_id, /*factory=*/{}, /*metadata=*/{}, /*codec=*/std::nullopt,
                   /*binder=*/std::nullopt, arbc::KindStateWalker{&record_reach})
              .has_value());

  TempPath path;
  Reopened r = make_and_reopen(path, k_token, StateHandle{9});
  REQUIRE(r.model->recovered_content_state().size() == 1);

  arbc::RecoveredStateHooks hooks;
  hooks.kind_id_of = [&](std::uint64_t kind) -> std::string_view {
    return kind == k_token ? k_kind_id : std::string_view{};
  };
  hooks.store_of = [&](std::string_view id) -> void* {
    return id == k_kind_id ? &store_object : nullptr;
  };

  const arbc::RecoveredStateReplayStats stats =
      arbc::replay_recovered_content_state(r.model->recovered_content_state(), registry, hooks);

  REQUIRE(stats.dispatched == 1);
  REQUIRE(stats.skipped == 0);
  REQUIRE(g_calls.size() == 1);
  REQUIRE(g_calls[0].store == &store_object); // the kind's own document-level store
  REQUIRE(g_calls[0].content == r.with_state);
  REQUIRE(g_calls[0].handle == StateHandle{9});
}

TEST_CASE("the recovery replay skips a handle whose kind registered no walker") {
  g_calls.clear();

  constexpr std::uint64_t k_token = 0x1234u;
  constexpr std::string_view k_kind_id = "test.walkerless";

  arbc::Registry registry; // factory-only registration: no state walker
  REQUIRE(registry.add(k_kind_id, /*factory=*/{}).has_value());

  TempPath path;
  Reopened r = make_and_reopen(path, k_token, StateHandle{3});
  REQUIRE(r.model->recovered_content_state().size() == 1);

  arbc::RecoveredStateHooks hooks;
  hooks.kind_id_of = [&](std::uint64_t kind) -> std::string_view {
    return kind == k_token ? k_kind_id : std::string_view{};
  };
  hooks.store_of = [](std::string_view) -> void* { return nullptr; };

  const arbc::RecoveredStateReplayStats stats =
      arbc::replay_recovered_content_state(r.model->recovered_content_state(), registry, hooks);

  REQUIRE(stats.dispatched == 0);
  REQUIRE(stats.skipped == 1); // no walker -> counted, never fatal
  REQUIRE(g_calls.empty());
}

TEST_CASE("the recovery replay skips a handle whose kind token does not resolve") {
  g_calls.clear();

  arbc::Registry registry;
  REQUIRE(registry
              .add("test.persistent", /*factory=*/{}, /*metadata=*/{}, /*codec=*/std::nullopt,
                   /*binder=*/std::nullopt, arbc::KindStateWalker{&record_reach})
              .has_value());

  TempPath path;
  Reopened r = make_and_reopen(path, /*token=*/0x9999u, StateHandle{5});
  REQUIRE(r.model->recovered_content_state().size() == 1);

  // An unknown kind token (a kind this build does not know) resolves to nothing --
  // a recovered document opens even holding a since-removed kind's remnants.
  arbc::RecoveredStateHooks hooks;
  hooks.kind_id_of = [](std::uint64_t) -> std::string_view { return {}; };
  hooks.store_of = [](std::string_view) -> void* { return nullptr; };

  const arbc::RecoveredStateReplayStats stats =
      arbc::replay_recovered_content_state(r.model->recovered_content_state(), registry, hooks);

  REQUIRE(stats.dispatched == 0);
  REQUIRE(stats.skipped == 1);
  REQUIRE(g_calls.empty());
}

#endif // ARBC_HAS_WORKSPACE_FILES
