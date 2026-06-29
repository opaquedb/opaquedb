find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

# One test executable per source file keeps failures isolated and parallel.
# add_opaquedb_test(name file.cpp... [LABELS lbl...]) builds and registers a
# gtest binary. A test tagged "slow" runs the heavy SEAL matcher at poly 16384;
# the tag lets the fast inner loop skip it with `ctest -LE slow` and lets CI run
# the slow tier separately. The label rides on every discovered case.
function(add_opaquedb_test name)
  cmake_parse_arguments(T "" "" "LABELS" ${ARGN})
  add_executable(${name} ${T_UNPARSED_ARGUMENTS})
  target_link_libraries(${name} PRIVATE
    opaquedb_compile_options
    GTest::gtest
    GTest::gtest_main)
  if(T_LABELS)
    gtest_discover_tests(${name} PROPERTIES LABELS "${T_LABELS}")
  else()
    gtest_discover_tests(${name})
  endif()
endfunction()

add_opaquedb_test(core_version_test core_version_test.cpp)
target_link_libraries(core_version_test PRIVATE opaquedb_core)

add_opaquedb_test(config_test config_test.cpp)
target_link_libraries(config_test PRIVATE opaquedb_config)

add_opaquedb_test(key_codec_test key_codec_test.cpp)
target_link_libraries(key_codec_test PRIVATE opaquedb_core)

# The typed payload codec, including the untrusted-decode bounds checks. Pure
# and fast, so it stays in the default (fast) tier.
add_opaquedb_test(record_codec_test record_codec_test.cpp)
target_link_libraries(record_codec_test PRIVATE opaquedb_core)

# Property sweep over the SIMD slot packing round-trip.
add_opaquedb_test(slot_codec_test slot_codec_test.cpp)
target_link_libraries(slot_codec_test PRIVATE opaquedb_core)

# Schema validation and structural accessors (pure core).
add_opaquedb_test(schema_test schema_test.cpp)
target_link_libraries(schema_test PRIVATE opaquedb_core)

# Epoch manifest parse/validate: the untrusted on-disk manifest gate.
add_opaquedb_test(manifest_test manifest_test.cpp)
target_link_libraries(manifest_test PRIVATE opaquedb_storage opaquedb_core)

add_opaquedb_test(crypto_test crypto_test.cpp LABELS slow)
target_link_libraries(crypto_test PRIVATE opaquedb_crypto)

# The ciphertext wire trust boundary: cheap (no matcher), so it stays in the
# fast tier even though it links crypto.
add_opaquedb_test(serialize_test serialize_test.cpp)
target_link_libraries(serialize_test PRIVATE opaquedb_crypto)

add_opaquedb_test(reference_backend_test reference_backend_test.cpp LABELS slow)
target_link_libraries(reference_backend_test PRIVATE
  opaquedb_backend opaquedb_backend_reference opaquedb_crypto opaquedb_core)

add_opaquedb_test(storage_test storage_test.cpp)
target_link_libraries(storage_test PRIVATE opaquedb_storage opaquedb_core)

add_opaquedb_test(catalog_test catalog_test.cpp)
target_link_libraries(catalog_test PRIVATE opaquedb_storage opaquedb_core
  opaquedb_config)

add_opaquedb_test(sql_test sql_test.cpp)
target_link_libraries(sql_test PRIVATE opaquedb_sql)

# Client-side result presentation (table rendering, DISTINCT/ORDER BY/projection
# over decoded rows). Pure, no matcher, so it stays in the fast tier.
add_opaquedb_test(cli_render_test cli_render_test.cpp)
target_link_libraries(cli_render_test PRIVATE opaquedb_cli opaquedb_sql
  opaquedb_core)

add_opaquedb_test(planner_test planner_test.cpp)
target_link_libraries(planner_test PRIVATE opaquedb_planner opaquedb_backend)

add_opaquedb_test(server_test server_test.cpp LABELS slow)
target_link_libraries(server_test PRIVATE opaquedb_server opaquedb_admin
  opaquedb_client opaquedb_config opaquedb_storage opaquedb_core)

add_opaquedb_test(auth_test auth_test.cpp)
target_link_libraries(auth_test PRIVATE opaquedb_auth opaquedb_config)

add_opaquedb_test(request_gate_test request_gate_test.cpp)
target_link_libraries(request_gate_test PRIVATE opaquedb_server opaquedb_auth)

add_opaquedb_test(admin_test admin_test.cpp)
target_link_libraries(admin_test PRIVATE opaquedb_admin opaquedb_server
  opaquedb_config opaquedb_storage opaquedb_core)

# The persistent file-backed keyring: one keyset per client id, surviving
# restarts. Opaque bytes, no SEAL, so it stays in the fast tier.
add_opaquedb_test(file_keyring_store_test file_keyring_store_test.cpp)
target_link_libraries(file_keyring_store_test PRIVATE opaquedb_admin)

add_opaquedb_test(cluster_test cluster_test.cpp)
target_link_libraries(cluster_test PRIVATE opaquedb_cluster)

add_opaquedb_test(distributed_test distributed_test.cpp LABELS slow)
target_link_libraries(distributed_test PRIVATE opaquedb_server opaquedb_client
  opaquedb_config opaquedb_crypto opaquedb_core opaquedb_storage)

add_opaquedb_test(transport_security_test transport_security_test.cpp LABELS slow)
target_link_libraries(transport_security_test PRIVATE opaquedb_server
  opaquedb_client opaquedb_config opaquedb_crypto opaquedb_core opaquedb_storage)

# Reproductions for suspected matcher correctness bugs (run the real matcher).
add_opaquedb_test(matcher_bug_repro_test matcher_bug_repro_test.cpp LABELS slow)
target_link_libraries(matcher_bug_repro_test PRIVATE
  opaquedb_backend opaquedb_backend_reference opaquedb_crypto opaquedb_core)
