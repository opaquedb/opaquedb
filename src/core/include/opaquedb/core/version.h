#ifndef OPAQUEDB_CORE_VERSION_H_
#define OPAQUEDB_CORE_VERSION_H_

#include <string_view>

namespace opaquedb::core {

// The build-time version string, set from the project version by CMake.
// Packaging derives the project version from the git tag, so this matches the
// installed package version.
std::string_view version();

} // namespace opaquedb::core

#endif // OPAQUEDB_CORE_VERSION_H_
