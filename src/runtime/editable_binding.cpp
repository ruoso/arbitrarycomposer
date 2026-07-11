#include <arbc/runtime/editable_binding.hpp>

#include <stdexcept>
#include <string>

namespace arbc {

void EditableBinding::attach(Model& model, Journal& journal) noexcept {
  d_model = &model;
  d_journal = &journal;
}

Editable* EditableBinding::bind(ObjectId id, Content& content) {
  Editable* const editable = content.editable();
  if (editable == nullptr) {
    return nullptr; // leaf/live content: nothing to bind, the inert path stays exact
  }
  if (bound()) {
    // Never a silent overwrite: the second content's sinks would replace the
    // first's in the single model/journal slots, and the first content's already
    // published handles would then be retained/released against the WRONG store
    // (a `StateHandle` is a bare slot index, local to its content). Fail loudly,
    // the same idiom as `DeviceMonitor`'s already-bound transport.
    throw std::logic_error("Document: an editable content (object " +
                           std::to_string(d_owner.value) +
                           ") is already bound; v1 binds one editable content per document "
                           "(runtime.editable_sink_multiplex lifts this)");
  }

  d_owner = id;
  d_ref_sink.emplace(*editable);
  d_cost_fn.emplace(*editable);
  d_restore_sink.emplace(*editable, id);

  d_model->set_state_ref_sink(&*d_ref_sink);
  d_journal->set_state_cost_fn(&*d_cost_fn);
  d_journal->set_restore_sink(&*d_restore_sink);
  return editable;
}

void EditableBinding::unbind() {
  if (!bound()) {
    return;
  }
  // Drain with the sinks STILL installed: a removed content's records reclaim at
  // zero count through `ContentStateReclaimSink`, which releases their embedded
  // handles through the `StateRefSink` -- the release must land on the live sink,
  // not on a cleared slot, or the version refcount never reaches zero.
  d_model->drain();

  d_model->set_state_ref_sink(nullptr);
  d_journal->set_state_cost_fn(nullptr);
  d_journal->set_restore_sink(nullptr);

  d_ref_sink.reset();
  d_cost_fn.reset();
  d_restore_sink.reset();
  d_owner = ObjectId{};
}

} // namespace arbc
