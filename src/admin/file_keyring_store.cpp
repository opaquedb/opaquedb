#include "opaquedb/admin/file_keyring_store.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/strings/str_cat.h"

namespace opaquedb::admin {
namespace {

namespace fs = std::filesystem;

// Magic and version for the on-disk key file. A mismatch means the file is not
// ours or is from an incompatible layout, and is rejected rather than parsed.
constexpr std::string_view kMagic = "OQKEYR01";

void AppendU64(std::string &out, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<char>(value & 0xFF));
    value >>= 8;
  }
}

// Reads a little-endian u64 at offset, advancing it. Returns false if there are
// not 8 bytes left, so a truncated file can never drive an over-read.
bool ReadU64(std::string_view buf, std::size_t &offset, std::uint64_t &out) {
  if (offset + 8 > buf.size())
    return false;
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i)
    value |=
        static_cast<std::uint64_t>(static_cast<unsigned char>(buf[offset + i]))
        << (8 * i);
  offset += 8;
  out = value;
  return true;
}

// Reads a length-prefixed blob, validating the declared length against the
// bytes that actually remain before copying.
bool ReadBlob(std::string_view buf, std::size_t &offset, std::string &out) {
  std::uint64_t len = 0;
  if (!ReadU64(buf, offset, len))
    return false;
  if (len > buf.size() - offset)
    return false;
  out.assign(buf.data() + offset, len);
  offset += len;
  return true;
}

std::string HexEncode(const std::string &in) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(in.size() * 2);
  for (char ch : in) {
    const auto c = static_cast<unsigned char>(ch);
    out.push_back(kDigits[c >> 4]);
    out.push_back(kDigits[c & 0xF]);
  }
  return out;
}

bool HexDecode(std::string_view in, std::string &out) {
  if (in.size() % 2 != 0)
    return false;
  auto nibble = [](char c, int &v) -> bool {
    if (c >= '0' && c <= '9')
      v = c - '0';
    else if (c >= 'a' && c <= 'f')
      v = c - 'a' + 10;
    else
      return false;
    return true;
  };
  out.clear();
  out.reserve(in.size() / 2);
  for (std::size_t i = 0; i < in.size(); i += 2) {
    int hi = 0;
    int lo = 0;
    if (!nibble(in[i], hi) || !nibble(in[i + 1], lo))
      return false;
    out.push_back(static_cast<char>((hi << 4) | lo));
  }
  return true;
}

std::string Serialize(const crypto::KeyMaterial &keys) {
  std::string out;
  out.append(kMagic);
  AppendU64(out, keys.public_key.size());
  out.append(keys.public_key);
  AppendU64(out, keys.relin_keys.size());
  out.append(keys.relin_keys);
  AppendU64(out, keys.galois_keys.size());
  out.append(keys.galois_keys);
  return out;
}

absl::StatusOr<crypto::KeyMaterial> Deserialize(std::string_view buf) {
  if (buf.size() < kMagic.size() || buf.substr(0, kMagic.size()) != kMagic)
    return absl::DataLossError("key file has a bad or missing magic");
  std::size_t offset = kMagic.size();
  crypto::KeyMaterial keys;
  if (!ReadBlob(buf, offset, keys.public_key) ||
      !ReadBlob(buf, offset, keys.relin_keys) ||
      !ReadBlob(buf, offset, keys.galois_keys))
    return absl::DataLossError("key file is truncated or malformed");
  return keys;
}

} // namespace

absl::StatusOr<std::unique_ptr<FileKeyringStore>>
FileKeyringStore::Open(const std::string &dir) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec)
    return absl::InternalError(
        absl::StrCat("cannot create keyring dir '", dir, "': ", ec.message()));
  if (!fs::is_directory(dir, ec))
    return absl::InvalidArgumentError(
        absl::StrCat("keyring path '", dir, "' is not a directory"));
  return std::unique_ptr<FileKeyringStore>(new FileKeyringStore(dir));
}

std::string FileKeyringStore::PathFor(const std::string &client_id) const {
  return (fs::path(dir_) / (HexEncode(client_id) + ".keys")).string();
}

absl::Status FileKeyringStore::Put(const std::string &client_id,
                                   crypto::KeyMaterial keys) {
  if (client_id.empty())
    return absl::InvalidArgumentError("client id is empty");

  const std::string serialized = Serialize(keys);
  const std::string path = PathFor(client_id);
  const std::string tmp = path + ".tmp";

  std::lock_guard<std::mutex> lock(mutex_);
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out)
      return absl::InternalError(
          absl::StrCat("cannot open temp key file '", tmp, "'"));
    out.write(serialized.data(),
              static_cast<std::streamsize>(serialized.size()));
    out.flush();
    if (!out)
      return absl::InternalError(
          absl::StrCat("failed to write key file '", tmp, "'"));
  }

  std::error_code ec;
  // Owner read/write only: this is secret-adjacent material.
  fs::permissions(tmp, fs::perms::owner_read | fs::perms::owner_write,
                  fs::perm_options::replace, ec);
  // A re-register overwrites: rename replaces any existing file atomically, so
  // a client id always maps to exactly one keyset.
  fs::rename(tmp, path, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return absl::InternalError(
        absl::StrCat("cannot publish key file '", path, "': ", ec.message()));
  }
  cache_[client_id] = std::move(keys);
  return absl::OkStatus();
}

absl::StatusOr<crypto::KeyMaterial>
FileKeyringStore::Get(const std::string &client_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = cache_.find(client_id);
  if (it != cache_.end())
    return it->second;

  const std::string path = PathFor(client_id);
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return absl::NotFoundError(
        absl::StrCat("client '", client_id, "' has not registered keys"));
  std::string bytes((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
  if (!in.good() && !in.eof())
    return absl::InternalError(
        absl::StrCat("failed to read key file for client '", client_id, "'"));

  absl::StatusOr<crypto::KeyMaterial> keys = Deserialize(bytes);
  if (!keys.ok())
    return keys.status();
  cache_[client_id] = *keys;
  return keys;
}

bool FileKeyringStore::Contains(const std::string &client_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (cache_.find(client_id) != cache_.end())
    return true;
  std::error_code ec;
  return fs::exists(PathFor(client_id), ec);
}

std::vector<std::string> FileKeyringStore::ClientIds() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> ids;
  std::error_code ec;
  for (const auto &entry : fs::directory_iterator(dir_, ec)) {
    if (ec)
      break;
    const fs::path &p = entry.path();
    if (p.extension() != ".keys")
      continue;
    std::string decoded;
    if (HexDecode(p.stem().string(), decoded))
      ids.push_back(std::move(decoded));
  }
  return ids;
}

} // namespace opaquedb::admin
