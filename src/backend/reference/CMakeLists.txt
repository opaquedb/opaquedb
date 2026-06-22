# The reference backend. It links the backend interface and crypto, and depends
# on neither SQL nor transport. Linking this library and referencing
# LinkReferenceBackend registers it with the backend registry.
add_library(opaquedb_backend_reference
  reference.cpp)

target_include_directories(opaquedb_backend_reference PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(opaquedb_backend_reference
  PUBLIC
    opaquedb_backend
    opaquedb_crypto
    SEAL::seal
  PRIVATE
    opaquedb_compile_options
    opaquedb_core
    Threads::Threads)
