#include "opaquedb/admin/keyring_store.h"

#include <utility>
#include <vector>

#include "absl/strings/str_cat.h"

namespace opaquedb::admin {

absl::Status InMemoryKeyringStore::Put(const std::string &client_id,
                                       crypto::KeyMaterial keys) {
  if (client_id.empty()) {
    return absl::InvalidArgumentError("client id is empty");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  keys_[client_id] = std::move(keys);
  return absl::OkStatus();
}

absl::StatusOr<crypto::KeyMaterial>
InMemoryKeyringStore::Get(const std::string &client_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = keys_.find(client_id);
  if (it == keys_.end()) {
    return absl::NotFoundError(
        absl::StrCat("client '", client_id, "' has not registered keys"));
  }
  return it->second;
}

bool InMemoryKeyringStore::Contains(const std::string &client_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return keys_.find(client_id) != keys_.end();
}

std::vector<std::string> InMemoryKeyringStore::ClientIds() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> ids;
  ids.reserve(keys_.size());
  for (const auto &[id, unused] : keys_)
    ids.push_back(id);
  return ids;
}

} // namespace opaquedb::admin
