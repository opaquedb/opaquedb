#include "opaquedb/backend/registry.h"

#include <string>
#include <utility>

#include "absl/strings/str_cat.h"

namespace opaquedb::backend {

BackendRegistry &BackendRegistry::instance() {
  static BackendRegistry registry;
  return registry;
}

void BackendRegistry::register_backend(absl::string_view name, Factory factory,
                                       BackendCapabilities capabilities) {
  std::lock_guard<std::mutex> lock(mutex_);
  entries_[std::string(name)] =
      Entry{std::move(factory), std::move(capabilities)};
}

absl::StatusOr<std::unique_ptr<PirBackend>>
BackendRegistry::create(absl::string_view name,
                        const crypto::CryptoContext &ctx) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = entries_.find(name);
  if (it == entries_.end()) {
    return absl::NotFoundError(
        absl::StrCat("no backend registered as '", name, "'"));
  }
  return it->second.factory(ctx);
}

std::vector<BackendCapabilities> BackendRegistry::list() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<BackendCapabilities> out;
  out.reserve(entries_.size());
  for (const auto &[name, entry] : entries_)
    out.push_back(entry.capabilities);
  return out;
}

bool BackendRegistry::contains(absl::string_view name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_.find(name) != entries_.end();
}

} // namespace opaquedb::backend
