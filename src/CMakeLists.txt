# Levelized components (design doc 17), lowest level first. The dependency
# edges declared in each component are validated by scripts/check_levels.py.
add_subdirectory(base)
add_subdirectory(pool)
add_subdirectory(media)
add_subdirectory(surface)
add_subdirectory(model)
add_subdirectory(contract)
add_subdirectory(backend_cpu)
add_subdirectory(compositor)
add_subdirectory(kind_solid)
add_subdirectory(runtime)

arbc_finalize_library()
