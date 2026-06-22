#include "opaquedb/storage/mapped_file.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

#include "absl/strings/str_cat.h"

namespace opaquedb::storage {
namespace {

// Closes a file descriptor, retrying on EINTR. Used after mmap, which keeps the
// mapping alive independent of the descriptor.
void CloseFd(int fd) {
  while (::close(fd) == -1 && errno == EINTR) {
  }
}

} // namespace

absl::StatusOr<MappedFile> MappedFile::Open(const std::string &path) {
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd == -1) {
    return absl::NotFoundError(
        absl::StrCat("cannot open '", path, "': ", std::strerror(errno)));
  }

  struct stat st {};
  if (::fstat(fd, &st) == -1) {
    int saved = errno;
    CloseFd(fd);
    return absl::InternalError(
        absl::StrCat("cannot stat '", path, "': ", std::strerror(saved)));
  }
  if (!S_ISREG(st.st_mode)) {
    CloseFd(fd);
    return absl::InvalidArgumentError(
        absl::StrCat("'", path, "' is not a regular file"));
  }
  if (st.st_size <= 0) {
    CloseFd(fd);
    return absl::InvalidArgumentError(absl::StrCat("'", path, "' is empty"));
  }

  // off_t is signed; we have already rejected <= 0. Guard the conversion to
  // size_t so a value that does not fit cannot become a bogus length.
  const auto file_size = static_cast<std::uintmax_t>(st.st_size);
  if (file_size > static_cast<std::uintmax_t>(SIZE_MAX)) {
    CloseFd(fd);
    return absl::OutOfRangeError(
        absl::StrCat("'", path, "' is too large to map"));
  }
  const std::size_t size = static_cast<std::size_t>(file_size);

  void *addr = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
  CloseFd(fd); // the mapping outlives the descriptor
  if (addr == MAP_FAILED) {
    return absl::InternalError(
        absl::StrCat("cannot mmap '", path, "': ", std::strerror(errno)));
  }
  return MappedFile(static_cast<const std::uint8_t *>(addr), size);
}

MappedFile::~MappedFile() { Reset(); }

MappedFile::MappedFile(MappedFile &&other) noexcept
    : data_(other.data_), size_(other.size_) {
  other.data_ = nullptr;
  other.size_ = 0;
}

MappedFile &MappedFile::operator=(MappedFile &&other) noexcept {
  if (this != &other) {
    Reset();
    data_ = other.data_;
    size_ = other.size_;
    other.data_ = nullptr;
    other.size_ = 0;
  }
  return *this;
}

void MappedFile::Reset() noexcept {
  if (data_ != nullptr && size_ > 0) {
    ::munmap(const_cast<std::uint8_t *>(data_), size_);
  }
  data_ = nullptr;
  size_ = 0;
}

} // namespace opaquedb::storage
