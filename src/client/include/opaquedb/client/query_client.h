#ifndef OPAQUEDB_CLIENT_QUERY_CLIENT_H_
#define OPAQUEDB_CLIENT_QUERY_CLIENT_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "grpcpp/grpcpp.h"
#include "opaquedb.grpc.pb.h"
#include "opaquedb/config/config.h"
#include "opaquedb/core/schema.h"
#include "opaquedb/crypto/context.h"
#include "opaquedb/crypto/key_material.h"

// Reusable client for the OpaqueDB wire protocol. It owns the keyring and
// performs the constant-weight encoding, encryption of the query parameter,
// Register streaming, Execute, and result decryption.
//
// This eliminates duplication between the dev client, examples, and any
// future real application clients.

namespace opaquedb::client {

class QueryClient {
public:
  // Create a client connected to the given target. The config supplies the
  // FHE parameters (cw length/weight, etc). bearer_token is optional for token
  // auth.
  static absl::StatusOr<std::unique_ptr<QueryClient>>
  Create(const config::Config &cfg, const std::string &target,
         const std::string &bearer_token = "");

  // Upload this client's public/eval keys.
  absl::Status Register(const std::string &client_id);

  // Fetch a table's schema from the node, to decode rows and project the SELECT
  // columns. database empty means "default".
  absl::StatusOr<core::Schema> DescribeTable(const std::string &database,
                                             const std::string &table);

  // Execute a private query. Returns the decrypted record bytes for matches
  // (usually 0 or 1 row today). The table comes from the SQL; database selects
  // which database holds it (default "default"). value is the bound match value
  // when the SQL uses a parameter; if the SQL carries an inline literal, value
  // is ignored.
  absl::StatusOr<std::vector<std::vector<std::uint8_t>>>
  Query(const std::string &client_id, const std::string &sql_template,
        std::uint64_t value, const std::string &backend_hint = "",
        const std::string &database = "default");

  // The outcome of an insert: the new epoch version and total row count.
  struct InsertResult {
    std::uint64_t epoch_version = 0;
    std::uint64_t row_count = 0;
  };

  // Append one row to a table. values are the column values as text in the
  // table's declared (CREATE TABLE) column order, including the match key. The
  // table must already exist. This is a plaintext insert; the values travel over
  // the (optionally TLS) channel but are stored in the clear, matching how data
  // is stored at rest today. A future encrypted-payload insert encodes/encrypts
  // the values on the client and uses this same call.
  absl::StatusOr<InsertResult> Insert(const std::string &database,
                                      const std::string &table,
                                      const std::vector<std::string> &values);

  const crypto::CryptoContext &crypto_context() const { return ctx_; }

private:
  QueryClient(config::Config cfg, crypto::CryptoContext ctx,
              crypto::ClientKeyring keyring,
              std::unique_ptr<opaquedb::proto::OpaqueDB::Stub> stub,
              std::string bearer_token);

  void Authorize(grpc::ClientContext &context) const;

  config::Config cfg_;
  crypto::CryptoContext ctx_;
  crypto::ClientKeyring keyring_;
  std::unique_ptr<opaquedb::proto::OpaqueDB::Stub> stub_;
  std::string bearer_token_;
};

} // namespace opaquedb::client

#endif // OPAQUEDB_CLIENT_QUERY_CLIENT_H_
