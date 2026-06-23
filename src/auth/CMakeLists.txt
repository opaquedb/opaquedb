# auth: the Authenticator strategy and its implementations. It depends on config
# (the auth mode) and Abseil. It does not depend on gRPC; the transport edge
# extracts the inputs an authenticator needs, which keeps this library
# unit-testable without a network. Tokens are compared in constant time and not
# hashed at rest, so no extra crypto library is pulled in (see the auth header).
add_library(opaquedb_auth
  authenticator.cpp
  authenticators.cpp)

target_include_directories(opaquedb_auth PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(opaquedb_auth
  PUBLIC
    opaquedb_config
    absl::status
    absl::statusor
  PRIVATE
    opaquedb_compile_options
    absl::strings)
