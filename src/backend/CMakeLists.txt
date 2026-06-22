# backend: the PirBackend strategy interface, its capability vocabulary, and the
# registry. It depends on crypto (ciphertexts and keys cross the interface) and
# core, but not on SQL, the planner, or transport. Concrete backends live in
# their own libraries and link this one.
add_library(opaquedb_backend
  capabilities.cpp
  registry.cpp)

target_include_directories(opaquedb_backend PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(opaquedb_backend
  PUBLIC
    opaquedb_core
    opaquedb_crypto
    SEAL::seal
    absl::status
    absl::statusor
    absl::strings
  PRIVATE
    opaquedb_compile_options)
