#include "io_util.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>

#include "absl/strings/str_cat.h"

namespace opaquedb::storage {
namespace {

void CloseFd(int fd) {
  while (::close(fd) == -1 && errno == EINTR) {
  }
}

// Writes all bytes, retrying short writes and EINTR.
absl::Status WriteAll(int fd, std::span<const std::uint8_t> data,
                      const std::string &path) {
  std::size_t written = 0;
  while (written < data.size()) {
    const ssize_t n = ::write(fd, data.data() + written, data.size() - written);
    if (n == -1) {
      if (errno == EINTR)
        continue;
      return absl::InternalError(
          absl::StrCat("write '", path, "' failed: ", std::strerror(errno)));
    }
    written += static_cast<std::size_t>(n);
  }
  return absl::OkStatus();
}

} // namespace

absl::Status WriteFileDurably(const std::string &path,
                              std::span<const std::uint8_t> data) {
  const int fd =
      ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0640);
  if (fd == -1) {
    return absl::InternalError(
        absl::StrCat("cannot create '", path, "': ", std::strerror(errno)));
  }
  if (absl::Status s = WriteAll(fd, data, path); !s.ok()) {
    CloseFd(fd);
    return s;
  }
  if (::fsync(fd) == -1) {
    int saved = errno;
    CloseFd(fd);
    return absl::InternalError(
        absl::StrCat("fsync '", path, "' failed: ", std::strerror(saved)));
  }
  CloseFd(fd);
  return absl::OkStatus();
}

absl::Status FsyncDir(const std::string &dir) {
  const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd == -1) {
    return absl::InternalError(
        absl::StrCat("cannot open dir '", dir, "': ", std::strerror(errno)));
  }
  const int rc = ::fsync(fd);
  int saved = errno;
  CloseFd(fd);
  if (rc == -1) {
    return absl::InternalError(
        absl::StrCat("fsync dir '", dir, "' failed: ", std::strerror(saved)));
  }
  return absl::OkStatus();
}

absl::Status RenameDurably(const std::string &from, const std::string &to) {
  if (::rename(from.c_str(), to.c_str()) == -1) {
    return absl::InternalError(absl::StrCat(
        "rename '", from, "' -> '", to, "' failed: ", std::strerror(errno)));
  }
  std::filesystem::path parent = std::filesystem::path(to).parent_path();
  if (parent.empty())
    parent = ".";
  return FsyncDir(parent.string());
}

absl::StatusOr<std::string> ReadSmallFile(const std::string &path,
                                          std::size_t max_bytes) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd == -1) {
    if (errno == ENOENT) {
      return absl::NotFoundError(absl::StrCat("no such file '", path, "'"));
    }
    return absl::InternalError(
        absl::StrCat("cannot open '", path, "': ", std::strerror(errno)));
  }
  struct stat st {};
  if (::fstat(fd, &st) == -1) {
    int saved = errno;
    CloseFd(fd);
    return absl::InternalError(
        absl::StrCat("cannot stat '", path, "': ", std::strerror(saved)));
  }
  if (st.st_size < 0 || static_cast<std::uintmax_t>(st.st_size) > max_bytes) {
    CloseFd(fd);
    return absl::OutOfRangeError(absl::StrCat(
        "'", path, "' is larger than the ", max_bytes, " byte limit"));
  }
  std::string out;
  out.resize(static_cast<std::size_t>(st.st_size));
  std::size_t read_total = 0;
  while (read_total < out.size()) {
    const ssize_t n =
        ::read(fd, out.data() + read_total, out.size() - read_total);
    if (n == -1) {
      if (errno == EINTR)
        continue;
      int saved = errno;
      CloseFd(fd);
      return absl::InternalError(
          absl::StrCat("read '", path, "' failed: ", std::strerror(saved)));
    }
    if (n == 0)
      break;
    read_total += static_cast<std::size_t>(n);
  }
  out.resize(read_total);
  CloseFd(fd);
  return out;
}

} // namespace opaquedb::storage
