# cli: command objects, a thin front over the lower layers. It composes config
# and the server (engine, node, dev client), and admin and cluster later. It
# holds no domain logic of its own.
add_library(opaquedb_cli
  app.cpp
  util.cpp
  commands/config_command.cpp
  commands/data_commands.cpp
  commands/insert_command.cpp
  commands/admin_commands.cpp
  commands/run_command.cpp
  commands/query_command.cpp
  commands/repl_command.cpp
  commands/token_command.cpp)

target_include_directories(opaquedb_cli PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(opaquedb_cli
  PUBLIC
    opaquedb_config
    CLI11::CLI11
  PRIVATE
    replxx::replxx
    opaquedb_compile_options
    opaquedb_core
    opaquedb_sql
    opaquedb_admin
    opaquedb_server
    opaquedb_client
    opaquedb_storage
    opaquedb_cluster
    opaquedb_log
    absl::strings)
