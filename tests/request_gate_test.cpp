#include "opaquedb/server/request_gate.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "opaquedb/auth/authenticator.h"
#include "opaquedb/auth/authenticators.h"

namespace {

using opaquedb::auth::Authenticator;
using opaquedb::auth::AuthInputs;
using opaquedb::auth::Role;
using opaquedb::auth::TokenAuthenticator;
using opaquedb::server::RequestGate;

std::shared_ptr<const Authenticator> TwoPrincipals() {
  auto authn = TokenAuthenticator::FromEntries(
      {{"alice", Role::kQuery, "tok-alice"}, {"bob", Role::kQuery, "tok-bob"}});
  EXPECT_TRUE(authn.ok());
  return std::shared_ptr<const Authenticator>(*std::move(authn));
}

AuthInputs Token(const std::string &t) {
  AuthInputs in;
  in.bearer_token = t;
  return in;
}

// A burst of 1 with a tiny refill: the first request per principal passes, the
// immediate second is throttled. The key property is that one principal hitting
// its limit does not consume another principal's budget.
TEST(RequestGate, RateLimitIsPerPrincipal) {
  RequestGate gate(TwoPrincipals(), /*tokens_per_second=*/1, /*burst=*/1);

  EXPECT_TRUE(gate.Check(Token("tok-alice"), Role::kQuery).ok());
  // alice's bucket is now empty: her next immediate request is throttled.
  EXPECT_EQ(gate.Check(Token("tok-alice"), Role::kQuery).status().code(),
            absl::StatusCode::kResourceExhausted);
  // bob has his own bucket, untouched by alice's flood.
  EXPECT_TRUE(gate.Check(Token("tok-bob"), Role::kQuery).ok());
}

TEST(RequestGate, RejectsUnauthenticatedBeforeRateLimiting) {
  RequestGate gate(TwoPrincipals(), 1, 1);
  EXPECT_EQ(gate.Check(Token("nope"), Role::kQuery).status().code(),
            absl::StatusCode::kUnauthenticated);
}

// A reloaded authenticator takes effect immediately; the old token stops
// working and the new one starts.
TEST(RequestGate, SetAuthenticatorSwapsCredentials) {
  RequestGate gate(TwoPrincipals(), 1000, 1000);
  EXPECT_TRUE(gate.Check(Token("tok-alice"), Role::kQuery).ok());

  auto rotated =
      TokenAuthenticator::FromEntries({{"carol", Role::kQuery, "tok-carol"}});
  ASSERT_TRUE(rotated.ok());
  gate.SetAuthenticator(
      std::shared_ptr<const Authenticator>(*std::move(rotated)));

  EXPECT_FALSE(gate.Check(Token("tok-alice"), Role::kQuery).ok());
  EXPECT_TRUE(gate.Check(Token("tok-carol"), Role::kQuery).ok());
}

} // namespace
