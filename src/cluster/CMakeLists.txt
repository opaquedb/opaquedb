# cluster: self-organization through etcd. The EtcdClient adapter, an in-memory
# fake, the consistent-hash shard map, and the cluster manager (election,
# membership, epoch coordination) depend only on core and config. The real
# etcd-backed client is compiled in when etcd-cpp-apiv3 is available, so the
# library and its tests build with or without the dependency.
add_library(opaquedb_cluster
  fake_etcd.cpp
  shard_map.cpp
  manager.cpp)

target_include_directories(opaquedb_cluster PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(opaquedb_cluster
  PUBLIC
    opaquedb_core
    opaquedb_config
    absl::status
    absl::statusor
  PRIVATE
    opaquedb_compile_options
    opaquedb_log
    absl::strings)

find_package(etcd-cpp-api CONFIG QUIET)
if(etcd-cpp-api_FOUND)
  target_sources(opaquedb_cluster PRIVATE real_etcd_client.cpp)
  # The etcd headers and gRPC are an implementation detail behind the adapter.
  target_link_libraries(opaquedb_cluster PRIVATE etcd-cpp-api)
  target_compile_definitions(opaquedb_cluster PUBLIC OPAQUEDB_HAVE_ETCD=1)
  message(STATUS "OpaqueDB: building the real etcd client")
else()
  message(STATUS "OpaqueDB: etcd-cpp-apiv3 not found; cluster uses the fake only")
endif()
