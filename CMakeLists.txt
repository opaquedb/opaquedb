cmake_minimum_required(VERSION 3.24)

# Project version. Packaging (step 14) derives this from the git tag and embeds
# it into the binary so `opaquedb --version` matches the package version. Until
# then this literal is the single source of truth.
project(OpaqueDB
  VERSION 0.5.0
  DESCRIPTION "Post-quantum distributed private database."
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to a build type so a bare `cmake .` is not a surprise.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(OPAQUEDB_WERROR "Treat warnings as errors for first-party targets" OFF)
option(OPAQUEDB_BUILD_TESTS "Build the test suite" ON)
option(OPAQUEDB_COVERAGE "Instrument first-party targets for gcov coverage" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(OpaqueDBCompileOptions)

# Linking the full static dependency set (SEAL, gRPC, protobuf) is memory heavy.
# Serialize link steps with a Ninja job pool and prefer the gold linker, which
# uses less memory, so the build is not OOM-killed on a constrained machine.
set_property(GLOBAL PROPERTY JOB_POOLS link_pool=1)
set(CMAKE_JOB_POOL_LINK link_pool)
find_program(OPAQUEDB_GOLD_LINKER ld.gold)
if(OPAQUEDB_GOLD_LINKER)
  add_link_options(-fuse-ld=gold)
endif()

# The project VERSION above is the single source of truth for the version. It
# flows to the compiled binary (OPAQUEDB_VERSION -> core::version -> --version)
# and to CPack below. vcpkg.json must carry a literal, so verify it matches here
# rather than letting the two drift; configure fails loudly if they disagree.
add_compile_definitions(OPAQUEDB_VERSION="${PROJECT_VERSION}")

file(READ ${CMAKE_SOURCE_DIR}/vcpkg.json OPAQUEDB_VCPKG_JSON)
string(JSON OPAQUEDB_VCPKG_VERSION GET "${OPAQUEDB_VCPKG_JSON}" version)
if(NOT OPAQUEDB_VCPKG_VERSION STREQUAL PROJECT_VERSION)
  message(FATAL_ERROR
    "version mismatch: vcpkg.json is ${OPAQUEDB_VCPKG_VERSION} but project() is "
    "${PROJECT_VERSION}. Update vcpkg.json \"version\" to match.")
endif()

find_package(Threads REQUIRED)
find_package(CLI11 CONFIG REQUIRED)
find_package(replxx CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)
find_package(absl CONFIG REQUIRED)
find_package(SEAL CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

# Each src subdirectory is its own library target with explicit visibility.
# The dependency direction is enforced by which targets each library links.
add_subdirectory(src/core)
add_subdirectory(src/config)
add_subdirectory(src/log)
add_subdirectory(src/crypto)
add_subdirectory(src/storage)
add_subdirectory(src/backend)
add_subdirectory(src/backend/reference)
add_subdirectory(src/sql)
add_subdirectory(src/planner)
add_subdirectory(src/auth)
add_subdirectory(src/admin)
add_subdirectory(src/cluster)
add_subdirectory(proto)
add_subdirectory(src/server)
add_subdirectory(src/client)
add_subdirectory(src/cli)

# The single binary that wires the libraries together.
add_executable(opaquedb src/main.cpp)
target_link_libraries(opaquedb PRIVATE
  opaquedb_cli
  opaquedb_compile_options)

include(GNUInstallDirs)
install(TARGETS opaquedb RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Packaging artifacts: the systemd unit and an example config. The example is
# generated from the single canonical default (config/opaquedb.default.toml) so
# it never drifts from the compiled-in defaults. The default config itself is
# written on install by the postinst (from `config init`), so it is not shipped
# as a conffile to avoid clobbering operator edits.
configure_file(${CMAKE_SOURCE_DIR}/config/opaquedb.default.toml
               ${CMAKE_BINARY_DIR}/opaquedb.toml.example COPYONLY)
install(FILES packaging/opaquedb.service
        DESTINATION lib/systemd/system)
install(FILES ${CMAKE_BINARY_DIR}/opaquedb.toml.example
        DESTINATION ${CMAKE_INSTALL_DOCDIR})

# CPack builds the .deb (the full install tree: binary, systemd unit, example
# config). Run from a release build: `cmake --preset release && cmake --build
# build/release && (cd build/release && cpack)`.
set(CPACK_PACKAGE_NAME "opaquedb")
set(CPACK_PACKAGE_VENDOR "OpaqueDB")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Distributed PIR database answering SQL-like queries over encrypted data")
set(CPACK_PACKAGE_CONTACT "halptekin@schubergphilis.com")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_GENERATOR "DEB")
set(CPACK_DEBIAN_PACKAGE_SECTION "database")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/postinst;${CMAKE_CURRENT_SOURCE_DIR}/packaging/prerm;${CMAKE_CURRENT_SOURCE_DIR}/packaging/postrm")
include(CPack)

# Binary-only release tarball, named to match the .deb (Debian style:
# opaquedb_<version>_<arch>.tar.gz). The .deb carries the systemd unit and
# example config; this tarball is just the bare binary for a drop-in install.
# Architecture token comes from dpkg so it matches the .deb exactly (amd64),
# falling back to the CMake processor name off Debian.
find_program(DPKG_EXECUTABLE dpkg)
if(DPKG_EXECUTABLE)
  execute_process(COMMAND ${DPKG_EXECUTABLE} --print-architecture
                  OUTPUT_VARIABLE OPAQUEDB_PKG_ARCH
                  OUTPUT_STRIP_TRAILING_WHITESPACE)
else()
  set(OPAQUEDB_PKG_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
endif()
set(OPAQUEDB_TARBALL_NAME
    "opaquedb_${PROJECT_VERSION}_${OPAQUEDB_PKG_ARCH}.tar.gz")
add_custom_target(tarball
  COMMAND ${CMAKE_COMMAND} -E tar czf
          "${CMAKE_BINARY_DIR}/${OPAQUEDB_TARBALL_NAME}" opaquedb
  WORKING_DIRECTORY "$<TARGET_FILE_DIR:opaquedb>"
  DEPENDS opaquedb
  VERBATIM
  COMMENT "Packaging ${OPAQUEDB_TARBALL_NAME} (binary only)")

if(OPAQUEDB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

option(OPAQUEDB_BUILD_EXAMPLES "Build example client and direct-access programs" ON)
if(OPAQUEDB_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
