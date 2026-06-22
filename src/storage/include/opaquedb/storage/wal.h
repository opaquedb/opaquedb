#ifndef OPAQUEDB_STORAGE_WAL_H_
#define OPAQUEDB_STORAGE_WAL_H_

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

// The write-ahead log. Writes are appended here before they accumulate into a
// staging epoch, so they survive a crash before the next epoch is published.
// Each entry is length-prefixed and CRC-checked. On recovery the log is
// replayed up to the first entry that is incomplete or fails its CRC, which is
// the normal shape of a crash that interrupted a write. The write path never
// mmaps the log.

namespace opaquedb::storage {

inline constexpr std::uint32_t kWalFormatVersion = 1;

// Appends opaque entries to a log file durably (fsync per append).
class WalWriter {
public:
  // Opens the log for appending, creating it if absent.
  static absl::StatusOr<WalWriter> Open(const std::string &path);

  WalWriter() = default;
  ~WalWriter();
  WalWriter(WalWriter &&other) noexcept;
  WalWriter &operator=(WalWriter &&other) noexcept;
  WalWriter(const WalWriter &) = delete;
  WalWriter &operator=(const WalWriter &) = delete;

  // Appends one entry and fsyncs. The entry is durable when this returns ok.
  absl::Status Append(std::span<const std::uint8_t> entry);

  absl::Status Close();

private:
  explicit WalWriter(int fd, std::string path)
      : fd_(fd), path_(std::move(path)) {}

  int fd_ = -1;
  std::string path_;
};

// Replays a log, returning the valid prefix of entries. A missing file yields
// an empty list. A truncated or CRC-failing tail is treated as a crash artifact
// and stops the replay; the entries before it are returned.
absl::StatusOr<std::vector<std::vector<std::uint8_t>>>
ReplayWal(const std::string &path);

} // namespace opaquedb::storage

#endif // OPAQUEDB_STORAGE_WAL_H_
