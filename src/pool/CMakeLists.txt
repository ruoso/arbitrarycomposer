arbc_add_component(
  NAME pool
  SOURCES slot_store.cpp workspace_file.cpp
  PUBLIC_HEADERS arbc/pool/chunk_source.hpp arbc/pool/slab_directory.hpp
                 arbc/pool/slot_store.hpp arbc/pool/typed_store.hpp
                 arbc/pool/refs.hpp arbc/pool/reclamation.hpp
                 arbc/pool/workspace_file.hpp arbc/pool/checkpoint.hpp
  DEPENDS base)

arbc_component_test(COMPONENT pool SOURCES t/pool.t.cpp t/refs.t.cpp t/reclamation.t.cpp
                    t/refcounts_in_store.t.cpp t/workspace_file.t.cpp t/checkpoint.t.cpp
                    t/crash_tests.t.cpp t/bench_smoke.t.cpp t/free_pools.t.cpp)

arbc_component_bench(COMPONENT pool SOURCES bench/allocator_bench.cpp)
