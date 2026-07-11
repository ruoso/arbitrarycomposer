#include <arbc/runtime/editable_binding.hpp>

namespace arbc {

void EditableBinding::attach(Model& model, Journal& journal) noexcept {
  d_model = &model;
  d_journal = &journal;

  // The trio is installed ONCE, here, and serves the document for its whole life
  // however many editable contents it goes on to hold: each sink holds the routing
  // table, not a bare `Editable*`, so `bind()` never has to touch these slots.
  // That is the whole shape of the multiplex -- the model's half stays one
  // type-erased pointer per seam (doc 17:66-72), and the N lives in `runtime`.
  d_model->set_state_ref_sink(&d_ref_sink);
  d_journal->set_state_cost_fn(&d_cost_fn);
  d_journal->set_restore_sink(&d_restore_sink);
  d_registrations += 3;
}

Editable* EditableBinding::bind(ObjectId id, Content& content) {
  Editable* const editable = content.editable();
  if (editable == nullptr) {
    return nullptr; // leaf/live content: nothing to route, the inert path stays exact
  }
  d_router.insert(id, *editable);
  return editable;
}

void EditableBinding::unbind(ObjectId id) {
  if (!d_router.contains(id)) {
    return;
  }
  // Drain with the row STILL installed: the model's reclamation queue may hold
  // releases for records embedding this content's handles, and each must land on
  // the content that owns it. Dropping the row first would strand those releases
  // (the version refcount never reaches zero, so the content's pool blocks leak)
  // and trip the unrouted-call counter besides. Drain, then drop.
  d_model->drain();
  d_router.erase(id);
}

void EditableBinding::unbind_all() {
  if (d_model == nullptr) {
    return;
  }
  d_model->drain(); // the same ordering, document-wide: every row is still live here

  d_model->set_state_ref_sink(nullptr);
  d_journal->set_state_cost_fn(nullptr);
  d_journal->set_restore_sink(nullptr);
  d_registrations += 3;

  d_router.clear();
}

} // namespace arbc
