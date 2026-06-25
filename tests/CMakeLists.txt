find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

# One test executable per source file keeps failures isolated and parallel.
# add_opaquedb_test(name file.cpp lib...) builds and registers a gtest binary.
function(add_opaquedb_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE
    opaquedb_compile_options
    GTest::gtest
    GTest::gtest_main)
  gtest_discover_tests(${name})
endfunction()

add_opaquedb_test(core_version_test core_version_test.cpp)
target_link_libraries(core_version_test PRIVATE opaquedb_core)

add_opaquedb_test(config_test config_test.cpp)
target_link_libraries(config_test PRIVATE opaquedb_config)

add_opaquedb_test(key_codec_test key_codec_test.cpp)
target_link_libraries(key_codec_test PRIVATE opaquedb_core)

add_opaquedb_test(crypto_test crypto_test.cpp)
target_link_libraries(crypto_test PRIVATE opaquedb_crypto)

add_opaquedb_test(reference_backend_test reference_backend_test.cpp)
target_link_libraries(reference_backend_test PRIVATE
  opaquedb_backend opaquedb_backend_reference opaquedb_crypto opaquedb_core)

add_opaquedb_test(storage_test storage_test.cpp)
target_link_libraries(storage_test PRIVATE opaquedb_storage opaquedb_core)

add_opaquedb_test(catalog_test catalog_test.cpp)
target_link_libraries(catalog_test PRIVATE opaquedb_storage opaquedb_core
  opaquedb_config)

add_opaquedb_test(sql_test sql_test.cpp)
target_link_libraries(sql_test PRIVATE opaquedb_sql)

add_opaquedb_test(planner_test planner_test.cpp)
target_link_libraries(planner_test PRIVATE opaquedb_planner opaquedb_backend)

add_opaquedb_test(server_test server_test.cpp)
target_link_libraries(server_test PRIVATE opaquedb_server opaquedb_admin
  opaquedb_client opaquedb_config opaquedb_storage opaquedb_core)

add_opaquedb_test(auth_test auth_test.cpp)
target_link_libraries(auth_test PRIVATE opaquedb_auth opaquedb_config)

add_opaquedb_test(admin_test admin_test.cpp)
target_link_libraries(admin_test PRIVATE opaquedb_admin opaquedb_server
  opaquedb_config opaquedb_storage opaquedb_core)

add_opaquedb_test(cluster_test cluster_test.cpp)
target_link_libraries(cluster_test PRIVATE opaquedb_cluster)

add_opaquedb_test(distributed_test distributed_test.cpp)
target_link_libraries(distributed_test PRIVATE opaquedb_server opaquedb_client
  opaquedb_config opaquedb_crypto opaquedb_core opaquedb_storage)
