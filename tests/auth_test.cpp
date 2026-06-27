#include "opaquedb/auth/authenticators.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <vector>

#include "opaquedb/auth/authenticator.h"
#include "opaquedb/config/config.h"

namespace {

using opaquedb::auth::AuthInputs;
using opaquedb::auth::Authorize;
using opaquedb::auth::MakeAuthenticator;
using opaquedb::auth::MtlsAuthenticator;
using opaquedb::auth::NoAuthenticator;
using opaquedb::auth::Principal;
using opaquedb::auth::Role;
using opaquedb::auth::TokenAuthenticator;

AuthInputs WithToken(const std::string &token) {
  AuthInputs in;
  in.bearer_token = token;
  return in;
}

TEST(TokenAuth, AcceptsKnownTokenAndAssignsRole) {
  auto authn =
      TokenAuthenticator::FromEntries({{"alice", Role::kQuery, "tok-alice"},
                                       {"root", Role::kAdmin, "tok-root"}});
  ASSERT_TRUE(authn.ok());

  auto alice = (*authn)->Authenticate(WithToken("tok-alice"));
  ASSERT_TRUE(alice.ok());
  EXPECT_EQ(alice->id, "alice");
  EXPECT_EQ(alice->role, Role::kQuery);

  auto root = (*authn)->Authenticate(WithToken("tok-root"));
  ASSERT_TRUE(root.ok());
  EXPECT_EQ(root->role, Role::kAdmin);
}

TEST(TokenAuth, RejectsUnknownOrMissingToken) {
  auto authn = TokenAuthenticator::FromEntries({{"a", Role::kQuery, "secret"}});
  ASSERT_TRUE(authn.ok());
  EXPECT_FALSE((*authn)->Authenticate(WithToken("wrong")).ok());
  EXPECT_FALSE((*authn)->Authenticate(AuthInputs{}).ok()); // no token
}

TEST(TokenAuth, LoadsFromFile) {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() / "opaquedb_tokens_test";
  {
    std::ofstream out(path);
    out << "# a comment\n";
    out << "alice query tok-alice\n";
    out << "\n";
    out << "root admin tok-root\n";
  }
  auto authn = TokenAuthenticator::FromFile(path.string());
  ASSERT_TRUE(authn.ok()) << authn.status().message();
  EXPECT_TRUE((*authn)->Authenticate(WithToken("tok-alice")).ok());
  auto root = (*authn)->Authenticate(WithToken("tok-root"));
  ASSERT_TRUE(root.ok());
  EXPECT_EQ(root->role, Role::kAdmin);
  std::filesystem::remove(path);
}

TEST(Authorize, AdminRequirementNeedsAdminRole) {
  auto authn = TokenAuthenticator::FromEntries(
      {{"q", Role::kQuery, "q"}, {"a", Role::kAdmin, "a"}});
  ASSERT_TRUE(authn.ok());

  // A Query principal may run queries but not admin operations.
  EXPECT_TRUE(Authorize(**authn, WithToken("q"), Role::kQuery).ok());
  EXPECT_FALSE(Authorize(**authn, WithToken("q"), Role::kAdmin).ok());

  // An Admin principal satisfies both.
  EXPECT_TRUE(Authorize(**authn, WithToken("a"), Role::kQuery).ok());
  EXPECT_TRUE(Authorize(**authn, WithToken("a"), Role::kAdmin).ok());
}

TEST(MtlsAuth, UsesPeerIdentity) {
  MtlsAuthenticator authn;
  EXPECT_FALSE(authn.Authenticate(AuthInputs{}).ok()); // no client cert

  AuthInputs in;
  in.peer_identity = "spiffe://node-1";
  auto principal = authn.Authenticate(in);
  ASSERT_TRUE(principal.ok());
  EXPECT_EQ(principal->id, "spiffe://node-1");
}

TEST(MtlsAuth, MapsListedIdentityToAdmin) {
  MtlsAuthenticator authn({"spiffe://admin", "cn=ops"});

  AuthInputs admin_in;
  admin_in.peer_identity = "spiffe://admin";
  auto admin = authn.Authenticate(admin_in);
  ASSERT_TRUE(admin.ok());
  EXPECT_EQ(admin->role, Role::kAdmin);

  // An identity that is not listed is a Query principal.
  AuthInputs query_in;
  query_in.peer_identity = "spiffe://someone-else";
  auto query = authn.Authenticate(query_in);
  ASSERT_TRUE(query.ok());
  EXPECT_EQ(query->role, Role::kQuery);
}

TEST(NoAuth, GrantsAnonymousQuery) {
  NoAuthenticator authn;
  auto principal = authn.Authenticate(AuthInputs{});
  ASSERT_TRUE(principal.ok());
  EXPECT_EQ(principal->role, Role::kQuery);
}

TEST(Factory, NoneModeNeedsInsecureFlag) {
  opaquedb::config::AuthConfig cfg;
  cfg.mode = opaquedb::config::AuthMode::kNone;
  cfg.enable_insecure = false;
  EXPECT_FALSE(MakeAuthenticator(cfg).ok());
  cfg.enable_insecure = true;
  EXPECT_TRUE(MakeAuthenticator(cfg).ok());
}

TEST(Factory, MtlsModeBuilds) {
  opaquedb::config::AuthConfig cfg;
  cfg.mode = opaquedb::config::AuthMode::kMtls;
  EXPECT_TRUE(MakeAuthenticator(cfg).ok());
}

} // namespace
