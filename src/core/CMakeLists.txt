# core: foundational types. Everything may depend on core; core depends on
# nothing in the project. Keep it free of crypto, SQL, storage and transport.
add_library(opaquedb_core
  version.cpp
  schema.cpp
  record_codec.cpp
  key_codec.cpp
  slot_codec.cpp)

target_include_directories(opaquedb_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

# absl::Status and absl::StatusOr appear in the public API of the value codec,
# so Abseil is PUBLIC. Strings are an implementation detail of error messages.
target_link_libraries(opaquedb_core
  PUBLIC
    absl::status
    absl::statusor
  PRIVATE
    opaquedb_compile_options
    absl::strings
    # JSON columns are validated as well-formed JSON at the record_codec
    # text -> Value boundary. Header-only, an implementation detail, so PRIVATE.
    nlohmann_json::nlohmann_json)
