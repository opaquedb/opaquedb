#include "opaquedb/server/request_gate.h"

#include "spdlog/spdlog.h"

namespace opaquedb::server {

absl::StatusOr<auth::Principal>
RequestGate::Check(const auth::AuthInputs &inputs, auth::Role required) {
  absl::StatusOr<auth::Principal> principal =
      auth::Authorize(*authenticator_, inputs, required);
  if (!principal.ok()) {
    // The status message names the reason (no token, token not recognized, role
    // mismatch); it never contains the token itself, so this is safe to log.
    spdlog::warn("auth rejected: {}", principal.status().message());
    return principal.status();
  }
  if (!limiter_.TryAcquire()) {
    spdlog::warn("rate limit exceeded for principal {}", principal->id);
    return absl::ResourceExhaustedError("rate limit exceeded");
  }
  return principal;
}

} // namespace opaquedb::server
