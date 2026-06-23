#ifndef OPAQUEDB_AUTH_AUTHENTICATORS_H_
#define OPAQUEDB_AUTH_AUTHENTICATORS_H_

#include <memory>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "opaquedb/auth/authenticator.h"
#include "opaquedb/config/config.h"

// The three authenticator strategies and a factory that builds one from config.

namespace opaquedb::auth {

// Bearer tokens compared in constant time. Tokens are high-entropy random
// strings held in memory, loaded from a file that is readable only by the
// service user. The presented token is compared against each known token in
// time that does not depend on the content, so a near-miss does not leak
// through timing. (Hashing tokens at rest with libsodium is an option the spec
// allows but is not used here, to keep the dependency set free of a crypto
// library that conflicts with the gRPC TLS stack in one binary.)
class TokenAuthenticator : public Authenticator {
public:
  struct Entry {
    std::string id;
    Role role;
    std::string token;
  };

  // Builds from in-memory entries (used by tests).
  static absl::StatusOr<std::unique_ptr<TokenAuthenticator>>
  FromEntries(const std::vector<Entry> &entries);

  // Loads a token file. Each non-empty, non-comment line is "id role token".
  static absl::StatusOr<std::unique_ptr<TokenAuthenticator>>
  FromFile(const std::string &path);

  absl::StatusOr<Principal>
  Authenticate(const AuthInputs &inputs) const override;
  std::string name() const override { return "token"; }

private:
  struct Known {
    std::string token;
    Principal principal;
  };
  explicit TokenAuthenticator(std::vector<Known> tokens)
      : tokens_(std::move(tokens)) {}

  std::vector<Known> tokens_;
};

// Client-certificate identity. The verified peer identity becomes the
// principal.
class MtlsAuthenticator : public Authenticator {
public:
  absl::StatusOr<Principal>
  Authenticate(const AuthInputs &inputs) const override;
  std::string name() const override { return "mtls"; }
};

// The open mode. It grants an anonymous Query principal. It is only built when
// the config explicitly enables the insecure flag; the node logs a warning.
class NoAuthenticator : public Authenticator {
public:
  absl::StatusOr<Principal>
  Authenticate(const AuthInputs &inputs) const override;
  std::string name() const override { return "none"; }
};

// Builds the authenticator named by the auth config. Fails for none mode unless
// the insecure flag is set.
absl::StatusOr<std::unique_ptr<Authenticator>>
MakeAuthenticator(const config::AuthConfig &cfg);

} // namespace opaquedb::auth

#endif // OPAQUEDB_AUTH_AUTHENTICATORS_H_
