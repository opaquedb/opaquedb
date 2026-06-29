#ifndef OPAQUEDB_CLIENT_QUERY_CLIENT_H_
#define OPAQUEDB_CLIENT_QUERY_CLIENT_H_

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "grpcpp/grpcpp.h"
#include "opaquedb.grpc.pb.h"
#include "opaquedb/config/config.h"
#include "opaquedb/core/schema.h"
#include "opaquedb/crypto/context.h"
#include "opaquedb/crypto/key_material.h"
#include "opaquedb/crypto/ops.h"
#include "opaquedb/sql/parser.h"

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

  // Create a client that reuses a keyset previously produced by SerializeKeyset
  // instead of generating a fresh one. This is how a real client registers its
  // keys once and then reuses the same identity (and the keys already held by
  // the server) on later runs without re-registering the large Galois set.
  // keyset is the secret blob from SerializeKeyset and must come from a trusted
  // local store.
  static absl::StatusOr<std::unique_ptr<QueryClient>>
  CreateWithKeyset(const config::Config &cfg, const std::string &target,
                   std::string_view keyset,
                   const std::string &bearer_token = "");

  // Upload this client's public/eval keys.
  absl::Status Register(const std::string &client_id);

  // Serialize this client's full keyset (including the secret key) so it can be
  // persisted locally and reused via CreateWithKeyset. The returned bytes are
  // the secret: store them with owner-only permissions and never send them to
  // the server.
  absl::StatusOr<std::string> SerializeKeyset() const;

  // Fetch a table's schema from the node, to decode rows and project the SELECT
  // columns. database empty means "default".
  absl::StatusOr<core::Schema> DescribeTable(const std::string &database,
                                             const std::string &table);

  // Execute a private query. Returns the decrypted record bytes for the matched
  // rows: one row by default, or up to LIMIT rows (from a window of result
  // buckets) when the SQL carries a LIMIT/OFFSET. The table comes from the SQL;
  // database selects which database holds it (default "default"). value is the
  // bound match value when the SQL uses a parameter; if the SQL carries an
  // inline literal, value is ignored. If collided_buckets is non-null it
  // receives the number of result buckets dropped because more than one row
  // with the same key hashed into them (raise crypto.result_buckets or page
  // with OFFSET to recover those rows).
  absl::StatusOr<std::vector<std::vector<std::uint8_t>>>
  Query(const std::string &client_id, const std::string &sql_template,
        std::uint64_t value, const std::string &backend_hint = "",
        const std::string &database = "default",
        std::uint32_t *collided_buckets = nullptr);

  // Every clean matched row, with no LIMIT/OFFSET window applied. The caller
  // applies DISTINCT, ORDER BY, and the row window itself (the CLI does this
  // over decoded rows). Same value / literal handling as Query.
  // collided_buckets receives the dropped same-key collision count as in Query.
  absl::StatusOr<std::vector<std::vector<std::uint8_t>>>
  QueryClean(const std::string &client_id, const std::string &sql_template,
             std::uint64_t value, const std::string &backend_hint = "",
             const std::string &database = "default",
             std::uint32_t *collided_buckets = nullptr);

  // The rows of a table for a SELECT with no WHERE (a full scan). rows are
  // plaintext payload records the caller decodes with the schema; total_rows is
  // the table's true row count (for a no-WHERE COUNT(*)). max_rows bounds how
  // many rows the server returns (0 returns none, just the count). Plaintext by
  // design: a no-WHERE query matches no secret value, so there is nothing to
  // hide. database empty means "default".
  struct ScanResult {
    std::vector<std::vector<std::uint8_t>> rows;
    std::uint64_t total_rows = 0;
  };
  absl::StatusOr<ScanResult> Scan(const std::string &database,
                                  const std::string &table,
                                  std::uint64_t max_rows);

  // Execute a private SELECT COUNT(*) query and return the number of matching
  // rows. The match count comes from the encrypted presence ciphertext summed
  // across every result bucket, so it is exact even when rows collide in a
  // bucket. Same value / literal handling as Query.
  absl::StatusOr<std::uint64_t>
  QueryCount(const std::string &client_id, const std::string &sql_template,
             std::uint64_t value, const std::string &backend_hint = "",
             const std::string &database = "default");

  // The outcome of an insert: the new epoch version and total row count.
  struct InsertResult {
    std::uint64_t epoch_version = 0;
    std::uint64_t row_count = 0;
  };

  // Append one row to a table. values are the column values as text in the
  // table's declared (CREATE TABLE) column order, including the match key. The
  // table must already exist. This is a plaintext insert; the values travel
  // over the (optionally TLS) channel but are stored in the clear, matching how
  // data is stored at rest today. A future encrypted-payload insert
  // encodes/encrypts the values on the client and uses this same call.
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

  // Runs a prepared query end to end: lifts inline literals, encrypts the
  // operand(s), executes, and decodes every result bucket. Query and QueryCount
  // share it and then interpret the buckets differently (rows vs. count).
  struct Decoded {
    sql::PreparedQuery prepared;
    std::vector<crypto::BucketResult> buckets;
  };
  absl::StatusOr<Decoded> RunQuery(const std::string &client_id,
                                   const std::string &sql_template,
                                   std::uint64_t value,
                                   const std::string &backend_hint,
                                   const std::string &database);

  config::Config cfg_;
  crypto::CryptoContext ctx_;
  crypto::ClientKeyring keyring_;
  std::unique_ptr<opaquedb::proto::OpaqueDB::Stub> stub_;
  std::string bearer_token_;
};

} // namespace opaquedb::client

#endif // OPAQUEDB_CLIENT_QUERY_CLIENT_H_
