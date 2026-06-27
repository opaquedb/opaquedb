#include "opaquedb/auth/authenticators.h"

#include <cstddef>
#include <fstream>
#include <string>
#include <vector>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "absl/strings/strip.h"

namespace opaquedb::auth {
namespace {

// Compares two strings in time that depends only on their lengths, not their
// contents, so a partial match cannot be found by timing. volatile keeps the
// compiler from short-circuiting the accumulation.
bool ConstantTimeEquals(const std::string &a, const std::string &b) {
  if (a.size() != b.size())
    return false;
  volatile unsigned char diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    diff =
        static_cast<unsigned char>(diff | (static_cast<unsigned char>(a[i]) ^
                                           static_cast<unsigned char>(b[i])));
  }
  return diff == 0;
}

} // namespace

absl::StatusOr<std::unique_ptr<TokenAuthenticator>>
TokenAuthenticator::FromEntries(const std::vector<Entry> &entries) {
  std::vector<Known> tokens;
  tokens.reserve(entries.size());
  for (const Entry &entry : entries) {
    if (entry.id.empty() || entry.token.empty()) {
      return absl::InvalidArgumentError("token entry has empty id or token");
    }
    tokens.push_back(Known{entry.token, Principal{entry.id, entry.role}});
  }
  return std::unique_ptr<TokenAuthenticator>(
      new TokenAuthenticator(std::move(tokens)));
}

absl::StatusOr<std::unique_ptr<TokenAuthenticator>>
TokenAuthenticator::FromFile(const std::string &path) {
  std::ifstream in(path);
  if (!in) {
    return absl::NotFoundError(
        absl::StrCat("cannot open token file '", path, "'"));
  }
  std::vector<Entry> entries;
  std::string line;
  std::size_t line_no = 0;
  while (std::getline(in, line)) {
    ++line_no;
    std::string_view trimmed = absl::StripAsciiWhitespace(line);
    if (trimmed.empty() || trimmed.front() == '#')
      continue;
    std::vector<std::string> fields =
        absl::StrSplit(trimmed, absl::ByAnyChar(" \t"), absl::SkipEmpty());
    if (fields.size() != 3) {
      return absl::InvalidArgumentError(absl::StrCat(
          "token file line ", line_no, ": expected 'id role token'"));
    }
    absl::StatusOr<Role> role = ParseRole(fields[1]);
    if (!role.ok()) {
      return absl::InvalidArgumentError(absl::StrCat(
          "token file line ", line_no, ": ", role.status().message()));
    }
    entries.push_back(Entry{fields[0], *role, fields[2]});
  }
  return FromEntries(entries);
}

absl::StatusOr<Principal>
TokenAuthenticator::Authenticate(const AuthInputs &inputs) const {
  if (!inputs.bearer_token || inputs.bearer_token->empty()) {
    return absl::UnauthenticatedError("no bearer token presented");
  }
  // Compare against every known token without stopping at the first match, so
  // neither the result nor its position leaks through timing.
  const Principal *found = nullptr;
  for (const Known &entry : tokens_) {
    if (ConstantTimeEquals(*inputs.bearer_token, entry.token)) {
      found = &entry.principal;
    }
  }
  if (found == nullptr) {
    return absl::UnauthenticatedError("token not recognized");
  }
  return *found;
}

absl::StatusOr<Principal>
MtlsAuthenticator::Authenticate(const AuthInputs &inputs) const {
  if (!inputs.peer_identity || inputs.peer_identity->empty()) {
    return absl::UnauthenticatedError(
        "no verified client certificate identity");
  }
  // A verified identity listed as admin gets the Admin role; everyone else is a
  // Query principal. The identity is the cert subject or SAN the TLS layer
  // verified, so this is access control over an already-authenticated caller.
  for (const std::string &admin : admin_identities_) {
    if (admin == *inputs.peer_identity) {
      return Principal{*inputs.peer_identity, Role::kAdmin};
    }
  }
  return Principal{*inputs.peer_identity, Role::kQuery};
}

absl::StatusOr<Principal>
NoAuthenticator::Authenticate(const AuthInputs & /*inputs*/) const {
  return Principal{"anonymous", Role::kQuery};
}

absl::StatusOr<std::unique_ptr<Authenticator>>
MakeAuthenticator(const config::AuthConfig &cfg) {
  switch (cfg.mode) {
  case config::AuthMode::kToken: {
    absl::StatusOr<std::unique_ptr<TokenAuthenticator>> token =
        TokenAuthenticator::FromFile(cfg.token_file);
    if (!token.ok())
      return token.status();
    return std::unique_ptr<Authenticator>(*std::move(token));
  }
  case config::AuthMode::kMtls:
    return std::unique_ptr<Authenticator>(
        std::make_unique<MtlsAuthenticator>(cfg.admin_identities));
  case config::AuthMode::kNone:
    if (!cfg.enable_insecure) {
      return absl::FailedPreconditionError(
          "auth mode none requires enable_insecure = true");
    }
    return std::unique_ptr<Authenticator>(std::make_unique<NoAuthenticator>());
  }
  return absl::InvalidArgumentError("unknown auth mode");
}

} // namespace opaquedb::auth
