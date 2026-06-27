#include "token_command.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "absl/strings/str_cat.h"
#include "spdlog/spdlog.h"

namespace opaquedb::cli {
namespace {

// Reads n bytes from the system CSPRNG. /dev/urandom is the right source for
// long-lived secrets and avoids pulling in a crypto library (which conflicts
// with the SEAL + gRPC OpenSSL build, see CLAUDE.md).
absl::StatusOr<std::string> RandomHex(std::uint32_t n) {
  std::ifstream urandom("/dev/urandom", std::ios::binary);
  if (!urandom)
    return absl::UnavailableError("cannot open /dev/urandom");
  std::vector<unsigned char> raw(n);
  urandom.read(reinterpret_cast<char *>(raw.data()),
               static_cast<std::streamsize>(n));
  if (!urandom)
    return absl::UnavailableError("short read from /dev/urandom");
  static constexpr std::array<char, 16> kHex = {'0', '1', '2', '3', '4', '5',
                                                '6', '7', '8', '9', 'a', 'b',
                                                'c', 'd', 'e', 'f'};
  std::string hex;
  hex.reserve(static_cast<std::size_t>(n) * 2);
  for (unsigned char b : raw) {
    hex.push_back(kHex[b >> 4]);
    hex.push_back(kHex[b & 0x0f]);
  }
  return hex;
}

} // namespace

void TokenCommand::Register(CLI::App &parent, const GlobalOptions & /*globals*/,
                            int &exit_code) {
  CLI::App *token =
      parent.add_subcommand("token", "Provision auth tokens for token mode");
  CLI::App *mint =
      token->add_subcommand("mint", "Generate a high-entropy bearer token");
  mint->add_option("--id", id_, "Principal id for the token")->required();
  mint->add_option("--role", role_, "Role: query or admin")
      ->default_val("query");
  mint->add_option("--bytes", bytes_, "Token entropy in bytes")
      ->default_val(32);
  mint->callback([this, &exit_code]() {
    if (role_ != "query" && role_ != "admin") {
      spdlog::error("token: --role must be query or admin, got '{}'", role_);
      exit_code = 1;
      return;
    }
    if (bytes_ < 16) {
      spdlog::error("token: --bytes must be at least 16 for adequate entropy");
      exit_code = 1;
      return;
    }
    absl::StatusOr<std::string> tok = RandomHex(bytes_);
    if (!tok.ok()) {
      spdlog::error("token: {}", tok.status().message());
      exit_code = 1;
      return;
    }
    // Data, not a log line: print the token-file line to stdout so it can be
    // appended to the token file (chmod 0640, owned by the service user).
    std::cout << id_ << " " << role_ << " " << *tok << "\n";
  });
}

} // namespace opaquedb::cli
