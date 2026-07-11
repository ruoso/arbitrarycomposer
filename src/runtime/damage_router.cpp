#include <arbc/runtime/damage_router.hpp>
#include <arbc/runtime/document.hpp>

#include "document_access.hpp" // HostViewportDocumentAccess (the Model& behind a Document)

#include <algorithm>
#include <cassert>
#include <cstddef>

namespace arbc {

DamageRouter::DamageRouter(Model& model) : d_model(model) {
  // Occupy the model's single damage slot exactly once (Constraint 1). Child
  // sinks register with the router, never with the model directly.
  d_model.set_damage_sink(this);
}

DamageRouter::DamageRouter(Document& doc) : DamageRouter(HostViewportDocumentAccess::model(doc)) {}

DamageRouter::~DamageRouter() {
  // The router must outlive all its registrants (Constraint 5): each viewport's
  // `Registration` unregisters at its own scope exit, so a live registrant here is
  // a lifetime-ordering bug, not a recoverable state.
  assert(d_registrants.empty() && "DamageRouter destroyed with live registrants");
  d_model.set_damage_sink(nullptr);
}

void DamageRouter::flush(const std::vector<Damage>& damage) {
  // Forward the model's already-unioned batch verbatim to each registrant in
  // registration order, exactly once (Constraint 2/Decision 4). Iterate by index:
  // register/unregister during a flush is not a supported call pattern and no
  // registrant's `flush` mutates the router (Constraint 6).
  for (std::size_t i = 0; i < d_registrants.size(); ++i) {
    d_registrants[i]->flush(damage);
    ++d_deliveries;
  }
}

DamageRouter::Registration DamageRouter::register_sink(DamageSink& sink) {
  d_registrants.push_back(&sink);
  return Registration(*this, sink);
}

void DamageRouter::unregister(DamageSink* sink) noexcept {
  // O(registrants) removal by identity; the other registrants (and the router)
  // stay intact (Constraint 3).
  const auto it = std::find(d_registrants.begin(), d_registrants.end(), sink);
  if (it != d_registrants.end()) {
    d_registrants.erase(it);
  }
}

void DamageRouter::Registration::release() noexcept {
  if (d_router != nullptr) {
    d_router->unregister(d_sink);
    d_router = nullptr;
    d_sink = nullptr;
  }
}

} // namespace arbc
