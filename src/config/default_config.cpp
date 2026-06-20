#include "opaquedb/config/default_config_data.h" // generated from the toml
#include "opaquedb/config/loader.h"

namespace opaquedb::config {

// The default config text is embedded from config/opaquedb.default.toml at
// build time (see default_config_data.h.in and CMakeLists.txt). That file is
// the single source of truth for `config init`, the packaging example, and the
// compiled-in defaults, which a test asserts stay in sync.
std::string_view DefaultConfigToml() { return kDefaultConfigTomlData; }

} // namespace opaquedb::config
