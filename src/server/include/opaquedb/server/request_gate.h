#ifndef OPAQUEDB_SERVER_REQUEST_GATE_H_
#define OPAQUEDB_SERVER_REQUEST_GATE_H_

#include "absl/status/statusor.h"
#include "opaquedb/auth/authenticator.h"
#include "opaquedb/server/rate_limiter.h"

// The request gate is the interceptor chain applied to the query and admin
// services, in one place so both enforce the same policy. It runs, in order:
//   1. authentication and role authorization (the Authenticator strategy)
//   2. rate limiting
// Input validation (wire version, ciphertext and key checks) runs downstream in
// the service and engine, which is where the typed messages are available.
//
// Unauthenticated clients cannot pass the gate. This is how queries require
// authentication by default.

namespace opaquedb::server {

class RequestGate {
public:
  RequestGate(const auth::Authenticator *authenticator,
              double tokens_per_second = 2000.0, double burst = 2000.0)
      : authenticator_(authenticator), limiter_(tokens_per_second, burst) {}

  // Authenticates, checks the role, then applies the rate limit. Returns the
  // principal on success.
  absl::StatusOr<auth::Principal> Check(const auth::AuthInputs &inputs,
                                        auth::Role required);

private:
  const auth::Authenticator *authenticator_;
  RateLimiter limiter_;
};

} // namespace opaquedb::server

#endif // OPAQUEDB_SERVER_REQUEST_GATE_H_
