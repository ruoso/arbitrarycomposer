#include <arbc/model/model.hpp>
#include <arbc/pool/checkpoint.hpp>
#include <arbc/pool/workspace_file.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// The workspace-backed document rides the file-backed arena (doc 15:156-166). On a
// platform without workspace files this compiles to just the runtime-query check,
// and the anonymous `Model()` -- which is what every other model test uses -- is
// unaffected either way.
TEST_CASE("workspace-backing tests track workspace-file support") {
  REQUIRE(arbc::workspace_files_supported() == (ARBC_HAS_WORKSPACE_FILES != 0));
}

#if ARBC_HAS_WORKSPACE_FILES

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

// A self-cleaning unique workspace-file path under the temp dir (mirrors the pool
// checkpoint / crash tests).
class TempPath {
public:
  TempPath() {
#if defined(_WIN32)
    char dir[MAX_PATH];
    const DWORD n = ::GetTempPathA(MAX_PATH, dir);
    char buf[MAX_PATH];
    if (n != 0 && n < static_cast<DWORD>(MAX_PATH) && ::GetTempFileNameA(dir, "wsb", 0, buf) != 0) {
      d_path = buf;
    }
#else
    char tmpl[] = "/tmp/arbc_wsback_XXXXXX";
    const int fd = ::mkstemp(tmpl);
    if (fd >= 0) {
      ::close(fd);
    }
    d_path = tmpl;
#endif
  }
  ~TempPath() {
    if (!d_path.empty()) {
#if defined(_WIN32)
      ::DeleteFileA(d_path.c_str());
#else
      ::unlink(d_path.c_str());
#endif
    }
  }
  TempPath(const TempPath&) = delete;
  TempPath& operator=(const TempPath&) = delete;
  const std::string& str() const { return d_path; }

private:
  std::string d_path;
};

// Byte-copy the workspace file so recovery runs against an INDEPENDENT file. The
// writer must stay alive across the copy: its teardown would hole-punch the very
// chunks a crash would have left behind (the landed pool idiom).
void copy_file(const std::string& src, const std::string& dst) {
#if defined(_WIN32)
  REQUIRE(::CopyFileA(src.c_str(), dst.c_str(), FALSE) != 0);
#else
  const int in = ::open(src.c_str(), O_RDONLY);
  REQUIRE(in >= 0);
  const int out = ::open(dst.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
  REQUIRE(out >= 0);
  char buf[65536];
  ssize_t n = 0;
  while ((n = ::read(in, buf, sizeof(buf))) > 0) {
    ssize_t off = 0;
    while (off < n) {
      const ssize_t w = ::write(out, buf + off, static_cast<std::size_t>(n - off));
      REQUIRE(w > 0);
      off += w;
    }
  }
  REQUIRE(n == 0);
  ::close(in);
  ::close(out);
#endif
}

// The document under test: one composition whose membership is deliberately pushed
// PAST `k_max_inline_layers`, so the order spills to a HAMT chunk chain and the walk
// has to reach spill-chunk records too (they are ordinary HAMT leaves keyed by their
// own ObjectId -- exactly the arm a walk that only knew about compositions/layers/
// contents would drop on the floor). Returns the composition id; `layers` and
// `contents` collect the members.
constexpr int k_layer_count = 20; // > k_max_inline_layers (8): forces a spill chain

struct Doc {
  arbc::ObjectId composition{};
  std::vector<arbc::ObjectId> layers;
  std::vector<arbc::ObjectId> contents;
};

Doc build_doc(arbc::Model& model) {
  Doc doc;
  {
    auto txn = model.transact();
    doc.composition = txn.add_composition(640.0, 480.0);
    for (int i = 0; i < k_layer_count; ++i) {
      const arbc::ObjectId content = txn.add_content(0x5000u + static_cast<std::uint64_t>(i));
      const arbc::ObjectId layer =
          txn.add_layer(content, arbc::Affine::identity(), 0.5 + 0.01 * i);
      txn.attach_layer(doc.composition, layer);
      doc.contents.push_back(content);
      doc.layers.push_back(layer);
    }
    REQUIRE(txn.commit().has_value());
  }
  model.drain(); // retire the intermediate working roots so live-slot deltas are exact
  return doc;
}

// Detach + remove the topmost layer, then reclaim. The transaction is SCOPED: it
// pins its base version, so the retired version's slots only become unreachable once
// it is destroyed -- draining while it is still alive would reclaim nothing.
void remove_last_layer(arbc::Model& model, const Doc& doc) {
  {
    auto txn = model.transact();
    txn.detach_layer(doc.composition, doc.layers.back());
    txn.remove(doc.layers.back());
    REQUIRE(txn.commit().has_value());
  }
  model.drain();
}

// Assert `version` holds exactly the STRUCTURE `build_doc` built: every id resolves,
// the composition's fields survive, and its MEMBERSHIP ORDER -- which lives in the
// spill chain, i.e. in `LayerOrderChunk` records the walk only reaches because they
// are ordinary HAMT leaves keyed by their own ObjectId -- is preserved end to end.
// Opacity is the one field the crash sweep rewrites, so it is asserted separately.
void assert_doc_intact_ignoring_opacity(const arbc::DocStatePtr& version, const Doc& doc) {
  const arbc::CompositionRecord* comp = version->find_composition(doc.composition);
  REQUIRE(comp != nullptr);
  REQUIRE(comp->canvas_w == 640.0);
  REQUIRE(comp->canvas_h == 480.0);
  REQUIRE(comp->layer_count == static_cast<std::uint32_t>(k_layer_count));
  REQUIRE(comp->spill_root.valid()); // the order really did spill out of line

  std::vector<arbc::ObjectId> order;
  version->for_each_layer_in(doc.composition, [&](arbc::ObjectId id) { order.push_back(id); });
  REQUIRE(order == doc.layers);

  for (int i = 0; i < k_layer_count; ++i) {
    const arbc::LayerRecord* layer = version->find_layer(doc.layers[static_cast<std::size_t>(i)]);
    REQUIRE(layer != nullptr);
    REQUIRE(layer->content == doc.contents[static_cast<std::size_t>(i)]);

    const arbc::ContentRecord* content =
        version->find_content(doc.contents[static_cast<std::size_t>(i)]);
    REQUIRE(content != nullptr);
    REQUIRE(content->kind == 0x5000u + static_cast<std::uint64_t>(i));
  }
}

// The full check: the structure above, plus the opacities `build_doc` stamped.
void assert_doc_intact(const arbc::DocStatePtr& version, const Doc& doc) {
  assert_doc_intact_ignoring_opacity(version, doc);
  for (int i = 0; i < k_layer_count; ++i) {
    const arbc::LayerRecord* layer = version->find_layer(doc.layers[static_cast<std::size_t>(i)]);
    REQUIRE(layer != nullptr);
    REQUIRE(layer->opacity == 0.5 + 0.01 * i);
  }
}

// In-process durable-snapshot injector over the workspace syscalls (the landed
// `pool.crash_tests` shim, driven here against a real document instead of a
// test-local record graph). The two header A/B root slots become durable only when a
// header msync completes, so this pins them to their last-durable values and, at a
// chosen injection point, byte-copies the file with those durable slots patched in --
// exactly the state a crash at that point recovers from.
class SnapshotInjector final : public arbc::SyscallInjector {
public:
  SnapshotInjector(arbc::WorkspaceFileChunkSource& source, std::string path)
      : d_source(&source), d_path(std::move(path)) {}

  void count_only() { reset(-1, false, {}); }
  void arm(long target, bool after_phase, std::string snapshot) {
    reset(target, after_phase, std::move(snapshot));
  }

  long points() const { return d_point; }
  bool captured() const { return d_captured; }

  int before(arbc::WorkspaceSyscall kind, std::uint64_t, std::size_t) noexcept override {
    if (kind == arbc::WorkspaceSyscall::Msync || kind == arbc::WorkspaceSyscall::RootFlip) {
      const long idx = d_point++;
      d_pending_after = false;
      if (idx == d_target && !d_after) {
        capture();
      } else if (idx == d_target && d_after) {
        d_pending_after = true;
      }
    }
    return 0;
  }

  void after(arbc::WorkspaceSyscall kind, std::uint64_t file_offset,
             std::size_t) noexcept override {
    if (kind == arbc::WorkspaceSyscall::Msync && file_offset == 0) {
      // The header msync completed: the flipped root is now durable.
      d_durable_a = d_source->root_slot(0);
      d_durable_b = d_source->root_slot(1);
    }
    if ((kind == arbc::WorkspaceSyscall::Msync || kind == arbc::WorkspaceSyscall::RootFlip) &&
        d_pending_after) {
      capture();
      d_pending_after = false;
    }
  }

private:
  void reset(long target, bool after_phase, std::string snapshot) {
    d_point = 0;
    d_target = target;
    d_after = after_phase;
    d_snapshot = std::move(snapshot);
    d_captured = false;
    d_pending_after = false;
  }

#if defined(_WIN32)
  static bool patch_slot(HANDLE h, std::size_t offset, std::uint64_t value) noexcept {
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(offset);
    if (!::SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) {
      return false;
    }
    DWORD wrote = 0;
    return ::WriteFile(h, &value, sizeof(value), &wrote, nullptr) && wrote == sizeof(value);
  }
#endif

  void capture() {
    if (d_snapshot.empty()) {
      return;
    }
    copy_file(d_path, d_snapshot);
#if defined(_WIN32)
    HANDLE h = ::CreateFileA(d_snapshot.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      const bool ok = patch_slot(h, offsetof(arbc::WorkspaceHeader, root_slot_a), d_durable_a) &&
                      patch_slot(h, offsetof(arbc::WorkspaceHeader, root_slot_b), d_durable_b);
      ::CloseHandle(h);
      if (!ok) {
        return;
      }
    }
#else
    const int fd = ::open(d_snapshot.c_str(), O_RDWR);
    if (fd >= 0) {
      const ssize_t wrote_a = ::pwrite(fd, &d_durable_a, sizeof(d_durable_a),
                                       offsetof(arbc::WorkspaceHeader, root_slot_a));
      const ssize_t wrote_b = ::pwrite(fd, &d_durable_b, sizeof(d_durable_b),
                                       offsetof(arbc::WorkspaceHeader, root_slot_b));
      if (wrote_a != static_cast<ssize_t>(sizeof(d_durable_a)) ||
          wrote_b != static_cast<ssize_t>(sizeof(d_durable_b))) {
        ::close(fd);
        return;
      }
      ::close(fd);
    }
#endif
    d_captured = true;
  }

  arbc::WorkspaceFileChunkSource* d_source;
  std::string d_path;
  std::string d_snapshot;
  std::uint64_t d_durable_a{0};
  std::uint64_t d_durable_b{0};
  long d_point{0};
  long d_target{-1};
  bool d_after{false};
  bool d_pending_after{false};
  bool d_captured{false};
};

// Build the document, checkpoint it durably (V1), then edit it into a distinct V2
// (every layer's opacity rewritten) and checkpoint V2 with the injector armed at the
// (`target`, `after`) syscall boundary. Returns the number of enumerated boundaries
// during the second checkpoint; `snapshot` empty means count-only.
constexpr double k_v2_opacity = 0.125;

long run_second_checkpoint(long target, bool after, const std::string& snapshot, Doc& out_doc) {
  TempPath path;
  auto created = arbc::Model::create(path.str());
  REQUIRE(created.has_value());
  arbc::Model& model = **created;

  // The injector rides the source the Model owns. It must be installed BEFORE the
  // first checkpoint: it learns which root is durable by watching header msyncs
  // complete, so a checkpoint it did not see would leave it believing no root is
  // durable at all -- and it would then patch a never-written root into the
  // snapshot. Arming (below) resets the boundary counter but deliberately keeps the
  // durable state it has observed so far.
  arbc::WorkspaceFileChunkSource& source = *model.workspace_source();
  SnapshotInjector injector(source, path.str());
  source.set_syscall_injector(&injector);

  const Doc doc = build_doc(model);
  out_doc = doc;
  REQUIRE(model.checkpoint().has_value()); // V1 durable (generation 1)

  {
    auto txn = model.transact();
    for (const arbc::ObjectId layer : doc.layers) {
      txn.set_opacity(layer, k_v2_opacity);
    }
    REQUIRE(txn.commit().has_value());
  }
  model.drain();

  if (snapshot.empty()) {
    injector.count_only();
  } else {
    injector.arm(target, after, snapshot);
  }
  REQUIRE(model.checkpoint().has_value()); // V2 (generation 2)
  source.set_syscall_injector(nullptr);    // clear before teardown: `injector` is a local

  if (snapshot.empty()) {
    REQUIRE_FALSE(injector.captured());
  } else {
    REQUIRE(injector.captured());
  }
  return injector.points();
}

} // namespace

TEST_CASE("a workspace-backed model is checkpointable; the anonymous one is unchanged") {
  // Decision 1: backing is a construction-time policy, and `Model()` stays anonymous
  // -- no file, no checkpointer, no slot fence, so a live-only host and every
  // existing model test are untouched.
  arbc::Model anonymous;
  REQUIRE_FALSE(anonymous.workspace_backed());
  REQUIRE(anonymous.checkpointer() == nullptr);
  const arbc::expected<std::monostate, arbc::WorkspaceFileError> refused = anonymous.checkpoint();
  REQUIRE_FALSE(refused.has_value());
  REQUIRE(refused.error().code == arbc::WorkspaceFileErrc::Unsupported);

  TempPath path;
  auto backed = arbc::Model::create(path.str());
  REQUIRE(backed.has_value());
  REQUIRE((*backed)->workspace_backed());
  REQUIRE((*backed)->checkpointer() != nullptr);
  REQUIRE((*backed)->current()->revision() == 0);
}

TEST_CASE("Model::create surfaces an unopenable workspace path as a value") {
  // Errors as values, never a throw or an abort (doc 10). A path under a directory
  // that does not exist cannot be created.
  auto failed = arbc::Model::create("/nonexistent-arbc-dir/nope/doc.arbcws");
  REQUIRE_FALSE(failed.has_value());
  REQUIRE(failed.error().code == arbc::WorkspaceFileErrc::OpenFailed);
}

// enforces: 15-memory-model#counts-rebuilt-by-reachability-walk
// enforces: 15-memory-model#recovery-resumes-last-durable-root
TEST_CASE("a checkpointed document recovers field-identical, with counts rebuilt by the walk") {
  TempPath path;
  Doc doc;
  std::size_t live_before = 0;

  {
    auto created = arbc::Model::create(path.str());
    REQUIRE(created.has_value());
    arbc::Model& model = **created;

    doc = build_doc(model);
    assert_doc_intact(model.current(), doc);
    live_before = model.live_slots();
    REQUIRE(live_before > 0);

    REQUIRE(model.checkpoint().has_value());
    REQUIRE(model.checkpointer()->commit_count() == 1);
    REQUIRE(model.checkpointer()->generation() == 1);

    // Recover from an independent copy taken while the writer is still alive: its
    // teardown would hole-punch chunks a crash would have left behind.
    TempPath recovered;
    copy_file(path.str(), recovered.str());

    auto reopened = arbc::Model::open(recovered.str());
    REQUIRE(reopened.has_value());
    arbc::Model& recovered_model = **reopened;

    // FIELD-IDENTICAL: every id resolves through the recovered version to a record
    // with identical fields, spill-chain membership order included.
    const arbc::DocStatePtr version = recovered_model.current();
    assert_doc_intact(version, doc);

    // The walk rebuilt EXACTLY the reachable slots -- no leak (a slot counted live
    // that no edge reaches) and no under-count (a reachable slot left off the live
    // set, which `finalize_open` would have handed to the free list for the next
    // allocation to overwrite).
    REQUIRE(recovered_model.live_slots() == live_before);
  }
}

// enforces: 15-memory-model#counts-rebuilt-by-reachability-walk
TEST_CASE("post-recovery reclamation frees exactly the unique slots: the in-degrees were right") {
  // Counts are correct, not merely nonzero. A recovered count that is too HIGH leaks
  // (the dropped version's slots never reach zero); one that is too LOW double-frees
  // (asan/the underflow assert catch it). Both are observable as a live-slot
  // divergence from the same edit replayed on a never-checkpointed model -- so run
  // the identical mutation on an anonymous twin and demand the same arithmetic.
  TempPath path;

  arbc::Model reference;
  const Doc ref_doc = build_doc(reference);

  auto created = arbc::Model::create(path.str());
  REQUIRE(created.has_value());
  const Doc doc = build_doc(**created);
  REQUIRE((*created)->checkpoint().has_value());
  REQUIRE((*created)->live_slots() == reference.live_slots()); // same graph, same slots

  TempPath recovered_path;
  copy_file(path.str(), recovered_path.str());
  auto reopened = arbc::Model::open(recovered_path.str());
  REQUIRE(reopened.has_value());
  arbc::Model& recovered = **reopened;
  REQUIRE(recovered.live_slots() == reference.live_slots());
  const std::size_t reference_before = reference.live_slots();

  // The same edit on both: detach and remove half the layers, dropping the old
  // version so its now-unshared path-copied nodes, records and spill chunks reclaim.
  const auto mutate = [](arbc::Model& model, const Doc& d) {
    {
      // Scope the transaction: it pins its BASE version, so the retired version's
      // slots only become unreachable once it is destroyed -- draining while it is
      // still alive would reclaim nothing.
      auto txn = model.transact();
      for (int i = 0; i < k_layer_count / 2; ++i) {
        txn.detach_layer(d.composition, d.layers[static_cast<std::size_t>(i)]);
        txn.remove(d.layers[static_cast<std::size_t>(i)]);
        txn.remove(d.contents[static_cast<std::size_t>(i)]);
      }
      REQUIRE(txn.commit().has_value());
    }
    model.drain();
  };
  mutate(reference, ref_doc);
  mutate(recovered, doc);

  // Reclamation really did retire slots -- established on the anonymous twin, whose
  // counts were never rebuilt and so are true by construction.
  REQUIRE(reference.live_slots() < reference_before);
  // And the recovered document freed EXACTLY the same slots: the walk's in-degrees
  // were right. A count rebuilt too HIGH would have left the retired subtree pinned
  // (a leak -- a higher live count than the twin); one rebuilt too LOW would have
  // dropped a still-referenced slot (a double-free, which asan and the count-
  // underflow assert trip on before this line).
  REQUIRE(recovered.live_slots() == reference.live_slots());
}

// enforces: 15-memory-model#checkpoint-of-still-scene-skips-data-msync
TEST_CASE("a checkpoint of a still scene skips the data msync but still syncs the header") {
  TempPath path;
  auto created = arbc::Model::create(path.str());
  REQUIRE(created.has_value());
  arbc::Model& model = **created;
  build_doc(model);

  REQUIRE(model.checkpoint().has_value());
  arbc::Checkpointer& ckpt = *model.checkpointer();
  const std::uint64_t data_before = ckpt.data_msyncs();
  const std::uint64_t header_before = ckpt.header_msyncs();
  REQUIRE(data_before >= 1); // the first checkpoint DID flush the document's chunks

  // Nothing changed since: no slot allocated, none freed, no chunk grown.
  REQUIRE(model.checkpoint().has_value());

  REQUIRE(ckpt.data_msyncs() - data_before == 0);   // nothing to flush
  REQUIRE(ckpt.header_msyncs() - header_before == 1); // ... but still flip + sync the root
  REQUIRE(ckpt.commit_count() == 2);
  REQUIRE(ckpt.generation() == 2);
}

// enforces: 15-memory-model#freed-slot-quarantined-until-durable
TEST_CASE("a slot freed after a checkpoint is quarantined until the next checkpoint") {
  // Doc 15:209-213: a slot freed AFTER the last durable checkpoint may still be
  // referenced by the on-disk root, so reclamation quarantines it per durability
  // epoch. The fence is installed on BOTH document stores, so this holds for the
  // HamtNode path-copies a commit retires just as much as for object records.
  TempPath path;
  auto created = arbc::Model::create(path.str());
  REQUIRE(created.has_value());
  arbc::Model& model = **created;
  const Doc doc = build_doc(model);
  REQUIRE(model.checkpoint().has_value());

  arbc::Checkpointer& ckpt = *model.checkpointer();
  const std::uint64_t freed_before = ckpt.slots_freed_to_list();
  const std::size_t live_before = model.live_slots();

  // Free some slots: remove a layer and drain the cascade.
  {
    auto txn = model.transact();
    txn.detach_layer(doc.composition, doc.layers.back());
    txn.remove(doc.layers.back());
    REQUIRE(txn.commit().has_value());
  }
  model.drain();

  // The slots ARE released (the live count fell), but they are NOT back on the free
  // list -- the freeing is not durable yet, so the fence is holding them.
  REQUIRE(model.live_slots() < live_before);
  REQUIRE(ckpt.slots_freed_to_list() == freed_before);

  // The checkpoint makes the freeing durable, and the quarantine drains.
  REQUIRE(model.checkpoint().has_value());
  REQUIRE(ckpt.slots_freed_to_list() > freed_before);
}

TEST_CASE("a workspace file with no durable root recovers as an empty document") {
  TempPath path;
  {
    auto created = arbc::Model::create(path.str());
    REQUIRE(created.has_value());
    build_doc(**created); // built, but never checkpointed: nothing is durable
  }
  TempPath recovered;
  copy_file(path.str(), recovered.str());

  auto reopened = arbc::Model::open(recovered.str());
  REQUIRE(reopened.has_value());
  REQUIRE((*reopened)->current()->revision() == 0);
  REQUIRE((*reopened)->live_slots() == 0);
  arbc::ObjectId id{};
  const arbc::CompositionRecord* comp = nullptr;
  REQUIRE_FALSE((*reopened)->current()->find_first_composition(id, comp));
}

TEST_CASE("Model::open surfaces a missing or corrupt workspace file as a value") {
  auto missing = arbc::Model::open("/nonexistent-arbc-dir/nope/doc.arbcws");
  REQUIRE_FALSE(missing.has_value());
  REQUIRE(missing.error().code == arbc::WorkspaceFileErrc::OpenFailed);

  // A file that exists but is not a workspace file.
  TempPath garbage;
  {
    const int fd = ::open(garbage.str().c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    REQUIRE(fd >= 0);
    const char junk[256] = {'n', 'o', 't', ' ', 'a', 'r', 'b', 'c'};
    REQUIRE(::write(fd, junk, sizeof(junk)) == static_cast<ssize_t>(sizeof(junk)));
    ::close(fd);
  }
  auto bad = arbc::Model::open(garbage.str());
  REQUIRE_FALSE(bad.has_value());
  REQUIRE(bad.error().code == arbc::WorkspaceFileErrc::BadMagic);
}

TEST_CASE("recovery continues to serve transactions: ids resume past every recovered record") {
  // Ids are never reused (doc 14 § Identity). The id counter is anonymous runtime
  // state like the counts, so recovery must reseed it PAST every id in the file --
  // otherwise the next transaction mints an id that aliases a recovered record and
  // the HAMT insert silently rebinds it.
  TempPath path;
  Doc doc;
  {
    auto created = arbc::Model::create(path.str());
    REQUIRE(created.has_value());
    doc = build_doc(**created);
    REQUIRE((*created)->checkpoint().has_value());

    TempPath recovered_path;
    copy_file(path.str(), recovered_path.str());
    auto reopened = arbc::Model::open(recovered_path.str());
    REQUIRE(reopened.has_value());
    arbc::Model& recovered = **reopened;

    const arbc::ObjectId fresh = recovered.allocate_id();
    for (const arbc::ObjectId existing : doc.layers) {
      REQUIRE(fresh.value > existing.value);
    }
    for (const arbc::ObjectId existing : doc.contents) {
      REQUIRE(fresh.value > existing.value);
    }
    REQUIRE(fresh.value > doc.composition.value);
    // Spill-chunk records draw from the same counter, so the guarantee has to hold
    // against ids the caller never saw: nothing in the recovered graph may collide.
    REQUIRE_FALSE(recovered.current()->contains(fresh));

    // And the recovered document keeps editing: a fresh layer lands, commits, and
    // the version publishes on top of the recovered baseline.
    {
      auto txn = recovered.transact();
      const arbc::ObjectId content = txn.add_content(0x9999u);
      const arbc::ObjectId layer = txn.add_layer(content, arbc::Affine::identity());
      txn.attach_layer(doc.composition, layer);
      REQUIRE(txn.commit().has_value());
    }
    recovered.drain();
    REQUIRE(recovered.current()->revision() == 1);
    const arbc::CompositionRecord* comp = recovered.current()->find_composition(doc.composition);
    REQUIRE(comp != nullptr);
    REQUIRE(comp->layer_count == static_cast<std::uint32_t>(k_layer_count) + 1);

    // The new version is checkpointable, and recovers as the NEW one.
    REQUIRE(recovered.checkpoint().has_value());
  }
}

TEST_CASE("a checkpoint concurrent with a pinned reader observes an unchanged version") {
  // Doc 16 tier 6, under the asan lane. `checkpoint()` mutates no live record --
  // records are immutable and it only msyncs + flips the header -- so a reader that
  // pins a version and traverses it by `peek` across an arbitrary number of writer
  // commits and checkpoints must never see a torn read or a use-after-free.
  TempPath path;
  auto created = arbc::Model::create(path.str());
  REQUIRE(created.has_value());
  arbc::Model& model = **created;
  const Doc doc = build_doc(model);
  REQUIRE(model.checkpoint().has_value());

  std::atomic<bool> go{false};
  std::atomic<bool> stop{false};
  std::atomic<bool> bad{false};

  std::thread reader([&] {
    while (!go.load(std::memory_order_acquire)) {
    }
    while (!stop.load(std::memory_order_acquire)) {
      const arbc::DocStatePtr pinned = model.current();
      // Traverse the pinned version across whatever the writer is doing: every
      // layer it names must still resolve, with its captured fields intact.
      std::vector<arbc::ObjectId> order;
      pinned->for_each_layer_in(doc.composition, [&](arbc::ObjectId id) { order.push_back(id); });
      for (const arbc::ObjectId id : order) {
        const arbc::LayerRecord* layer = pinned->find_layer(id);
        if (layer == nullptr || !(layer->opacity > 0.0)) {
          bad.store(true, std::memory_order_release);
          return;
        }
      }
    }
  });

  go.store(true, std::memory_order_release);
  constexpr int k_iterations = 200;
  for (int i = 0; i < k_iterations; ++i) {
    {
      auto txn = model.transact();
      txn.set_opacity(doc.layers[static_cast<std::size_t>(i % k_layer_count)],
                      0.25 + 0.001 * (i % 100));
      REQUIRE(txn.commit().has_value());
    }
    model.drain(); // single-drainer: the writer thread between transactions
    REQUIRE(model.checkpoint().has_value());
  }
  stop.store(true, std::memory_order_release);
  reader.join();
  REQUIRE_FALSE(bad.load(std::memory_order_acquire));

  // Every iteration checkpointed, so the durable generation advanced once per commit.
  REQUIRE(model.checkpointer()->commit_count() == k_iterations + 1);
}

// enforces: 15-memory-model#recovery-resumes-last-durable-root
TEST_CASE("checkpoint kill sweep: every syscall boundary recovers a consistent document",
          "[model]") {
  // Doc 16:74-78 / doc 15:205-209. Drive a real document's `checkpoint()` under a
  // kill-at-every-syscall sweep. The publish becomes durable only at the HEADER
  // msync, so a death at any earlier boundary -- and at the header msync's `before`
  // -- must recover the PRIOR committed version (V1), and only a death after it
  // recovers the new one (V2). Both must be field-consistent, with counts correctly
  // rebuilt by the walk (SQLite/LMDB discipline: land on the old root or the new
  // one, never on a half-published graph).
  Doc doc;
  const long num_points = run_second_checkpoint(0, false, {}, doc); // count-only pass
  REQUIRE(num_points >= 3); // >= 1 data msync + the root flip + the header msync

  // The yardstick: an ANONYMOUS twin of the same document given the identical edit.
  // Its counts were never rebuilt by a walk -- they are true by construction -- so
  // its slot arithmetic is what a correctly recovered document must reproduce. V1 and
  // V2 differ only in opacity, never in shape, so one twin serves both roots.
  std::size_t twin_live = 0;
  {
    arbc::Model twin;
    const Doc twin_doc = build_doc(twin);
    remove_last_layer(twin, twin_doc);
    twin_live = twin.live_slots();
  }

  for (long target = 0; target < num_points; ++target) {
    for (const bool after : {false, true}) {
      TempPath snapshot;
      Doc swept;
      const long points = run_second_checkpoint(target, after, snapshot.str(), swept);
      REQUIRE(points == num_points); // every re-run enumerates the same boundaries

      auto reopened = arbc::Model::open(snapshot.str());
      REQUIRE(reopened.has_value());
      arbc::Model& recovered = **reopened;

      // Only the header msync's `after` publishes V2; every earlier boundary lands
      // on V1.
      const bool is_new = (target == num_points - 1) && after;
      REQUIRE(recovered.checkpointer()->generation() == (is_new ? 2u : 1u));

      // Field-consistent either way. The pin is SCOPED: it keeps the recovered
      // version -- and so its whole reachable subtree -- alive, which is exactly what
      // a pin is for, but it would also stop the edit below from reclaiming anything.
      {
        const arbc::DocStatePtr version = recovered.current();
        // The whole document resolves, spill-chain membership order included ...
        assert_doc_intact_ignoring_opacity(version, swept);

        // ... and the recovered opacities are ENTIRELY V1's or ENTIRELY V2's -- never
        // a half-published mixture, which is what a torn publish would produce.
        for (int i = 0; i < k_layer_count; ++i) {
          const arbc::LayerRecord* layer =
              version->find_layer(swept.layers[static_cast<std::size_t>(i)]);
          REQUIRE(layer != nullptr);
          REQUIRE(layer->opacity == (is_new ? k_v2_opacity : 0.5 + 0.01 * i));
        }
      }

      // The counts the walk rebuilt against THIS root are right, at every boundary:
      // the recovered document keeps editing, and the edit reclaims with exactly the
      // slot arithmetic of a never-checkpointed twin -- no leak, no double-free.
      CAPTURE(target, after, is_new);
      remove_last_layer(recovered, swept);
      REQUIRE(recovered.current()->find_layer(swept.layers.back()) == nullptr);
      REQUIRE(recovered.live_slots() == twin_live);
    }
  }
}


#endif // ARBC_HAS_WORKSPACE_FILES
