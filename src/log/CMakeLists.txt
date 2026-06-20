# log: the single logging entry point. It reads config.logging and configures
# spdlog once, so every component writes through one logger to one destination.
# It sits just above config; anything that logs links it.
add_library(opaquedb_log
  log.cpp)

target_include_directories(opaquedb_log PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

# spdlog and config appear in the public header, so both are PUBLIC.
target_link_libraries(opaquedb_log
  PUBLIC
    opaquedb_config
    spdlog::spdlog
    absl::status
  PRIVATE
    opaquedb_compile_options)
