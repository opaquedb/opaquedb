# Shared compile options for first-party OpaqueDB targets.
#
# Dependencies built by vcpkg must not be held to these warning levels, so the
# options live in an INTERFACE target that only our own libraries link against.
# This keeps the build warning-clean for code we own without fighting upstream.

if(TARGET opaquedb_compile_options)
  return()
endif()

add_library(opaquedb_compile_options INTERFACE)

target_compile_features(opaquedb_compile_options INTERFACE cxx_std_20)

if(MSVC)
  target_compile_options(opaquedb_compile_options INTERFACE /W4 /permissive-)
  if(OPAQUEDB_WERROR)
    target_compile_options(opaquedb_compile_options INTERFACE /WX)
  endif()
else()
  target_compile_options(opaquedb_compile_options INTERFACE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor)
  if(OPAQUEDB_WERROR)
    target_compile_options(opaquedb_compile_options INTERFACE -Werror)
  endif()
endif()

# Coverage instrumentation for first-party code only. The vcpkg dependencies
# (SEAL, gRPC, ...) are not linked against this INTERFACE target, so they are
# never instrumented. We force -O0 on our own translation units (the last -O
# flag wins, overriding the build type) so line attribution is exact; the heavy
# arithmetic lives in the optimized SEAL release build, so this stays fast.
if(OPAQUEDB_COVERAGE AND NOT MSVC)
  target_compile_options(opaquedb_compile_options INTERFACE
    --coverage -O0 -g -fprofile-update=atomic)
  target_link_options(opaquedb_compile_options INTERFACE --coverage)
endif()
