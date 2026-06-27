#ifndef OPAQUEDB_SERVER_REQUEST_GATE_H_
#define OPAQUEDB_SERVER_REQUEST_GATE_H_

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "absl/status/statusor.h"
#include "opaquedb/auth/authenticator.h"
#include "opaquedb/server/rate_limiter.h"

// The request gate is the interceptor chain applied to the query and admin
// services, in one place so both enforce the same policy. It runs, in order:
//   1. authentication and role authorization (the Authenticator strategy)
//   2. rate limiting, per authenticated principal
// Input validation (wire version, ciphertext and key checks) runs downstream in
// the service and engine, which is where the typed messages are available.
//
// Unauthenticated clients cannot pass the gate. This is how queries require
// authentication by default.
//
// Rate limiting is per principal: each principal gets its own token bucket, so
// one principal flooding the node cannot starve the others. A global bucket
// sized well above one principal's bounds total admission as a backstop. The
// authenticator is hot-swappable (SetAuthenticator) so a token-file reload on
// SIGHUP takes effect without a restart or dropping in-flight requests.

namespace opaquedb::server {

class RequestGate {
public:
  explicit RequestGate(std::shared_ptr<const auth::Authenticator> authenticator,
                       std::uint32_t tokens_per_second = 2000,
                       std::uint32_t burst = 2000);

  // Authenticates, checks the role, then applies the per-principal rate limit.
  // Returns the principal on success.
  absl::StatusOr<auth::Principal> Check(const auth::AuthInputs &inputs,
                                        auth::Role required);

  // Atomically replaces the authenticator. Used to apply a reloaded token file
  // without a restart. In-flight Check calls finish against the old strategy;
  // subsequent calls see the new one.
  void
  SetAuthenticator(std::shared_ptr<const auth::Authenticator> authenticator);

private:
  std::shared_ptr<const auth::Authenticator> CurrentAuthenticator() const;

  // Admits one request for the given principal, refilling its bucket and the
  // global backstop bucket.
  bool Admit(const std::string &principal_id);

  mutable std::mutex auth_mu_;
  std::shared_ptr<const auth::Authenticator> authenticator_;

  const double rate_;
  const double burst_;
  std::mutex limiters_mu_;
  // Node-based map: RateLimiter holds a mutex (not movable), so entries are
  // constructed in place and never relocated by rehashing.
  std::unordered_map<std::string, RateLimiter> limiters_;
  RateLimiter global_;
};

} // namespace opaquedb::server

#endif // OPAQUEDB_SERVER_REQUEST_GATE_H_
