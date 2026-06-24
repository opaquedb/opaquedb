# admin: the transport-agnostic management facade (AdminService) and the
# client keyring store. The gRPC and CLI adapters are thin clients over this.
# It composes config, storage, crypto, and the backend registry; it knows
# nothing about transport. The server's gRPC admin adapter depends on it.
add_library(opaquedb_admin
  service.cpp
  keyring_store.cpp)

target_include_directories(opaquedb_admin PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

# config, storage, core, and crypto types appear in the public headers
# (Config, EpochRepository, Schema, KeyMaterial), so they are PUBLIC. The
# backend registry is only read in the implementation.
target_link_libraries(opaquedb_admin
  PUBLIC
    opaquedb_config
    opaquedb_core
    opaquedb_storage
    opaquedb_crypto
    absl::status
    absl::statusor
  PRIVATE
    opaquedb_compile_options
    opaquedb_backend)
