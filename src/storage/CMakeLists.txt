# storage: the on-disk engine. It depends on core (the schema) but knows nothing
# about crypto and stores opaque bytes. Slot geometry reaches it as plain
# integers, never SEAL types. nlohmann/json is used only for the machine-written
# epoch manifest.
add_library(opaquedb_storage
  crc32.cpp
  io_util.cpp
  mapped_file.cpp
  segment.cpp
  wal.cpp
  staging.cpp
  epoch_manifest.cpp
  local_repository.cpp
  catalog.cpp)

target_include_directories(opaquedb_storage
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(opaquedb_storage
  PUBLIC
    opaquedb_core
    absl::status
    absl::statusor
  PRIVATE
    opaquedb_compile_options
    nlohmann_json::nlohmann_json
    absl::strings)
