#ifndef OPAQUEDB_STORAGE_IO_UTIL_H_
#define OPAQUEDB_STORAGE_IO_UTIL_H_

#include <cstdint>
#include <span>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

// Durable filesystem primitives. Storage gets its durability from fsync and its
// atomicity from rename, with no transaction manager. These wrap the raw
// syscalls with EINTR retries and turn failures into a status.

namespace opaquedb::storage {

// Writes data to path (truncating any existing file), then fsyncs the file. The
// file content is durable when this returns ok. It does not fsync the
// directory; the caller does that once after creating all files it cares about.
absl::Status WriteFileDurably(const std::string &path,
                              std::span<const std::uint8_t> data);

// Opens dir and fsyncs it so that earlier creates, renames, or unlinks within
// it are durable.
absl::Status FsyncDir(const std::string &dir);

// Renames from to to, then fsyncs the parent directory so the rename is
// durable. rename is atomic on the same filesystem, which is how a publish
// swaps in a new CURRENT pointer or a finished epoch directory.
absl::Status RenameDurably(const std::string &from, const std::string &to);

// Reads a whole small file (a manifest or the CURRENT pointer) into a string.
// Caps the size so a wrong path to a huge file cannot exhaust memory.
absl::StatusOr<std::string> ReadSmallFile(const std::string &path,
                                          std::size_t max_bytes);

} // namespace opaquedb::storage

#endif // OPAQUEDB_STORAGE_IO_UTIL_H_
