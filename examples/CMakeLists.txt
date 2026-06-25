# Example programs demonstrating client usage and direct engine access.
# Built when OPAQUEDB_BUILD_EXAMPLES=ON (default).

add_executable(e2e_client e2e_client.cpp)
target_link_libraries(e2e_client PRIVATE
  opaquedb_client
  opaquedb_config
  opaquedb_core
  opaquedb_sql
  CLI11::CLI11
  opaquedb_compile_options
)

# Also produce a tiny direct (in-process) demo that bypasses gRPC and talks
# straight to the Engine. Useful to validate crypto + backend without network.
add_executable(crypto_bench crypto_bench.cpp)
target_link_libraries(crypto_bench PRIVATE
  opaquedb_crypto
  opaquedb_config
  opaquedb_core
  SEAL::seal
  opaquedb_compile_options
)

add_executable(direct_engine direct_engine.cpp)
target_link_libraries(direct_engine PRIVATE
  opaquedb_server
  opaquedb_backend_reference
  opaquedb_storage
  opaquedb_core
  opaquedb_config
  absl::status
  absl::statusor
  opaquedb_compile_options
)
