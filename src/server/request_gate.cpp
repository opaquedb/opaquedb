#include "opaquedb/server/request_gate.h"

#include <utility>

#include "spdlog/spdlog.h"

namespace opaquedb::server {
namespace {

// The global backstop admits well above any single principal's rate, so it
// bounds total load without throttling a normal multi-client workload.
constexpr double kGlobalBurstMultiple = 64.0;

} // namespace

RequestGate::RequestGate(
    std::shared_ptr<const auth::Authenticator> authenticator,
    std::uint32_t tokens_per_second, std::uint32_t burst)
    : authenticator_(std::move(authenticator)),
      rate_(static_cast<double>(tokens_per_second)),
      burst_(static_cast<double>(burst)),
      global_(rate_ * kGlobalBurstMultiple, burst_ * kGlobalBurstMultiple) {}

void RequestGate::SetAuthenticator(
    std::shared_ptr<const auth::Authenticator> authenticator) {
  std::lock_guard<std::mutex> lock(auth_mu_);
  authenticator_ = std::move(authenticator);
}

std::shared_ptr<const auth::Authenticator>
RequestGate::CurrentAuthenticator() const {
  std::lock_guard<std::mutex> lock(auth_mu_);
  return authenticator_;
}

bool RequestGate::Admit(const std::string &principal_id) {
  std::lock_guard<std::mutex> lock(limiters_mu_);
  // try_emplace constructs the per-principal bucket in place on first sight; a
  // node-based map keeps it stable across later inserts.
  auto [it, inserted] = limiters_.try_emplace(principal_id, rate_, burst_);
  // The per-principal bucket first, then the global backstop. Both must admit.
  if (!it->second.TryAcquire())
    return false;
  return global_.TryAcquire();
}

absl::StatusOr<auth::Principal>
RequestGate::Check(const auth::AuthInputs &inputs, auth::Role required) {
  std::shared_ptr<const auth::Authenticator> authenticator =
      CurrentAuthenticator();
  absl::StatusOr<auth::Principal> principal =
      auth::Authorize(*authenticator, inputs, required);
  if (!principal.ok()) {
    // The status message names the reason (no token, token not recognized, role
    // mismatch); it never contains the token itself, so this is safe to log.
    spdlog::warn("audit: auth rejected: {}", principal.status().message());
    return principal.status();
  }
  if (!Admit(principal->id)) {
    spdlog::warn("audit: rate limit exceeded for principal {}", principal->id);
    return absl::ResourceExhaustedError("rate limit exceeded");
  }
  return principal;
}

} // namespace opaquedb::server
