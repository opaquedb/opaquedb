#ifndef OPAQUEDB_STORAGE_MAPPED_FILE_H_
#define OPAQUEDB_STORAGE_MAPPED_FILE_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "absl/status/statusor.h"

// A read-only memory mapping of a file, owned with RAII. Used only for
// immutable published epoch segments; mapping a path that can be written
// concurrently is not allowed. The mapping is unmapped on destruction. The
// class is move-only so the mapping has a single owner.

namespace opaquedb::storage {

class MappedFile {
public:
  // Maps the whole file read-only. Fails if the file is empty (an empty mapping
  // is not useful and mmap rejects a zero length) or cannot be mapped.
  static absl::StatusOr<MappedFile> Open(const std::string &path);

  MappedFile() = default;
  ~MappedFile();

  MappedFile(MappedFile &&other) noexcept;
  MappedFile &operator=(MappedFile &&other) noexcept;
  MappedFile(const MappedFile &) = delete;
  MappedFile &operator=(const MappedFile &) = delete;

  std::span<const std::uint8_t> bytes() const { return {data_, size_}; }
  std::size_t size() const { return size_; }

private:
  MappedFile(const std::uint8_t *data, std::size_t size)
      : data_(data), size_(size) {}

  void Reset() noexcept;

  const std::uint8_t *data_ = nullptr;
  std::size_t size_ = 0;
};

} // namespace opaquedb::storage

#endif // OPAQUEDB_STORAGE_MAPPED_FILE_H_
