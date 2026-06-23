# planner: routes a logical plan to a backend. It depends on sql, core (schema),
# and the backend capability/registry layer. It does not depend on a concrete
# backend, transport, or storage.
add_library(opaquedb_planner
  planner.cpp)

target_include_directories(opaquedb_planner PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(opaquedb_planner
  PUBLIC
    opaquedb_core
    opaquedb_sql
    opaquedb_backend
    absl::statusor
  PRIVATE
    opaquedb_compile_options
    absl::strings)
