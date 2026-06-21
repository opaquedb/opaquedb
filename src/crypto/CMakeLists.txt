# crypto: the SEAL boundary. It reads FHE parameters from config and depends on
# core for shared types. It knows nothing about SQL, storage, or transport.
# SEAL is PUBLIC because seal types appear in this library's public headers.
add_library(opaquedb_crypto
  context.cpp
  key_material.cpp
  ops.cpp
  serialize.cpp)

target_include_directories(opaquedb_crypto PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(opaquedb_crypto
  PUBLIC
    opaquedb_core
    opaquedb_config
    SEAL::seal
    absl::status
    absl::statusor
  PRIVATE
    opaquedb_compile_options
    absl::strings)
