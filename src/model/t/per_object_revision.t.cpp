// Per-object revision stamps (`model.per_object_revision`, doc 01 § Identity and
// versioning, doc 14 § Per-object revisions). Every object carries its own revision
// stamp, minted from the publishing revision at the copy-on-write every mutation already
// performs -- so a commit stamps exactly the objects it touched, every untouched object
// keeps its stamp by structural sharing, and the tile cache key can name THAT stamp
// instead of the document-global revision. These cases pin the model half: the stamp
// write and the two primitives (`object_revision`, `composition_revision`) the runtime
// projects into the cache key.

#include <arbc/model/journal.hpp>
#include <arbc/model/model.hpp>
#include <arbc/model/records.hpp>
#include <arbc/pool/workspace_file.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>

#if ARBC_HAS_WORKSPACE_FILES && !defined(_WIN32)
#include <unistd.h>
#endif

namespace {

// A three-layer scene: one composition, three (content, layer) pairs attached in order.
// Enough that "the commit touched ONE of them" has two untouched witnesses.
struct Scene {
  arbc::ObjectId composition;
  arbc::ObjectId content[3];
  arbc::ObjectId layer[3];
};

Scene build_scene(arbc::Model& model) {
  Scene s;
  auto txn = model.transact();
  s.composition = txn.add_composition(256.0, 256.0);
  for (int i = 0; i < 3; ++i) {
    s.content[i] = txn.add_content(1);
    s.layer[i] = txn.add_layer(s.content[i], arbc::Affine::identity(), 1.0);
    txn.attach_layer(s.composition, s.layer[i]);
  }
  REQUIRE(txn.commit().has_value());
  return s;
}

// The record edge `id` is bound to in `state` -- the `SlotRef` identity that witnesses
// structural sharing (`DocRoot::record_edge` exists for exactly this proof).
arbc::SlotRef<arbc::ObjectRecord> edge_of(const arbc::DocStatePtr& state, arbc::ObjectId id) {
  arbc::SlotRef<arbc::ObjectRecord> edge;
  REQUIRE(state->record_edge(id, edge));
  return edge;
}

} // namespace

// enforces: 14-data-model-and-editing#commit-stamps-only-touched-objects
TEST_CASE("a commit stamps exactly the objects it touched, and shares the rest untouched") {
  arbc::Model model;
  const Scene scene = build_scene(model);
  const arbc::DocStatePtr before = model.current();

  // The build commit published revision 1, so every object it created carries stamp 1.
  REQUIRE(before->revision() == 1);
  for (int i = 0; i < 3; ++i) {
    CHECK(before->object_revision(scene.layer[i]) == 1);
    CHECK(before->object_revision(scene.content[i]) == 1);
  }
  CHECK(before->object_revision(scene.composition) == 1);

  // Touch exactly ONE layer.
  {
    auto txn = model.transact();
    txn.set_opacity(scene.layer[1], 0.5);
    REQUIRE(txn.commit().has_value());
  }
  const arbc::DocStatePtr after = model.current();
  REQUIRE(after->revision() == 2);

  // The touched object carries the PUBLISHED revision as its new stamp...
  CHECK(after->object_revision(scene.layer[1]) == 2);
  // ...and every untouched object keeps its PRIOR stamp with no write at all -- proven
  // not merely by the value but by `SlotRef` identity: the record itself is the same
  // record, shared from the pre-commit version (#commit-shares-untouched-structure). That
  // is the whole mechanism: the stamp costs no traversal because it rides a path-copy
  // that already happened, and an untouched object is never visited.
  CHECK(after->object_revision(scene.layer[0]) == 1);
  CHECK(after->object_revision(scene.layer[2]) == 1);
  CHECK(edge_of(after, scene.layer[0]) == edge_of(before, scene.layer[0]));
  CHECK(edge_of(after, scene.layer[2]) == edge_of(before, scene.layer[2]));
  // The layer's CONTENT is a different object and a placement edit does not touch it --
  // which is precisely why an unedited layer's tiles survive an edit elsewhere.
  for (int i = 0; i < 3; ++i) {
    CHECK(after->object_revision(scene.content[i]) == 1);
    CHECK(edge_of(after, scene.content[i]) == edge_of(before, scene.content[i]));
  }

  // The touched record is a DIFFERENT record (path-copied), as it must be for the old
  // one to survive in the journal carrying its old stamp.
  CHECK_FALSE(edge_of(after, scene.layer[1]) == edge_of(before, scene.layer[1]));
}

TEST_CASE("object_revision and composition_revision are well-defined no-ops off-target") {
  arbc::Model model;
  const Scene scene = build_scene(model);
  const arbc::DocStatePtr state = model.current();

  // An absent id has no record and therefore no stamp: 0, never a stale key. Nothing
  // renders under an id the document does not contain, so this can serve no tile.
  CHECK(state->object_revision(arbc::ObjectId{0xDEADBEEF}) == 0);
  CHECK(state->composition_revision(arbc::ObjectId{0xDEADBEEF}) == 0);
  // An id bound to a non-composition record has no arrangement: 0.
  CHECK(state->composition_revision(scene.layer[0]) == 0);
  CHECK(state->composition_revision(scene.content[0]) == 0);
  // A real composition folds to something -- and specifically not to the degenerate 0.
  CHECK(state->composition_revision(scene.composition) != 0);
}

// enforces: 05-recursive-composition#composition-arrangement-joins-the-contribution
TEST_CASE("composition_revision moves on every arrangement edit the inputs() fold cannot see") {
  arbc::Model model;
  const Scene scene = build_scene(model);

  const auto arrangement = [&]() {
    return model.current()->composition_revision(scene.composition);
  };
  const std::uint64_t base = arrangement();

  SECTION("a member layer's placement edits move it -- none of them touch a child content") {
    // Transform, opacity, span and time map all live on `LayerRecord`, which no child
    // CONTENT's stamp can see and the compositor's `inputs()` fold cannot reach. Each is
    // a distinct edit and each must move the embedder's key, or the parent's composed
    // result is served from a pre-edit cache entry.
    std::uint64_t prev = base;

    {
      auto txn = model.transact();
      txn.set_transform(scene.layer[1], arbc::Affine::translation(5.0, 0.0));
      REQUIRE(txn.commit().has_value());
    }
    CHECK(arrangement() != prev);
    prev = arrangement();

    {
      auto txn = model.transact();
      txn.set_opacity(scene.layer[1], 0.25);
      REQUIRE(txn.commit().has_value());
    }
    CHECK(arrangement() != prev);
    prev = arrangement();

    {
      auto txn = model.transact();
      txn.set_span(scene.layer[1], arbc::TimeRange{arbc::Time{0}, arbc::Time{1000}});
      REQUIRE(txn.commit().has_value());
    }
    CHECK(arrangement() != prev);
    prev = arrangement();

    {
      auto txn = model.transact();
      arbc::TimeMap tm;
      tm.offset = arbc::Time{7};
      txn.set_time_map(scene.layer[1], tm);
      REQUIRE(txn.commit().has_value());
    }
    CHECK(arrangement() != prev);
  }

  SECTION("a reorder moves it") {
    auto txn = model.transact();
    txn.reorder_layer(scene.composition, 0, 2);
    REQUIRE(txn.commit().has_value());
    CHECK(arrangement() != base);
  }

  SECTION("an attach and a detach move it") {
    arbc::ObjectId extra_layer;
    {
      auto txn = model.transact();
      const arbc::ObjectId c = txn.add_content(1);
      extra_layer = txn.add_layer(c, arbc::Affine::identity(), 1.0);
      txn.attach_layer(scene.composition, extra_layer);
      REQUIRE(txn.commit().has_value());
    }
    const std::uint64_t attached = arrangement();
    CHECK(attached != base);

    {
      auto txn = model.transact();
      txn.detach_layer(scene.composition, extra_layer);
      REQUIRE(txn.commit().has_value());
    }
    // The detach restores the membership SET, but not the arrangement value: the
    // composition record was path-copied twice, so its own stamp advanced. Returning to
    // the pre-attach key would mean serving the pre-attach composite, which happens to be
    // right here and would NOT be right after an intervening member edit -- the fold is
    // deliberately a function of the stamps, not of the membership set.
    CHECK(arrangement() != attached);
  }

  SECTION("a member layer's CONTENT edit does NOT move it -- the inputs() fold covers that") {
    // The division of labour: the arrangement fold covers what no content's stamp can see;
    // the compositor's `inputs()` fold covers the contents themselves. Double-counting a
    // content here would be harmless but redundant; MISSING it in both is the wrong-pixel
    // path, and that is what the other sections guard.
    auto txn = model.transact();
    txn.set_content_state(scene.content[1], arbc::StateHandle{});
    REQUIRE(txn.commit().has_value());
    CHECK(arrangement() == base);
  }
}

// enforces: 05-recursive-composition#composition-arrangement-joins-the-contribution
TEST_CASE("composition_revision folds the spill chain, not just the inline arm") {
  // Past `k_max_inline_layers` the membership lives in a `LayerOrderChunk` chain and the
  // inline array is dead. A fold that walked only `layers[]` would silently stop
  // detecting arrangement edits on exactly the compositions big enough to have them.
  arbc::Model model;
  const std::size_t n = arbc::k_max_inline_layers + 4;
  arbc::ObjectId composition;
  std::vector<arbc::ObjectId> layers;
  {
    auto txn = model.transact();
    composition = txn.add_composition(256.0, 256.0);
    for (std::size_t i = 0; i < n; ++i) {
      const arbc::ObjectId c = txn.add_content(1);
      layers.push_back(txn.add_layer(c, arbc::Affine::identity(), 1.0));
      txn.attach_layer(composition, layers.back());
    }
    REQUIRE(txn.commit().has_value());
  }
  // The membership really did spill.
  const arbc::CompositionRecord* rec = model.current()->find_composition(composition);
  REQUIRE(rec != nullptr);
  REQUIRE(rec->layer_count == n);
  REQUIRE(rec->spill_root.valid());

  const auto arrangement = [&]() { return model.current()->composition_revision(composition); };
  const std::uint64_t base = arrangement();

  // A placement edit on a member living in the SPILL CHAIN (the last one, well past the
  // inline cap) moves the fold. The composition's own record is untouched by this edit --
  // only the layer's is -- so this is exactly the case the member walk exists for.
  {
    auto txn = model.transact();
    txn.set_opacity(layers.back(), 0.5);
    REQUIRE(txn.commit().has_value());
  }
  CHECK(arrangement() != base);
}

// enforces: 14-data-model-and-editing#undo-restores-the-prior-stamp
TEST_CASE("undo restores the prior stamp while the document revision advances") {
  arbc::Model model;
  const Scene scene = build_scene(model);
  arbc::Journal journal(model);
  model.set_commit_sink(&journal);

  const std::uint64_t stamp_at_1 = model.current()->object_revision(scene.layer[1]);
  const arbc::SlotRef<arbc::ObjectRecord> record_at_1 = edge_of(model.current(), scene.layer[1]);
  REQUIRE(stamp_at_1 == 1);

  {
    auto txn = model.transact();
    txn.set_opacity(scene.layer[1], 0.5);
    REQUIRE(txn.commit().has_value());
  }
  const std::uint64_t stamp_at_2 = model.current()->object_revision(scene.layer[1]);
  REQUIRE(model.current()->revision() == 2);
  REQUIRE(stamp_at_2 == 2);

  // UNDO. The document revision goes FORWARD (undo is an ordinary forward publish), but
  // the touched object's stamp goes BACK to what it was -- because `navigate()`
  // republishes the stored owning edge by `SlotRef` identity and never re-creates a
  // record, so the pre-edit record travels back carrying its own stamp. That is why the
  // pre-edit tiles become legitimately reachable again: they were rendered from exactly
  // this record. A per-object stamp is therefore NOT globally monotone, and that is
  // correct rather than a hazard (doc 14 § Render purity).
  REQUIRE(journal.undo());
  CHECK(model.current()->revision() == 3);
  CHECK(model.current()->object_revision(scene.layer[1]) == stamp_at_1);
  // ...and it is the VERY SAME RECORD the pre-edit version held, by `SlotRef` identity --
  // which is what makes the restored stamp sound rather than merely equal: a tile keyed on
  // it shows exactly the pixels this record produces.
  CHECK(edge_of(model.current(), scene.layer[1]) == record_at_1);

  // REDO returns every stamp to its post-commit value (the stamp twin of
  // #undo-redo-round-trips).
  REQUIRE(journal.redo());
  CHECK(model.current()->revision() == 4);
  CHECK(model.current()->object_revision(scene.layer[1]) == stamp_at_2);

  // The untouched siblings never moved through any of it.
  CHECK(model.current()->object_revision(scene.layer[0]) == 1);
  CHECK(model.current()->object_revision(scene.layer[2]) == 1);
  model.set_commit_sink(nullptr);
}

TEST_CASE("the per-object stamp keeps every ObjectRecord slab property") {
  // The record grew by 8 bytes; doc 15 pins record PROPERTIES (standard-layout,
  // fixed-size, pointer-free, trivially destructible/copyable), never a field list, so
  // the growth is sanctioned exactly as long as these still hold. The stride change is
  // what makes an old workspace file get REFUSED as a value on reopen, which is the
  // designed behavior for this class of change (doc 15:240-256).
  static_assert(std::is_standard_layout_v<arbc::ObjectRecord>);
  static_assert(std::is_trivially_destructible_v<arbc::ObjectRecord>);
  static_assert(std::is_trivially_copyable_v<arbc::ObjectRecord>);
  static_assert(std::is_same_v<decltype(arbc::ObjectRecord::revision), std::uint64_t>);
  static_assert(sizeof(arbc::LayerOrderChunk) <= sizeof(arbc::CompositionRecord));
  SUCCEED();
}

#if ARBC_HAS_WORKSPACE_FILES

namespace {

// A self-cleaning unique workspace-file path under the temp dir (mirrors
// workspace_backing.t.cpp's).
class TempPath {
public:
  TempPath() {
    static std::atomic<int> counter{0};
    d_path = std::string("/tmp/arbc_por_") + std::to_string(counter.fetch_add(1)) + "_" +
             std::to_string(static_cast<long long>(::getpid())) + ".arbcws";
  }
  ~TempPath() { std::remove(d_path.c_str()); }
  TempPath(const TempPath&) = delete;
  TempPath& operator=(const TempPath&) = delete;
  const std::string& str() const { return d_path; }

private:
  std::string d_path;
};

} // namespace

// enforces: 15-memory-model#recovery-resumes-above-every-persisted-stamp
TEST_CASE("reopening a workspace file resumes the document above every persisted stamp") {
  TempPath path;
  arbc::ObjectId layer;
  std::uint64_t persisted_max = 0;

  {
    auto created = arbc::Model::create(path.str());
    REQUIRE(created.has_value());
    arbc::Model& model = **created;

    const Scene scene = build_scene(model);
    layer = scene.layer[1];
    // Commit a few more times so the persisted stamps are well above zero and the
    // "reopen at 0" bug would be flagrant rather than marginal.
    for (int i = 0; i < 4; ++i) {
      auto txn = model.transact();
      txn.set_opacity(layer, 0.5 - 0.1 * i);
      REQUIRE(txn.commit().has_value());
    }
    persisted_max = model.current()->object_revision(layer);
    REQUIRE(persisted_max == 5); // build(1) + four opacity commits
    REQUIRE(model.current()->revision() == 5);
    REQUIRE(model.checkpoint().has_value());
  }

  auto reopened = arbc::Model::open(path.str());
  REQUIRE(reopened.has_value());
  arbc::Model& recovered = **reopened;

  // The stamps ride INSIDE the records, so they persist -- and the recovered document
  // therefore must NOT reopen at revision 0. If it did, a commit in the recovered session
  // would re-mint a stamp a still-reachable record already carries, keying two different
  // renderings of one object alike.
  CHECK(recovered.current()->object_revision(layer) == persisted_max);
  CHECK(recovered.current()->revision() == persisted_max + 1);

  // And the very next commit mints a stamp strictly above every recovered one.
  {
    auto txn = recovered.transact();
    txn.set_opacity(layer, 0.9);
    REQUIRE(txn.commit().has_value());
  }
  CHECK(recovered.current()->object_revision(layer) > persisted_max);
}

#endif // ARBC_HAS_WORKSPACE_FILES
