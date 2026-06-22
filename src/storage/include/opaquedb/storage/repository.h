#ifndef OPAQUEDB_STORAGE_REPOSITORY_H_
#define OPAQUEDB_STORAGE_REPOSITORY_H_

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "opaquedb/storage/epoch_manifest.h"
#include "opaquedb/storage/segment.h"
#include "opaquedb/storage/staging.h"

// The repository hides files and mmap behind an interface. Higher layers ask
// for epochs and read records; they never touch paths or mappings. This is the
// seam a future blob-backed or remote store plugs into.

namespace opaquedb::storage {

// A read view over one published epoch: its manifest and its two segments,
// already mapped and validated against the manifest.
class EpochReader {
public:
  EpochReader(EpochManifest manifest,
              std::unique_ptr<SegmentReader> match_segment,
              std::unique_ptr<SegmentReader> payload_segment)
      : manifest_(std::move(manifest)),
        match_segment_(std::move(match_segment)),
        payload_segment_(std::move(payload_segment)) {}

  const EpochManifest &manifest() const { return manifest_; }
  std::uint64_t row_count() const { return match_segment_->record_count(); }

  absl::StatusOr<std::span<const std::uint8_t>>
  MatchRecord(std::uint64_t index) const {
    return match_segment_->Record(index);
  }
  absl::StatusOr<std::span<const std::uint8_t>>
  PayloadRecord(std::uint64_t index) const {
    return payload_segment_->Record(index);
  }

private:
  EpochManifest manifest_;
  std::unique_ptr<SegmentReader> match_segment_;
  std::unique_ptr<SegmentReader> payload_segment_;
};

// The epoch repository contract. Writes never mutate a published epoch; a
// publish builds new immutable files and atomically swaps the CURRENT pointer.
class EpochRepository {
public:
  virtual ~EpochRepository() = default;

  // Publishes a staging epoch under the given version. The version must not
  // already exist, since published epochs are immutable. On success CURRENT
  // points at it.
  virtual absl::Status Publish(const StagingEpoch &staging,
                               std::uint64_t epoch_version) = 0;

  // The version CURRENT points at, or NotFound if nothing is published yet.
  virtual absl::StatusOr<std::uint64_t> CurrentVersion() const = 0;

  // Every published version, ascending.
  virtual absl::StatusOr<std::vector<std::uint64_t>> ListEpochs() const = 0;

  virtual absl::StatusOr<std::unique_ptr<EpochReader>> OpenCurrent() const = 0;
  virtual absl::StatusOr<std::unique_ptr<EpochReader>>
  OpenVersion(std::uint64_t version) const = 0;

  // Points CURRENT back at an already-published version, which must exist and
  // parse. This does not delete newer epochs.
  virtual absl::Status Rollback(std::uint64_t version) = 0;
};

// A repository backed by a local directory tree:
//   <root>/epochs/<version>/{match.seg,payload.seg,manifest.json}
//   <root>/CURRENT   text file holding the current version
class LocalEpochRepository : public EpochRepository {
public:
  // Opens (creating the directory tree if needed) a repository rooted at dir.
  static absl::StatusOr<std::unique_ptr<LocalEpochRepository>>
  Open(std::string root);

  absl::Status Publish(const StagingEpoch &staging,
                       std::uint64_t epoch_version) override;
  absl::StatusOr<std::uint64_t> CurrentVersion() const override;
  absl::StatusOr<std::vector<std::uint64_t>> ListEpochs() const override;
  absl::StatusOr<std::unique_ptr<EpochReader>> OpenCurrent() const override;
  absl::StatusOr<std::unique_ptr<EpochReader>>
  OpenVersion(std::uint64_t version) const override;
  absl::Status Rollback(std::uint64_t version) override;

private:
  explicit LocalEpochRepository(std::string root) : root_(std::move(root)) {}

  std::string EpochsDir() const;
  std::string EpochDir(std::uint64_t version) const;
  std::string CurrentPath() const;
  absl::Status SetCurrent(std::uint64_t version);

  std::string root_;
};

} // namespace opaquedb::storage

#endif // OPAQUEDB_STORAGE_REPOSITORY_H_
