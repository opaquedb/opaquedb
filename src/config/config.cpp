#include "opaquedb/config/config.h"

#include "absl/strings/str_cat.h"

namespace opaquedb::config {

std::string Config::EffectiveEpochDir() const {
  if (!storage.epoch_dir.empty()) {
    return storage.epoch_dir;
  }
  return node.data_dir + "/epochs";
}

std::string Config::EpochRootFor(std::string_view database,
                                 std::string_view table) const {
  return absl::StrCat(DatabasesDir(), "/", database, "/", table, "/epochs");
}

std::string Config::DatabasesDir() const {
  const std::string base =
      storage.epoch_dir.empty() ? node.data_dir : storage.epoch_dir;
  return base + "/db";
}

std::string Config::KeyringDir() const {
  if (!blobstore.path.empty()) {
    return blobstore.path;
  }
  return node.data_dir + "/keys";
}

std::string ToString(AuthMode mode) {
  switch (mode) {
  case AuthMode::kToken:
    return "token";
  case AuthMode::kMtls:
    return "mtls";
  case AuthMode::kNone:
    return "none";
  }
  return "token";
}

std::string ToString(BlobStoreKind kind) {
  switch (kind) {
  case BlobStoreKind::kLocal:
    return "local";
  case BlobStoreKind::kS3:
    return "s3";
  }
  return "local";
}

std::string ToString(LogFormat format) {
  switch (format) {
  case LogFormat::kJson:
    return "json";
  case LogFormat::kText:
    return "text";
  }
  return "json";
}

} // namespace opaquedb::config
