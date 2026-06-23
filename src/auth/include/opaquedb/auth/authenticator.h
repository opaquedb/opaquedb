#ifndef OPAQUEDB_AUTH_AUTHENTICATOR_H_
#define OPAQUEDB_AUTH_AUTHENTICATOR_H_

#include <optional>
#include <string>

#include "absl/status/statusor.h"

// Authentication is access control: who may submit queries and who may manage
// the system. It does not weaken query privacy. The operator may learn which
// authenticated principal sent a query, that is metadata, but still never sees
// the query value.
//
// The Authenticator is a strategy chosen by config. Its inputs are the small
// set of facts a strategy needs, extracted from the transport at the edge, so
// this library does not depend on gRPC and is unit-testable.

namespace opaquedb::auth {

enum class Role { kQuery, kAdmin };

std::string ToString(Role role);
absl::StatusOr<Role> ParseRole(std::string_view text);

struct Principal {
  std::string id;
  Role role = Role::kQuery;
};

// What the transport edge hands an authenticator.
struct AuthInputs {
  // The bearer token from the request, if any (token mode).
  std::optional<std::string> bearer_token;
  // The peer's verified mTLS identity, if any (mtls mode).
  std::optional<std::string> peer_identity;
};

class Authenticator {
public:
  virtual ~Authenticator() = default;

  // Returns the authenticated principal, or an Unauthenticated status. Never
  // returns a principal for an unverified caller.
  virtual absl::StatusOr<Principal>
  Authenticate(const AuthInputs &inputs) const = 0;

  virtual std::string name() const = 0;
};

// Authenticates and then checks the principal holds at least the required role.
// An Admin principal satisfies a Query requirement; a Query principal does not
// satisfy an Admin requirement.
absl::StatusOr<Principal> Authorize(const Authenticator &authenticator,
                                    const AuthInputs &inputs, Role required);

} // namespace opaquedb::auth

#endif // OPAQUEDB_AUTH_AUTHENTICATOR_H_
