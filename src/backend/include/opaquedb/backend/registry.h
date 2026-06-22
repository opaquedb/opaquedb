#ifndef OPAQUEDB_BACKEND_REGISTRY_H_
#define OPAQUEDB_BACKEND_REGISTRY_H_

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "opaquedb/backend/capabilities.h"
#include "opaquedb/backend/pir_backend.h"
#include "opaquedb/crypto/context.h"

// Registry plus Factory. Backends self-register a factory by name through a
// static initializer, so linking a backend library registers it. The planner
// looks a backend up by name or filters the registered capabilities. The
// factory takes the node's FHE context so a constructed backend is bound to the
// parameters from config; per-client keys arrive later through load_keys.

namespace opaquedb::backend {

class BackendRegistry {
public:
  using Factory =
      std::function<std::unique_ptr<PirBackend>(const crypto::CryptoContext &)>;

  // The process-wide registry that backends self-register with at static init.
  static BackendRegistry &instance();

  // A registry can also be constructed directly and injected, so a unit can be
  // tested with a controlled set of backends instead of the global one. It is
  // not copyable or movable; hold it by reference.
  BackendRegistry() = default;
  BackendRegistry(const BackendRegistry &) = delete;
  BackendRegistry &operator=(const BackendRegistry &) = delete;

  // Registers a factory under a name. Later registration of the same name
  // replaces the earlier one; registering is expected only at static init.
  void register_backend(absl::string_view name, Factory factory,
                        BackendCapabilities capabilities);

  // Constructs the named backend bound to the given context.
  absl::StatusOr<std::unique_ptr<PirBackend>>
  create(absl::string_view name, const crypto::CryptoContext &ctx) const;

  // Capabilities of every registered backend, for the planner's selection.
  std::vector<BackendCapabilities> list() const;

  bool contains(absl::string_view name) const;

private:
  struct Entry {
    Factory factory;
    BackendCapabilities capabilities;
  };

  mutable std::mutex mutex_;
  std::map<std::string, Entry, std::less<>> entries_;
};

} // namespace opaquedb::backend

#endif // OPAQUEDB_BACKEND_REGISTRY_H_
