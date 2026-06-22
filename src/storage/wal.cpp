#include "opaquedb/storage/wal.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

#include "absl/strings/str_cat.h"
#include "crc32.h"
#include "little_endian.h"

namespace opaquedb::storage {
namespace {

constexpr std::size_t kEntryHeaderSize = 8; // u32 length + u32 crc32

void CloseFd(int fd) {
  while (fd != -1 && ::close(fd) == -1 && errno == EINTR) {
  }
}

absl::Status WriteAll(int fd, std::span<const std::uint8_t> data,
                      const std::string &path) {
  std::size_t written = 0;
  while (written < data.size()) {
    const ssize_t n = ::write(fd, data.data() + written, data.size() - written);
    if (n == -1) {
      if (errno == EINTR)
        continue;
      return absl::InternalError(absl::StrCat(
          "wal write '", path, "' failed: ", std::strerror(errno)));
    }
    written += static_cast<std::size_t>(n);
  }
  return absl::OkStatus();
}

// Reads a whole file into memory. Returns an empty buffer if the file is
// absent. The write path does not mmap, so recovery reads with read().
absl::StatusOr<std::vector<std::uint8_t>>
ReadWholeFile(const std::string &path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd == -1) {
    if (errno == ENOENT)
      return std::vector<std::uint8_t>{};
    return absl::InternalError(
        absl::StrCat("cannot open wal '", path, "': ", std::strerror(errno)));
  }
  struct stat st {};
  if (::fstat(fd, &st) == -1) {
    int saved = errno;
    CloseFd(fd);
    return absl::InternalError(
        absl::StrCat("cannot stat wal '", path, "': ", std::strerror(saved)));
  }
  if (st.st_size <= 0) {
    CloseFd(fd);
    return std::vector<std::uint8_t>{};
  }
  std::vector<std::uint8_t> buffer(static_cast<std::size_t>(st.st_size));
  std::size_t read_total = 0;
  while (read_total < buffer.size()) {
    const ssize_t n =
        ::read(fd, buffer.data() + read_total, buffer.size() - read_total);
    if (n == -1) {
      if (errno == EINTR)
        continue;
      int saved = errno;
      CloseFd(fd);
      return absl::InternalError(
          absl::StrCat("read wal '", path, "' failed: ", std::strerror(saved)));
    }
    if (n == 0)
      break; // file shrank under us; stop with what we have
    read_total += static_cast<std::size_t>(n);
  }
  buffer.resize(read_total);
  CloseFd(fd);
  return buffer;
}

} // namespace

absl::StatusOr<WalWriter> WalWriter::Open(const std::string &path) {
  const int fd =
      ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
  if (fd == -1) {
    return absl::InternalError(
        absl::StrCat("cannot open wal '", path, "': ", std::strerror(errno)));
  }
  return WalWriter(fd, path);
}

WalWriter::~WalWriter() { CloseFd(fd_); }

WalWriter::WalWriter(WalWriter &&other) noexcept
    : fd_(other.fd_), path_(std::move(other.path_)) {
  other.fd_ = -1;
}

WalWriter &WalWriter::operator=(WalWriter &&other) noexcept {
  if (this != &other) {
    CloseFd(fd_);
    fd_ = other.fd_;
    path_ = std::move(other.path_);
    other.fd_ = -1;
  }
  return *this;
}

absl::Status WalWriter::Append(std::span<const std::uint8_t> entry) {
  if (fd_ == -1)
    return absl::FailedPreconditionError("wal is not open");
  if (entry.size() > 0xFFFFFFFFu) {
    return absl::InvalidArgumentError("wal entry exceeds 4 GiB");
  }
  std::vector<std::uint8_t> framed;
  framed.reserve(kEntryHeaderSize + entry.size());
  AppendU32LE(framed, static_cast<std::uint32_t>(entry.size()));
  AppendU32LE(framed, Crc32(entry));
  framed.insert(framed.end(), entry.begin(), entry.end());

  if (absl::Status s = WriteAll(fd_, framed, path_); !s.ok())
    return s;
  if (::fsync(fd_) == -1) {
    return absl::InternalError(
        absl::StrCat("fsync wal '", path_, "' failed: ", std::strerror(errno)));
  }
  return absl::OkStatus();
}

absl::Status WalWriter::Close() {
  if (fd_ == -1)
    return absl::OkStatus();
  const int fd = fd_;
  fd_ = -1;
  if (::close(fd) == -1 && errno != EINTR) {
    return absl::InternalError(
        absl::StrCat("close wal '", path_, "' failed: ", std::strerror(errno)));
  }
  return absl::OkStatus();
}

absl::StatusOr<std::vector<std::vector<std::uint8_t>>>
ReplayWal(const std::string &path) {
  absl::StatusOr<std::vector<std::uint8_t>> buffer = ReadWholeFile(path);
  if (!buffer.ok())
    return buffer.status();
  std::span<const std::uint8_t> data(*buffer);

  std::vector<std::vector<std::uint8_t>> entries;
  std::size_t offset = 0;
  while (offset + kEntryHeaderSize <= data.size()) {
    const std::uint32_t length = *ReadU32LE(data, offset);
    const std::uint32_t stored_crc = *ReadU32LE(data, offset + 4);
    const std::size_t payload_start = offset + kEntryHeaderSize;
    // A length that runs past the end is a torn tail. Stop; keep the prefix.
    if (length > data.size() - payload_start)
      break;
    std::span<const std::uint8_t> payload = data.subspan(payload_start, length);
    if (Crc32(payload) != stored_crc)
      break; // corrupt entry, stop
    entries.emplace_back(payload.begin(), payload.end());
    offset = payload_start + length;
  }
  return entries;
}

} // namespace opaquedb::storage
