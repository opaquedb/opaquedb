#include "opaquedb/auth/authenticator.h"

#include "absl/strings/str_cat.h"

namespace opaquedb::auth {

std::string ToString(Role role) {
  switch (role) {
  case Role::kQuery:
    return "query";
  case Role::kAdmin:
    return "admin";
  }
  return "unknown";
}

absl::StatusOr<Role> ParseRole(std::string_view text) {
  if (text == "query")
    return Role::kQuery;
  if (text == "admin")
    return Role::kAdmin;
  return absl::InvalidArgumentError(absl::StrCat("unknown role '", text, "'"));
}

absl::StatusOr<Principal> Authorize(const Authenticator &authenticator,
                                    const AuthInputs &inputs, Role required) {
  absl::StatusOr<Principal> principal = authenticator.Authenticate(inputs);
  if (!principal.ok())
    return principal.status();
  if (required == Role::kAdmin && principal->role != Role::kAdmin) {
    return absl::PermissionDeniedError(
        "this operation requires the Admin role");
  }
  return principal;
}

} // namespace opaquedb::auth
