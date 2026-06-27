#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/strip.h"
#include "io_util.h"
#include "opaquedb/storage/repository.h"

namespace opaquedb::storage {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kMaxManifestBytes = 1u << 20; // 1 MiB is ample
constexpr char kMatchFile[] = "match.seg";
constexpr char kPayloadFile[] = "payload.seg";

absl::Status CreateDir(const std::string &path) {
  std::error_code ec;
  fs::create_directories(path, ec);
  if (ec) {
    return absl::InternalError(
        absl::StrCat("cannot create '", path, "': ", ec.message()));
  }
  return absl::OkStatus();
}

// Parses a directory name as a decimal epoch version. Returns false for names
// that are not a bare unsigned number (tmp dirs, junk).
bool ParseVersionName(const std::string &name, std::uint64_t *out) {
  if (name.empty())
    return false;
  for (char c : name) {
    if (c < '0' || c > '9')
      return false;
  }
  return absl::SimpleAtoi(name, out);
}

} // namespace

absl::StatusOr<std::unique_ptr<LocalEpochRepository>>
LocalEpochRepository::Open(std::string root) {
  if (root.empty()) {
    return absl::InvalidArgumentError("repository root is empty");
  }
  auto repo = std::unique_ptr<LocalEpochRepository>(
      new LocalEpochRepository(std::move(root)));
  if (absl::Status s = CreateDir(repo->root_); !s.ok())
    return s;
  return repo;
}

// Epoch version directories and the CURRENT pointer live directly under the
// root, so the configured epoch directory maps straight to it.
std::string LocalEpochRepository::EpochsDir() const { return root_; }

std::string LocalEpochRepository::EpochDir(std::uint64_t version) const {
  return (fs::path(EpochsDir()) / std::to_string(version)).string();
}

std::string LocalEpochRepository::CurrentPath() const {
  return (fs::path(root_) / "CURRENT").string();
}

absl::Status LocalEpochRepository::SetCurrent(std::uint64_t version) {
  const std::string text = std::to_string(version);
  const std::string tmp = (fs::path(root_) / "CURRENT.tmp").string();
  const std::vector<std::uint8_t> bytes(text.begin(), text.end());
  if (absl::Status s = WriteFileDurably(tmp, bytes); !s.ok())
    return s;
  return RenameDurably(tmp, CurrentPath());
}

absl::Status LocalEpochRepository::Publish(const StagingEpoch &staging,
                                           std::uint64_t epoch_version) {
  if (absl::Status s = staging.Validate(); !s.ok())
    return s;

  std::error_code ec;
  if (fs::exists(EpochDir(epoch_version), ec)) {
    return absl::AlreadyExistsError(absl::StrCat(
        "epoch ", epoch_version, " is already published and is immutable"));
  }

  // Split the rows into the two column segments.
  std::vector<std::vector<std::uint8_t>> match_records;
  std::vector<std::vector<std::uint8_t>> payload_records;
  match_records.reserve(staging.rows().size());
  payload_records.reserve(staging.rows().size());
  for (const Row &row : staging.rows()) {
    match_records.push_back(row.match);
    payload_records.push_back(row.payload);
  }

  // Build into a temp directory, then swap it in with one atomic rename.
  const std::string tmp =
      (fs::path(EpochsDir()) / absl::StrCat(".tmp.", epoch_version)).string();
  fs::remove_all(tmp, ec);
  if (absl::Status s = CreateDir(tmp); !s.ok())
    return s;

  std::uint32_t match_crc = 0;
  std::uint32_t payload_crc = 0;
  if (absl::Status s =
          WriteSegment((fs::path(tmp) / kMatchFile).string(),
                       staging.match_record_bytes(), match_records, &match_crc);
      !s.ok()) {
    fs::remove_all(tmp, ec);
    return s;
  }
  if (absl::Status s = WriteSegment((fs::path(tmp) / kPayloadFile).string(),
                                    staging.geometry().record_bytes,
                                    payload_records, &payload_crc);
      !s.ok()) {
    fs::remove_all(tmp, ec);
    return s;
  }

  EpochManifest manifest;
  manifest.format_version = kManifestFormatVersion;
  manifest.epoch_version = epoch_version;
  manifest.schema = staging.schema();
  manifest.key_bits = staging.key_bits();
  manifest.geometry = staging.geometry();
  manifest.match_segment = SegmentDescriptor{
      kMatchFile, staging.match_record_bytes(),
      static_cast<std::uint64_t>(match_records.size()), match_crc};
  manifest.payload_segment = SegmentDescriptor{
      kPayloadFile, staging.geometry().record_bytes,
      static_cast<std::uint64_t>(payload_records.size()), payload_crc};
  if (absl::Status s = manifest.Validate(); !s.ok()) {
    fs::remove_all(tmp, ec);
    return s;
  }

  const std::string manifest_text = SerializeManifest(manifest);
  const std::vector<std::uint8_t> manifest_bytes(manifest_text.begin(),
                                                 manifest_text.end());
  if (absl::Status s = WriteFileDurably(
          (fs::path(tmp) / "manifest.json").string(), manifest_bytes);
      !s.ok()) {
    fs::remove_all(tmp, ec);
    return s;
  }

  if (absl::Status s = FsyncDir(tmp); !s.ok()) {
    fs::remove_all(tmp, ec);
    return s;
  }
  // Atomic publish: the whole directory appears at once.
  if (absl::Status s = RenameDurably(tmp, EpochDir(epoch_version)); !s.ok()) {
    fs::remove_all(tmp, ec);
    return s;
  }
  // Swap the CURRENT pointer to the new epoch.
  return SetCurrent(epoch_version);
}

absl::StatusOr<std::uint64_t> LocalEpochRepository::CurrentVersion() const {
  absl::StatusOr<std::string> text = ReadSmallFile(CurrentPath(), 64);
  if (!text.ok())
    return text.status();
  std::string trimmed(absl::StripAsciiWhitespace(*text));
  std::uint64_t version = 0;
  if (trimmed.empty() || !absl::SimpleAtoi(trimmed, &version)) {
    return absl::DataLossError("CURRENT does not hold a valid version");
  }
  return version;
}

absl::StatusOr<std::vector<std::uint64_t>>
LocalEpochRepository::ListEpochs() const {
  std::vector<std::uint64_t> versions;
  std::error_code ec;
  fs::directory_iterator it(EpochsDir(), ec);
  if (ec) {
    return absl::InternalError(
        absl::StrCat("cannot list epochs: ", ec.message()));
  }
  for (const fs::directory_entry &entry : it) {
    if (!entry.is_directory())
      continue;
    std::uint64_t version = 0;
    if (ParseVersionName(entry.path().filename().string(), &version)) {
      versions.push_back(version);
    }
  }
  std::sort(versions.begin(), versions.end());
  return versions;
}

absl::StatusOr<std::unique_ptr<EpochReader>>
LocalEpochRepository::OpenVersion(std::uint64_t version) const {
  const std::string dir = EpochDir(version);
  std::error_code ec;
  if (!fs::exists(dir, ec)) {
    return absl::NotFoundError(absl::StrCat("epoch ", version, " not found"));
  }

  absl::StatusOr<std::string> manifest_text = ReadSmallFile(
      (fs::path(dir) / "manifest.json").string(), kMaxManifestBytes);
  if (!manifest_text.ok())
    return manifest_text.status();
  absl::StatusOr<EpochManifest> manifest = ParseManifest(*manifest_text);
  if (!manifest.ok())
    return manifest.status();

  // Validate manifest also confirmed the file names are safe basenames.
  absl::StatusOr<std::unique_ptr<SegmentReader>> match = SegmentReader::Open(
      (fs::path(dir) / manifest->match_segment.file).string(),
      manifest->match_segment.record_bytes,
      manifest->match_segment.record_count, manifest->match_segment.crc32);
  if (!match.ok())
    return match.status();

  absl::StatusOr<std::unique_ptr<SegmentReader>> payload = SegmentReader::Open(
      (fs::path(dir) / manifest->payload_segment.file).string(),
      manifest->payload_segment.record_bytes,
      manifest->payload_segment.record_count, manifest->payload_segment.crc32);
  if (!payload.ok())
    return payload.status();

  return std::make_unique<EpochReader>(*std::move(manifest), *std::move(match),
                                       *std::move(payload));
}

absl::StatusOr<std::unique_ptr<EpochReader>>
LocalEpochRepository::OpenCurrent() const {
  absl::StatusOr<std::uint64_t> version = CurrentVersion();
  if (!version.ok())
    return version.status();
  return OpenVersion(*version);
}

absl::Status LocalEpochRepository::Rollback(std::uint64_t version) {
  // Confirm the target exists and parses before pointing CURRENT at it.
  absl::StatusOr<std::unique_ptr<EpochReader>> reader = OpenVersion(version);
  if (!reader.ok())
    return reader.status();
  return SetCurrent(version);
}

absl::Status LocalEpochRepository::Prune(std::uint64_t keep) {
  if (keep == 0)
    return absl::OkStatus(); // retention disabled: keep every epoch

  absl::StatusOr<std::vector<std::uint64_t>> versions = ListEpochs();
  if (!versions.ok())
    return versions.status();
  if (versions->size() <= keep)
    return absl::OkStatus();

  // ListEpochs returns ascending, and CURRENT is the highest after a publish,
  // so keeping the top `keep` versions always retains CURRENT and a rollback
  // window. Delete everything below the cutoff. A failed unlink is logged via
  // the returned status but leaves earlier deletes done; epochs are independent
  // directories, so a partial prune is still consistent.
  const std::size_t cutoff = versions->size() - static_cast<std::size_t>(keep);
  std::error_code ec;
  for (std::size_t i = 0; i < cutoff; ++i) {
    const std::string dir = EpochDir((*versions)[i]);
    fs::remove_all(dir, ec);
    if (ec) {
      return absl::InternalError(absl::StrCat("cannot prune epoch ",
                                              (*versions)[i], ": ",
                                              ec.message()));
    }
  }
  return absl::OkStatus();
}

} // namespace opaquedb::storage
