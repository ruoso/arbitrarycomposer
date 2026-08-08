#include <arbc/runtime/document.hpp>

#include <memory>
#include <utility>

namespace arbc {

HousekeepingConfig Document::policy_for(const DocumentHousekeepingConfig& config,
                                        bool workspace_backed) {
  HousekeepingConfig policy;
  policy.drain_between_transactions = config.drain_between_transactions;
  // The transaction-count trigger fires from `after_commit`, i.e. on the writer --
  // the only thread a `Checkpointer::commit` may ever run on. Inert without a file.
  if (workspace_backed && config.checkpoint_every_n_transactions > 0) {
    policy.checkpoint_every_n_transactions = config.checkpoint_every_n_transactions;
  }
  // `checkpoint_tick_interval` is deliberately LEFT EMPTY, always: it is the one
  // trigger that fires from the background thread's `tick()`, and a commit there
  // races the writer's allocator (doc 15 § Version reclamation). The background
  // thread drains; the writer checkpoints.
  return policy;
}

Document::Document(const DocumentHousekeepingConfig& housekeeping)
    : Document(std::make_unique<Model>(), housekeeping) {}

Document::Document(std::unique_ptr<Model> model, const DocumentHousekeepingConfig& housekeeping)
    // The config is BORROWED and `housekeeping.thread` COPIED out of it, never
    // moved: two arguments of one call have unspecified evaluation order, and a
    // move would leave `policy_for` reading a moved-from struct if it happened to
    // run second.
    : d_model(std::move(model)),
      d_housekeeping(d_hk_target, policy_for(housekeeping, d_model->workspace_backed()),
                     housekeeping.thread) {
  // The document owns the single, document-wide history (doc 14:193-195): every
  // Document mutator's commit appends one entry and is undoable. The relay -- not the
  // journal itself -- is the model's CommitSink, so the same post-publish hook also
  // drives the housekeeper's between-transaction drain and checkpoint cadence for
  // EVERY commit, including one a host makes through a raw `transact()`.
  d_model->set_commit_sink(&d_commit_relay);
  d_binding.attach(*d_model, d_journal);
  // The binding's teardown drains must go through the single drainer, not around it
  // (Constraint 2): `unbind`/`unbind_all` would otherwise race the background loop.
  d_binding.set_drain_hook([this] { d_housekeeping.drain_and_quiesce(); });
  // Complete the teardown a removal deferred, at the moment it becomes safe
  // (`runtime.removed_content_reclaim`, issue #26). `remove_content` deliberately
  // RETAINS the live `Content*` and its binding row, because the erased record's
  // deferred `StateHandle` release must still route to a live row while the journal
  // holds the removal -- so a long editing session that repeatedly inserts and deletes
  // large content grew monotonically until close, even after history trims.
  //
  // The safe moment is the LAST release: zero outstanding retains means no live
  // `ContentRecord` instance -- not in the current version, not in a pinned snapshot,
  // not behind a journal edge -- still embeds this content's state. Nothing can route
  // to it any more, so nothing can strand. That is a stronger condition than "absent
  // from the current version", which a pinned render or an undoable removal can both
  // falsify.
  d_binding.set_reclaim_hook([this](ObjectId content) { reclaim_removed_content(content); });
}

// Out-of-line so `~HousekeepingThread` (which joins the loop) is instantiated here,
// with the whole member set complete -- the teardown contract in document.hpp.
Document::~Document() {
  // Go inert BEFORE any member is destroyed (issue #26): the teardown drain releases
  // every content's state, and the reclaim hook it would fire walks the binding and the
  // copy-on-write map -- both mid-destruction, and both about to be freed wholesale
  // anyway. The declaration-order contract does the real work; this just keeps the
  // shortening path out of it.
  d_tearing_down.store(true, std::memory_order_release);
}

expected<std::unique_ptr<Document>, WorkspaceFileError>
Document::create(const std::string& path, const DocumentHousekeepingConfig& housekeeping) {
  expected<std::unique_ptr<Model>, WorkspaceFileError> model = Model::create(path);
  if (!model) {
    return unexpected(model.error());
  }
  return std::unique_ptr<Document>(new Document(std::move(*model), housekeeping));
}

expected<std::unique_ptr<Document>, WorkspaceFileError>
Document::open(const std::string& path, const DocumentHousekeepingConfig& housekeeping) {
  expected<std::unique_ptr<Model>, WorkspaceFileError> model = Model::open(path);
  if (!model) {
    return unexpected(model.error());
  }
  return std::unique_ptr<Document>(new Document(std::move(*model), housekeeping));
}

void Document::CommitRelay::on_commit(JournalEntry entry) {
  d_doc->d_journal.on_commit(std::move(entry));

  // The between-transaction drain + the transaction-count checkpoint cadence, on the
  // writer thread, synchronized against the background loop by the thread's mutex.
  expected<std::monostate, WorkspaceFileError> kept = d_doc->d_housekeeping.after_commit();
  if (!kept) {
    // A cadence commit's I/O failure has no caller to return a value to (the publish
    // itself succeeded and is not being unwound): record it, never throw off a commit.
    d_doc->d_last_checkpoint_error = kept.error();
    return;
  }
  d_doc->d_last_checkpoint_error.reset();
}

// Everything `add_content` does INSIDE its transaction, for the one content: mint the
// record, register the `Editable` routing and capture its initial state onto the record,
// and capture the construction identity + input edges. Shared verbatim with
// `create_content_and_attach` so the two cannot drift -- the whole point of that verb is
// that a placed object is created the same way, in one entry rather than two.
ObjectId Document::mint_content(Model::Transaction& txn, Content& live, std::uint64_t kind) {
  const ObjectId id = txn.add_content(kind);
  if (Editable* editable = d_binding.bind(id, live)) {
    const StateHandle initial = editable->capture();
    if (initial.has_state()) {
      txn.set_content_state(id, initial);
    }
  }
  if (d_identity_capture) {
    std::string kind_id;
    std::string params;
    if (d_identity_capture(live, kind, kind_id, params)) {
      txn.set_content_identity(id, kind_id, params);
      // Remember what this object was BUILT from (issue #34): it is what makes a later
      // staleness check a string compare instead of a re-capture, and a content added before
      // any hook was installed simply has no entry and is never considered stale.
      d_bound_params.insert_or_assign(id, params);
    }
  }
  // Copy the edges out under the kind's storage lock (`for_each_input`) before the
  // id mapping below, which is a linear scan of the bindings map and has no business
  // running under that lock. This is the writer thread, concurrent with the render
  // thread's own walks, so the unlocked span form would be a read of freed storage.
  std::vector<ContentRef> inputs;
  for_each_input(live, [&](std::size_t /*index*/, ContentRef in) { inputs.push_back(in); });
  if (!inputs.empty()) {
    const std::shared_ptr<const ContentBindings> snap = d_contents.load();
    std::vector<ObjectId> input_ids;
    input_ids.reserve(inputs.size());
    for (const ContentRef in : inputs) {
      ObjectId found{};
      for (const auto& [bound_id, bound] : *snap) {
        if (bound.get() == in) {
          found = bound_id;
          break;
        }
      }
      input_ids.push_back(found);
    }
    txn.set_content_inputs(id, input_ids);
  }
  return id;
}

// Publish one id->Content row copy-on-write (issue #10). The whole-map copy is near-free
// -- the values are `shared_ptr` and this is rare -- and it is what lets `resolve()` and
// `for_each_content()` be lock-free pinned reads on the render thread.
void Document::publish_binding(ObjectId id, std::shared_ptr<Content> content) {
  auto next = std::make_shared<ContentBindings>(*d_contents.load());
  next->insert_or_assign(id, std::move(content));
  d_contents.store(std::move(next));
}

Document::Placed Document::create_content_and_attach(std::shared_ptr<Content> content,
                                                     std::uint64_t kind, ObjectId composition,
                                                     const Affine& transform, double opacity) {
  // ONE transaction for the whole user-visible action (issue #20): the content record,
  // its placement, and its membership publish together, so there is no intermediate
  // version in which a content exists attached to nothing and no second undo press.
  Content& live = *content;
  auto txn = begin();
  const ObjectId id = mint_content(txn, live, kind);
  const ObjectId layer = txn.add_layer(id, transform, opacity);
  txn.attach_layer(composition, layer);
  txn.commit();
  publish_binding(id, std::move(content));
  return Placed{id, layer};
}

void Document::remove_contents(std::span<const Removal> removals) {
  if (removals.empty()) {
    return; // nothing to publish, so no version and no journal entry
  }
  auto txn = begin();
  for (const Removal& r : removals) {
    txn.detach_layer(r.composition, r.layer);
    txn.remove(r.layer);
    txn.remove(r.content);
  }
  txn.commit();
  // Binding rows are RETAINED, exactly as the single-object `remove_content` retains
  // them and for the same reason: each erased record's deferred `StateHandle` release is
  // routed to the content's binding row when the record is finally reclaimed, and
  // dropping the row now would strand that release.
}

ObjectId Document::add_content(std::shared_ptr<Content> content, std::uint64_t kind) {
  // Mint a versioned ContentRecord (kind id + inert StateHandle) as a top-level
  // DocState entry, published as one new version -- exactly like every other
  // Document mutator. The id->Content vtable binding then rides the runtime
  // side-map keyed by that record's id (doc 17:66-72 keeps the model free of the
  // Content vtable). resolve(id) serves it; find_content(id) carries no pointer.
  Content& live = *content;
  auto txn = begin();
  const ObjectId id = txn.add_content(kind);

  // Register-on-instantiate, the state-sink analogue of the damage sink the core
  // connects on attach (doc 03:113-118). Through the `Editable` facet only, so the
  // runtime names no concrete kind (doc 17:66-72); non-editable content binds
  // nothing. Routed INSIDE the transaction: the content's row must be live before
  // the commit publishes the record, or the retain owed for the state it embeds
  // would find no owner. Any number of editable contents may be added -- the
  // document-wide sink trio dispatches by the owning id (`EditableBinding`).
  if (Editable* editable = d_binding.bind(id, live)) {
    // Close the `model.content_binding` gap: that task deliberately left the fresh
    // ContentRecord's StateHandle inert. An editable content already HAS state at
    // instantiation, so capture it onto the record here, in the same transaction --
    // one published version, exactly as before. Without this the record names no
    // state, so the first edit's journal entry has an INERT *before* handle and its
    // undo would restore the content to nothing (doc 14:133-152).
    const StateHandle initial = editable->capture();
    if (initial.has_state()) {
      txn.set_content_state(id, initial);
    }
  }

  // Capture the content's CONSTRUCTION IDENTITY and INPUT EDGES onto the record, in
  // the SAME transaction that publishes it (`runtime.workspace_content_reconstruction`,
  // issue #19). Without these a workspace reopen restores the record graph and has
  // nothing to rebuild a content out of: `kind` is a per-session interning token, and a
  // solid's colour or an operator's inputs live only in the `Content` object.
  //
  // Identity comes from the installed capture hook -- the kind's registered codec, which
  // is also what the canonical save path trusts, so the workspace and the `.arbc` agree
  // by construction. No hook, or a kind with no codec, captures nothing and leaves the
  // pre-issue-#19 record, which a reopen reports rather than guesses at.
  if (d_identity_capture) {
    std::string kind_id;
    std::string params;
    if (d_identity_capture(live, kind, kind_id, params)) {
      txn.set_content_identity(id, kind_id, params);
      // Remember what this object was BUILT from (issue #34): it is what makes a later
      // staleness check a string compare instead of a re-capture, and a content added before
      // any hook was installed simply has no entry and is never considered stale.
      d_bound_params.insert_or_assign(id, params);
    }
  }

  // Input edges are the CORE's, not a codec's (doc 08 Principle 6), so they are read
  // straight off the content's own `inputs()` and mapped back to the ids the document
  // knows them by. An operator is constructed over contents the host already added, so
  // those bindings are present; an input that is somehow unbound contributes an invalid
  // id, which reconstruction then reports rather than silently dropping (an operator
  // rebuilt with a missing input is wrong content, not partial content). Copied out
  // under the kind's storage lock (`for_each_input`) for the same reason as
  // `mint_content` above: this is the writer thread, racing the render thread's walks.
  std::vector<ContentRef> inputs;
  for_each_input(live, [&](std::size_t /*index*/, ContentRef in) { inputs.push_back(in); });
  if (!inputs.empty()) {
    const std::shared_ptr<const ContentBindings> snap = d_contents.load();
    std::vector<ObjectId> input_ids;
    input_ids.reserve(inputs.size());
    for (const ContentRef in : inputs) {
      ObjectId found{};
      for (const auto& [bound_id, bound] : *snap) {
        if (bound.get() == in) {
          found = bound_id;
          break;
        }
      }
      input_ids.push_back(found);
    }
    txn.set_content_inputs(id, input_ids);
  }

  txn.commit();

  // Publish the id->Content binding COPY-ON-WRITE (issue #10): copy the current
  // immutable map, append the new row, and atomically swap it in. The whole-map copy is
  // near-free -- the values are `shared_ptr` and `emplace` is rare -- and it is what lets
  // `resolve()`/`for_each_content()` be lock-free pinned reads on the render thread. This
  // is the SINGLE structural mutation of the table, and it runs on the writer thread
  // alone, so a plain load/copy/store needs no CAS loop. A reader mid-walk keeps its own
  // pinned generation; it simply does not see this new row until its next load.
  auto next = std::make_shared<ContentBindings>(*d_contents.load());
  next->emplace(id, std::move(content));
  d_contents.store(std::move(next));
  return id;
}

void Document::reclaim_removed_content(ObjectId content) {
  // Runs on whichever thread drained the record -- the housekeeping thread for a live
  // document -- so it must not assume the writer. Dropping the routing row is
  // `EditableBinding`'s own drain-safe teardown; erasing the binding is a
  // copy-on-write swap, which is safe against concurrent lock-free readers by
  // construction (they keep their pinned generation).
  //
  // Guarded on the content being absent from the CURRENT version as well: a content
  // whose state simply happens to have no live record right now -- one that has never
  // been edited, so no version embeds a non-inert handle for it -- is still very much
  // part of the document, and dropping its row would unbind a live layer.
  // No current version means the model is publishing or tearing down, and there is no
  // document to ask whether this content is still part of. Do nothing: the conservative
  // answer costs at most the retention this reclaim exists to shorten, while acting on a
  // null pin is a crash. (`~Document`'s declaration-order drain frees everything anyway,
  // which is the case this guard actually covers.)
  if (d_tearing_down.load(std::memory_order_acquire)) {
    return; // ~Document is releasing everything; there is nothing to shorten
  }
  const DocStatePtr pinned = d_model->current();
  if (pinned == nullptr || pinned->find_content(content) != nullptr) {
    return;
  }
  // NOT `unbind`, which drains before it drops: this runs FROM the release that
  // emptied this content's queue, so the drain has already happened and re-entering it
  // would recurse into the drainer currently running. Zero outstanding retains is
  // exactly the proof that drain exists to establish, already in hand.
  d_binding.drop_row_after_last_release(content);
  auto next = std::make_shared<ContentBindings>(*d_contents.load());
  if (next->erase(content) == 0) {
    return; // already gone: nothing to publish
  }
  d_contents.store(std::move(next));
}

void Document::set_content_identity_capture(ContentIdentityCapture capture) {
  d_identity_capture = std::move(capture);
}

void Document::set_content_reconstruct(ContentReconstruct reconstruct) {
  d_content_reconstruct = std::move(reconstruct);
}

// The live objects a content record's input edges name. Empty for every leaf; `false` when an
// edge resolves to nothing, which refuses the whole operation rather than rebuilding an
// operator with a missing input -- wrong content, not partial content (the same rule
// `runtime.workspace_content_reconstruction` applies on the reopen path).
bool Document::resolve_record_inputs(const DocStatePtr& pinned, ObjectId id,
                                     std::vector<ContentRef>& out) const {
  for (const ObjectId in : pinned->content_inputs(id)) {
    Content* const live = resolve(in);
    if (live == nullptr) {
      return false;
    }
    out.push_back(live);
  }
  return true;
}

bool Document::update_content_config(ObjectId id, std::string_view params, std::string name) {
  if (!d_content_reconstruct) {
    return false; // no way to rebuild the object: refuse rather than publish a lie
  }
  const DocStatePtr pinned = d_model->current();
  if (pinned->find_content(id) == nullptr) {
    return false;
  }
  // The kind comes from the RECORD, never from the caller: this rewrites a config, and a
  // config that names another kind is a different content, which is what `add_content` and
  // `remove_content` are for.
  const DocRoot::ContentIdentity identity = pinned->content_identity(id);
  if (identity.kind_id.empty()) {
    return false; // nothing persisted names this content's kind (a pre-issue-#19 record)
  }
  std::vector<ContentRef> inputs;
  if (!resolve_record_inputs(pinned, id, inputs)) {
    return false;
  }
  std::shared_ptr<Content> rebuilt = d_content_reconstruct(identity.kind_id, params, inputs);
  if (rebuilt == nullptr) {
    return false; // the codec declined: nothing published, nothing rebound
  }

  // ONE transaction: the record's new identity and the damage it implies, so one user action
  // is one journal entry and one undo press (issue #20's rule, applied to an edit that did
  // not exist when that landed). The damage is whole-object and all-time -- rebuilding a
  // content from a different config can change every pixel at every instant, and the absorbing
  // shape is what a frame needs to repaint the layers showing it.
  auto txn = begin(std::move(name));
  txn.set_content_identity(id, identity.kind_id, params);
  Damage dmg;
  dmg.object = id;
  dmg.rect = Rect::infinite();
  dmg.range = TimeRange::all();
  txn.add_damage(dmg);
  txn.commit();

  // The live object follows the record. `rebind_content` publishes no version and appends no
  // entry, which is exactly right here: the versioned change is the one just committed, and
  // this is the side-map catching up to it.
  static_cast<void>(rebind_content(id, std::move(rebuilt)));
  d_bound_params.insert_or_assign(id, std::string(params));
  return true;
}

std::size_t Document::reconcile_content_bindings() {
  if (!d_content_reconstruct) {
    return 0;
  }
  const DocStatePtr pinned = d_model->current();
  std::size_t rebound = 0;
  for (auto& [id, bound_params] : d_bound_params) {
    if (pinned->find_content(id) == nullptr) {
      continue; // the record is gone (an undone add, a removal still in history)
    }
    const DocRoot::ContentIdentity identity = pinned->content_identity(id);
    if (identity.kind_id.empty() || identity.params == bound_params) {
      continue; // the common case, and the reason staleness is a string compare
    }
    std::vector<ContentRef> inputs;
    if (!resolve_record_inputs(pinned, id, inputs)) {
      continue;
    }
    std::shared_ptr<Content> rebuilt =
        d_content_reconstruct(identity.kind_id, identity.params, inputs);
    if (rebuilt == nullptr) {
      continue; // a decline leaves the previous object bound: stale, never null
    }
    if (!rebind_content(id, std::move(rebuilt))) {
      continue;
    }
    // `rebind_content` records the identity it just bound, and the key is one this loop is
    // standing on -- an assignment to an EXISTING key, which rehashes nothing and invalidates
    // no iterator. Restating it here keeps the invariant readable at the site that owns it.
    bound_params = identity.params;
    ++rebound;
  }
  return rebound;
}

bool Document::undo() {
  if (!d_journal.undo()) {
    return false;
  }
  reconcile_content_bindings();
  return true;
}

bool Document::redo() {
  if (!d_journal.redo()) {
    return false;
  }
  reconcile_content_bindings();
  return true;
}

bool Document::rebind_content(ObjectId id, std::shared_ptr<Content> content) {
  // The record must already exist. Binding an object to an id the document does not
  // have would leave a row nothing can ever reach through `resolve` (which is keyed by
  // the record graph, not by the map) and nothing can ever reclaim, so it is refused
  // as a value rather than silently orphaned.
  const DocStatePtr pinned = d_model->current();
  if (pinned->find_content(id) == nullptr) {
    return false;
  }

  // Drop the outgoing content's `Editable` routing FIRST, with its row still installed,
  // exactly as `EditableBinding::unbind`'s contract requires: the drain has to run while
  // the row can still be routed to, or the queued reclaims of the content being replaced
  // strand and leak their pinned state handles. `unbind` on an unbound id is a no-op, so
  // a first bind onto a recovered record takes the same line.
  d_binding.unbind(id);

  // Publish the row copy-on-write, exactly as `add_content` does: copy the current
  // immutable map, replace (or erase) the row, swap it in. Readers keep their own pinned
  // generation and simply do not see the change until their next load, so a render
  // thread mid-walk is never torn.
  auto next = std::make_shared<ContentBindings>(*d_contents.load());
  if (content == nullptr) {
    next->erase(id);
    d_contents.store(std::move(next));
    return true;
  }

  // Register the incoming content's state sinks under the SAME id, so a rebound editable
  // journals and undoes against the record it was bound to. Unlike `add_content` this does
  // NOT capture `initial` onto the record: the record's `StateHandle` is the one that was
  // persisted (or the one the outgoing content left), and overwriting it here would
  // discard exactly the state a reopen is trying to recover -- and would do it outside any
  // transaction, since this publishes no version.
  Content& live = *content;
  next->insert_or_assign(id, std::move(content));
  d_contents.store(std::move(next));
  d_binding.bind(id, live);
  // Whatever was just bound is taken to BE what the record describes -- that is this seam's
  // whole contract, and it is literally true on the reopen path, which builds the object from
  // this very identity. Recording it keeps the staleness check (issue #34) honest: without
  // this, every reconstructed content would read as stale on the first navigation.
  const DocRoot::ContentIdentity identity = pinned->content_identity(id);
  if (!identity.kind_id.empty()) {
    d_bound_params.insert_or_assign(id, identity.params);
  }
  return true;
}

const std::vector<Model::RecoveredContentState>&
Document::recovered_content_state() const noexcept {
  return d_model->recovered_content_state();
}

void Document::remove_content(ObjectId content, ObjectId composition, ObjectId layer) {
  // The inverse of `add_content`, composing three existing model teardowns into one
  // atomic, undoable, per-content deletion (doc 14 § Transactions, the removal
  // paragraph). One transaction => one publish, one journal entry, one damage flush:
  // detach the referencing layer from its composition's ordered membership, erase the
  // layer record, and erase the content record. Because the mutations share a
  // transaction an observer sees the document with the content present or fully gone,
  // never a layer naming an erased content.
  auto txn = begin();
  txn.detach_layer(composition, layer);
  txn.remove(layer);
  txn.remove(content);
  txn.commit();

  // Decision 1 (`model.content_removal`): do NOT `d_binding.unbind(content)` or
  // `d_contents.erase(content)` here. The removal is journaled, so the erased
  // ContentRecord is NOT reclaimed -- the journal's `before` edge holds it so undo
  // can restore it (doc 14 § History). That record's `StateHandle::release` is
  // deferred to its eventual reclaim and routed to the content's binding row; dropping
  // the row now would strand that future release with nowhere to route, tripping the
  // binding's asserted-zero `unrouted_state_calls()` invariant and leaking the
  // handle's pool blocks -- the exact hazard `EditableBinding::unbind`'s own
  // doc-comment warns against. So the live `Content*` and its row are RETAINED here;
  // they are torn down by the document-close declaration-order drain, and, once the
  // named `runtime.removed_content_reclaim` follow-up lands, at history-trim.
}

Model::Transaction Document::transact(std::string name) { return begin(std::move(name)); }

Model::Transaction Document::begin(std::string name) {
  // The one place a `Document` edit opens a transaction, and therefore the one place the
  // arrival-before-edit rule lives (issue #13). Ordered BEFORE the model transaction, never
  // inside it: the settler publishes its own revisions, and the edit that follows must be
  // based on the version those left behind -- exactly the ordering `HostViewport::step()`
  // uses when IT is the writer thread (settle at step 0, then pin).
  settle_arrived_external_loads();
  return d_model->transact(std::move(name));
}

void Document::settle_arrived_external_loads() {
  // Three ways to cost nothing, in the order they are cheapest: nobody installed a settler
  // (every document with no viewport bound, and every programmatically-built one); a load or a
  // settle is already in flight, so THIS call is the installer's own `add_content` re-entering
  // (or a host edit made from inside a load, which the loader owns and must not have
  // rearranged under it); and -- the overwhelmingly common case on a live document -- nothing
  // has arrived, which is one relaxed atomic load.
  if (!d_external_settle || d_settling || d_pending_loads->ready() == 0) {
    return;
  }
  // Scoped, not paired by hand: a settler that throws (a codec reaching for memory it cannot
  // get) must not leave the document permanently marked as installing, which would silently
  // disable the auto-settle for the rest of its life.
  struct Scope {
    explicit Scope(bool& flag) noexcept : d_flag(flag) { d_flag = true; }
    ~Scope() { d_flag = false; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    bool& d_flag;
  } const scope(d_settling);
  d_auto_settled += d_external_settle();
}

void Document::drain() { d_housekeeping.drain_and_quiesce(); }

void Document::set_external_load_settler(std::function<std::size_t()> settle) {
  // Install-counted rather than last-wins (see the header): N viewports over one document each
  // install the same derived behavior, and the first of them to be destroyed must not take the
  // auto-settle away from the ones still running.
  if (settle) {
    d_external_settle = std::move(settle);
    ++d_settler_installs;
    return;
  }
  if (d_settler_installs > 0) {
    --d_settler_installs;
    if (d_settler_installs == 0) {
      d_external_settle = nullptr;
    }
  }
}

expected<std::monostate, WorkspaceFileError> Document::checkpoint() {
  if (!d_model->workspace_backed()) {
    // The anonymous document answers rather than asserting: there is no file, so there
    // is nothing to make durable (Constraint 6). `Model::checkpoint()` says the same.
    return unexpected(WorkspaceFileError{WorkspaceFileErrc::Unsupported, 0});
  }
  return d_housekeeping.request_checkpoint();
}

HousekeepingStats Document::memory_stats() const { return d_housekeeping.stats(); }

std::uint64_t Document::background_ticks() const noexcept {
  return d_housekeeping.background_ticks();
}

std::uint64_t Document::flush_housekeeping() { return d_housekeeping.flush(); }

ObjectId Document::add_layer(ObjectId content, const Affine& transform, double opacity) {
  auto txn = begin();
  const ObjectId id = txn.add_layer(content, transform, opacity);
  txn.commit();
  return id;
}

void Document::set_layer_transform(ObjectId layer, const Affine& transform) {
  auto txn = begin();
  txn.set_transform(layer, transform);
  txn.commit();
}

void Document::set_layer_span(ObjectId layer, const TimeRange& span) {
  auto txn = begin();
  txn.set_span(layer, span);
  txn.commit();
}

void Document::set_layer_time_map(ObjectId layer, const TimeMap& time_map) {
  auto txn = begin();
  txn.set_time_map(layer, time_map);
  txn.commit();
}

void Document::attach_layer(ObjectId composition, ObjectId layer) {
  auto txn = begin();
  txn.attach_layer(composition, layer);
  txn.commit();
}

void Document::set_layer_gain(ObjectId layer, double gain) {
  auto txn = begin();
  txn.set_gain(layer, gain);
  txn.commit();
}

void Document::set_layer_audible(ObjectId layer, bool audible) {
  auto txn = begin();
  txn.set_audible(layer, audible);
  txn.commit();
}

void Document::set_working_audio_format(ObjectId composition, const AudioFormat& format) {
  auto txn = begin();
  txn.set_working_audio_format(composition, format);
  txn.commit();
}

ObjectId Document::add_composition(double canvas_w, double canvas_h) {
  auto txn = begin();
  const ObjectId id = txn.add_composition(canvas_w, canvas_h);
  txn.commit();
  return id;
}

void Document::set_working_space(ObjectId composition, const SurfaceFormat& format) {
  auto txn = begin();
  txn.set_working_space(composition, format);
  txn.commit();
}

DocStatePtr Document::pin() const { return d_model->current(); }

Content* Document::resolve(ObjectId content) const {
  // Lock-free pinned read (issue #10): load the current published map once and look up
  // in that immutable snapshot. A concurrent writer publishing a new map cannot tear this
  // lookup -- the pinned `shared_ptr` keeps the snapshot alive for the call's duration.
  const std::shared_ptr<const ContentBindings> snap = d_contents.load();
  const auto found = snap->find(content);
  return found == snap->end() ? nullptr : found->second.get();
}

void Document::for_each_content(const std::function<void(Content*)>& fn) const {
  // Pin one published generation for the whole walk (issue #10): a content the writer
  // appends mid-iteration lands in a NEW map the writer swaps in, leaving this snapshot
  // untouched, so the walk never observes a rehash of the container it is iterating.
  const std::shared_ptr<const ContentBindings> snap = d_contents.load();
  for (const auto& entry : *snap) {
    fn(entry.second.get());
  }
}

void Document::for_each_content(const std::function<void(ObjectId, Content*)>& fn) const {
  const std::shared_ptr<const ContentBindings> snap = d_contents.load();
  for (const auto& entry : *snap) {
    fn(entry.first, entry.second.get());
  }
}

} // namespace arbc
