#ifndef OPAQUEDB_ADMIN_KEYRING_STORE_H_
#define OPAQUEDB_ADMIN_KEYRING_STORE_H_

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "opaquedb/crypto/key_material.h"

// Stores each registered client's public and evaluation keys, keyed by client
// id. Auth credentials and the keyring are linked but distinct: auth maps a
// credential to a client id, and this store maps a client id to its keys. The
// secret key is never here.
//
// An interface: the node persists keys with FileKeyringStore (one file per
// client id, surviving restarts), while tests use the in-memory store.

namespace opaquedb::admin {

class KeyringStore {
public:
  virtual ~KeyringStore() = default;
  virtual absl::Status Put(const std::string &client_id,
                           crypto::KeyMaterial keys) = 0;
  virtual absl::StatusOr<crypto::KeyMaterial>
  Get(const std::string &client_id) const = 0;
  virtual bool Contains(const std::string &client_id) const = 0;
  // The ids of all registered clients, for keyring inspection by the admin API.
  virtual std::vector<std::string> ClientIds() const = 0;
};

class InMemoryKeyringStore : public KeyringStore {
public:
  absl::Status Put(const std::string &client_id,
                   crypto::KeyMaterial keys) override;
  absl::StatusOr<crypto::KeyMaterial>
  Get(const std::string &client_id) const override;
  bool Contains(const std::string &client_id) const override;
  std::vector<std::string> ClientIds() const override;

private:
  mutable std::mutex mutex_;
  std::map<std::string, crypto::KeyMaterial> keys_;
};

} // namespace opaquedb::admin

#endif // OPAQUEDB_ADMIN_KEYRING_STORE_H_
