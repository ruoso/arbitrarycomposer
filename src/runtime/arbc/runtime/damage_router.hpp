#pragma once

#include <arbc/model/damage.hpp> // DamageSink, Damage
#include <arbc/model/model.hpp>  // Model

#include <cstddef>
#include <cstdint>
#include <vector>

// The fan-out damage sink for `arbc::runtime` (L5, doc 17:60): the single
// occupant of the `Model`'s one `set_damage_sink` slot that multiplexes each
// flushed model-damage batch to N registered per-viewport child sinks. The
// `Model` deliberately exposes exactly ONE sink and stays ignorant of host
// viewports (doc 17 keeps the model host-agnostic); the seam's own comment names
// "a fan-out router" as the intended occupant (`damage.hpp:75-78`,
// `host_viewport.cpp:33-35`, `host_objects` Decision 6). This router is the
// concrete realization of doc 01:108-110 / 137-138 -- multiple viewports observing
// one composition simultaneously, damage propagating to every viewport
// independently -- which the single-slot `Model` API cannot itself express.
//
// It is a PURE additive layer (refinement Decision 1/2): it forwards the model's
// already-unioned batch verbatim, in deterministic registration order, exactly
// once per registrant (Constraint 2/Decision 4). It performs no union, filter, or
// copy of its own -- each viewport's `HostViewport::DamageAccumulator` unions via
// `damage_add` (`host_viewport.hpp:150-154`). An empty registrant set makes a
// flush a no-op.
//
// Concurrency: `flush`, `register_sink`, and unregister all run on the single
// writer/UI thread that already owns `Model::set_damage_sink` (WRITER-THREAD ONLY)
// and the single-owner `HostViewport` value state. The router adds NO shared
// mutable state and NO cross-thread channel, so it carries NO new TSan/stress
// obligation (refinement Decision 5, `host_objects` Decision 5). Register/
// unregister during a flush is not a supported call pattern (no registrant's
// `flush` mutates the router); `flush` iterates the registrant list by index.

namespace arbc {

class DamageRouter final : public DamageSink {
public:
  // Move-only RAII registration token modeled on `CacheHold`
  // (`cache/keyed_store.hpp:80-108`): destroying (or moving-out of) it unregisters
  // the child sink from the router exactly once, so unregister-before-router-
  // destroy is structural rather than a caller obligation (Constraint 3/Decision 3).
  // A default-constructed or moved-from handle is inert (`valid() == false`) and
  // unregisters nothing.
  class Registration {
  public:
    Registration() noexcept = default;

    Registration(const Registration&) = delete;
    Registration& operator=(const Registration&) = delete;

    Registration(Registration&& other) noexcept : d_router(other.d_router), d_sink(other.d_sink) {
      other.d_router = nullptr;
      other.d_sink = nullptr;
    }

    Registration& operator=(Registration&& other) noexcept {
      if (this != &other) {
        release();
        d_router = other.d_router;
        d_sink = other.d_sink;
        other.d_router = nullptr;
        other.d_sink = nullptr;
      }
      return *this;
    }

    ~Registration() { release(); }

    // False for a default-constructed or moved-from handle (registers nothing).
    bool valid() const noexcept { return d_router != nullptr; }

  private:
    friend class DamageRouter;

    Registration(DamageRouter& router, DamageSink& sink) noexcept
        : d_router(&router), d_sink(&sink) {}

    void release() noexcept;

    DamageRouter* d_router{nullptr};
    DamageSink* d_sink{nullptr};
  };

  // Occupy the model's single damage slot for this object's lifetime (RAII,
  // Constraint 1), mirroring today's `HostViewport`. WRITER-THREAD ONLY, like the
  // `Model::set_damage_sink` it installs into.
  explicit DamageRouter(Model& model);
  // Clears the model slot and asserts an empty registrant list (Constraint 5): the
  // router must outlive all its registrants.
  ~DamageRouter() override;

  DamageRouter(const DamageRouter&) = delete;
  DamageRouter& operator=(const DamageRouter&) = delete;
  DamageRouter(DamageRouter&&) = delete;
  DamageRouter& operator=(DamageRouter&&) = delete;

  // Forward the same `const std::vector<Damage>&` to every registrant's `flush`,
  // exactly once per registrant, in registration order (Constraint 2). Empty
  // registrant set => zero deliveries. No router-side union/copy.
  void flush(const std::vector<Damage>& damage) override;

  // Register a child sink; the returned move-only handle unregisters it on
  // destruction (Constraint 3). Registrations dispatch in the order they were made.
  Registration register_sink(DamageSink& sink);

  // The current registrant count (Constraint 3 observability).
  std::size_t registered() const noexcept { return d_registrants.size(); }
  // A wall-clock-free behavioral counter (doc 16:54-62): the cumulative number of
  // per-registrant deliveries this router has performed. A zero-registrant flush
  // adds nothing; a flush to N registrants adds N.
  std::uint64_t deliveries() const noexcept { return d_deliveries; }

private:
  void unregister(DamageSink* sink) noexcept;

  Model& d_model;
  std::vector<DamageSink*> d_registrants;
  std::uint64_t d_deliveries{0};
};

} // namespace arbc
