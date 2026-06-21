#ifndef OPAQUEDB_CORE_WIRE_H_
#define OPAQUEDB_CORE_WIRE_H_

#include <cstdint>

// The wire schema version carried in the protos. Bumping it lets a node reject
// a client or peer that speaks an incompatible version rather than misread its
// messages. It is defined once here and used by the server and the dev client.

namespace opaquedb::core {

inline constexpr std::uint32_t kWireVersion = 1;

} // namespace opaquedb::core

#endif // OPAQUEDB_CORE_WIRE_H_
