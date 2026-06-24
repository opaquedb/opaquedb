# Client support library. Provides a clean, reusable API for applications that
# want to talk to an OpaqueDB cluster without duplicating crypto+wire logic.
# Depends only on crypto (SEAL boundary), core, and the generated proto.
add_library(opaquedb_client
  query_client.cpp
)

target_include_directories(opaquedb_client PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(opaquedb_client
  PUBLIC
    opaquedb_crypto
    opaquedb_core
    opaquedb_config
    opaquedb_proto
    absl::status
    absl::statusor
  PRIVATE
    opaquedb_compile_options
    opaquedb_sql
    absl::strings
)
