#ifndef OPAQUEDB_ADMIN_FILE_KEYRING_STORE_H_
#define OPAQUEDB_ADMIN_FILE_KEYRING_STORE_H_

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "opaquedb/admin/keyring_store.h"
#include "opaquedb/crypto/key_material.h"

// A keyring store that persists each client's public and evaluation keys to
// disk, one file per client id, under a directory (config.keyring.path). The
// in-memory store loses every registration on restart, which forces clients to
// re-register their ~110 MB Galois set on every node bounce. This store keeps
// them across restarts, so a client registers once and the keys are read back
// lazily from disk on the first query after a restart.
//
// One file per client id is the storage form of the rule "one client id maps to
// exactly one keyset": a re-register overwrites the file atomically (temp file
// then rename), it never appends. The secret key is never here, same as the
// in-memory store.

namespace opaquedb::admin {

class FileKeyringStore : public KeyringStore {
public:
  // Opens (creating if absent) the directory that holds the per-client key
  // files. Fails if the path exists as a non-directory or cannot be created.
  static absl::StatusOr<std::unique_ptr<FileKeyringStore>>
  Open(const std::string &dir);

  absl::Status Put(const std::string &client_id,
                   crypto::KeyMaterial keys) override;
  absl::StatusOr<crypto::KeyMaterial>
  Get(const std::string &client_id) const override;
  bool Contains(const std::string &client_id) const override;
  std::vector<std::string> ClientIds() const override;

private:
  explicit FileKeyringStore(std::string dir) : dir_(std::move(dir)) {}

  // The on-disk path for a client id. The id is hex encoded so any byte string
  // is a safe file name and ClientIds can decode it back.
  std::string PathFor(const std::string &client_id) const;

  std::string dir_;
  mutable std::mutex mutex_;
  // A read-through cache so a repeated query does not reload the large Galois
  // set from disk on every eval-context miss.
  mutable std::map<std::string, crypto::KeyMaterial> cache_;
};

} // namespace opaquedb::admin

#endif // OPAQUEDB_ADMIN_FILE_KEYRING_STORE_H_
