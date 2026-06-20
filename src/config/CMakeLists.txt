# config: the typed schema and the layered loader. Foundational alongside core;
# everything may depend on it. It depends only on tomlplusplus (file format) and
# Abseil (the error type and string helpers), nothing else in the project.

# Embed the canonical default config so there is one source of truth for the
# defaults and no runtime path dependency. The file content becomes a string
# literal in a generated header. Re-run configure if the file changes.
set(OPAQUEDB_DEFAULT_CONFIG_FILE ${CMAKE_SOURCE_DIR}/config/opaquedb.default.toml)
file(READ ${OPAQUEDB_DEFAULT_CONFIG_FILE} OPAQUEDB_DEFAULT_CONFIG_TOML)
configure_file(default_config_data.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/gen/opaquedb/config/default_config_data.h @ONLY)
set_property(DIRECTORY APPEND PROPERTY
  CMAKE_CONFIGURE_DEPENDS ${OPAQUEDB_DEFAULT_CONFIG_FILE})

add_library(opaquedb_config
  config.cpp
  default_config.cpp
  loader.cpp)

target_include_directories(opaquedb_config
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/gen)

# absl::Status and absl::StatusOr appear in the public API, so Abseil is PUBLIC.
target_link_libraries(opaquedb_config
  PUBLIC
    absl::status
    absl::statusor
  PRIVATE
    opaquedb_compile_options
    tomlplusplus::tomlplusplus
    absl::strings)
