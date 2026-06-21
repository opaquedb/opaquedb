#include "opaquedb/core/version.h"

#ifndef OPAQUEDB_VERSION
#define OPAQUEDB_VERSION "0.0.0-unknown"
#endif

namespace opaquedb::core {

std::string_view version() { return OPAQUEDB_VERSION; }

} // namespace opaquedb::core
