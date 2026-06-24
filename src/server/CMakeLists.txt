# server: the Engine facade and the gRPC query service. It composes crypto,
# storage, the planner, and backends, and ships the reference backend. The engine
# itself is transport agnostic; query_service and node_server are the gRPC
# adapter and the node composition root.
add_library(opaquedb_server
  engine.cpp
  query_service.cpp
  admin_grpc_service.cpp
  shard_service.cpp
  query_coordinator.cpp
  repository_manager.cpp
  node_server.cpp
  request_gate.cpp
  ingest.cpp)

target_include_directories(opaquedb_server PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(opaquedb_server
  PUBLIC
    opaquedb_core
    opaquedb_config
    opaquedb_crypto
    opaquedb_storage
    opaquedb_planner
    opaquedb_backend
    opaquedb_sql
    opaquedb_auth
    opaquedb_admin
    opaquedb_cluster
    opaquedb_proto
    absl::status
    absl::statusor
  PRIVATE
    opaquedb_compile_options
    opaquedb_backend_reference
    opaquedb_log
    absl::strings)
