#ifndef OPAQUEDB_CONFIG_CONFIG_H_
#define OPAQUEDB_CONFIG_CONFIG_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The typed configuration schema. This struct is the single source of truth for
// every setting. Nothing else reads the config file or the environment; the
// loader resolves all layers into one immutable Config that is injected
// everywhere. See loader.h for the resolution order.
//
// Keep this struct free of behaviour. It is plain data. Validation lives in
// Validate (loader.h) and parsing lives in the loader.

namespace opaquedb::config {

// How clients authenticate. Chosen by [auth].mode. None is only legal when
// enable_insecure is true; see Validate.
enum class AuthMode { kToken, kMtls, kNone };

// Where large evaluation key blobs are stored. Local is a directory now; s3 is
// a documented extension point and is the intended production path for
// distributing the large Galois key set: the coordinator writes a client's keys
// to a shared object store (S3 or a MinIO deployment) once, and shards read
// them by client id instead of having the keys re-streamed to every shard.
enum class BlobStoreKind { kLocal, kS3 };

// Log output format. JSON by default so journald and log shippers parse it.
enum class LogFormat { kJson, kText };

struct NodeConfig {
  // Empty means generate a stable random id on first start.
  std::string id;
  std::string data_dir = "/var/lib/opaquedb";
};

struct ClusterConfig {
  // Whether to join an etcd cluster (leader election, membership, shard map).
  // Off by default so a single node runs standalone without an etcd.
  bool enabled = false;
  std::vector<std::string> etcd_endpoints = {"http://127.0.0.1:2379"};
  std::string leader_key = "/opaquedb/leader";

  // Authentication to etcd itself. This is the connection from this node to the
  // etcd servers, separate from both client auth and node-to-node mTLS below.
  // Username/password auth is used when etcd_username is set. TLS to etcd is
  // used when etcd_ca_cert is set; etcd_client_cert/etcd_client_key add client
  // (mutual) TLS, and etcd_tls_name overrides the certificate host name when
  // connecting by IP. A simple build does not combine password auth with TLS in
  // one channel; TLS takes precedence when both groups are set.
  std::string etcd_username;
  std::string etcd_password;
  std::string etcd_ca_cert;
  std::string etcd_client_cert;
  std::string etcd_client_key;
  std::string etcd_tls_name;

  // The node-to-node (Internal) RPC runs on its own listener, separate from the
  // client-facing one, so cluster traffic is isolated from clients (the
  // Elasticsearch transport-port model). Empty shares the client listener, kept
  // for single-node and test setups; a clustered deployment binds this to a
  // private interface. advertise is the address peers dial; empty derives it
  // from listen (wrong for a wildcard bind, so a clustered node sets it).
  std::string listen;
  std::string advertise;

  // mTLS for the node-to-node (Internal) RPC, which carries client ciphertexts
  // and keys between shards. These are SEPARATE from the client-facing auth
  // certs so the cluster has its own trust domain. A node presents tls_cert and
  // verifies peers against ca_cert, and the Internal listener requires and
  // verifies a peer certificate (mutual TLS), so only a node holding a
  // cluster-CA-signed certificate can call Evaluate or RegisterKeys. A
  // clustered node (enabled = true) must set these (or server TLS) or it fails
  // to start; allow_insecure is the explicit, documented escape hatch for local
  // development only.
  std::string tls_cert;
  std::string tls_key;
  std::string ca_cert;
  bool allow_insecure = false;
};

struct ServerConfig {
  std::string listen = "0.0.0.0:50051";
  // The address peers reach this node on. Empty falls back to listen, which is
  // wrong for a wildcard bind, so a clustered node sets a routable address.
  std::string advertise;
  // Large enough for evaluation key streams. gRPC max message size is set from
  // this value.
  std::uint64_t max_message_bytes = 67108864;
  // Required when auth.mode = mtls.
  std::string tls_cert;
  std::string tls_key;
  // Token-bucket rate limit applied per authenticated principal: a flood from
  // one principal cannot starve the others. rate is the steady refill in
  // requests per second, burst the bucket capacity. A global limiter at
  // burst * 64 bounds total admission as a backstop.
  std::uint32_t rate_limit_per_second = 2000;
  std::uint32_t rate_limit_burst = 2000;
};

// How a client connects to a node. The CLI and any application client read
// this; a node ignores it. TLS is used when ca_cert is set (the client then
// verifies the server certificate); tls_cert and tls_key add a client
// certificate for mutual TLS (required when the server runs auth.mode = mtls).
// server_name overrides the certificate host name when dialing by IP or
// loopback. The client fails closed: with no TLS material and allow_insecure
// false it refuses to connect rather than silently sending a bearer token over
// a plaintext channel. Set allow_insecure for local development only.
struct ClientConfig {
  std::string ca_cert;
  std::string tls_cert;
  std::string tls_key;
  std::string server_name;
  bool allow_insecure = false;
};

// The single source of truth for the FHE parameter set. crypto reads these; it
// does not define them.
struct CryptoConfig {
  std::uint32_t poly_modulus_degree = 16384; // power of two
  std::uint32_t plain_modulus_bits = 20;
  // The bit-sliced equality matcher has multiplicative depth 1 + log2(key_bits)
  // (one square, then a rotate-and-multiply AND tree), plus plaintext
  // multiplies for the masked retrieve. At the default key_bits = 16 that is
  // depth 5, which exhausts the noise budget at poly_modulus_degree 8192, so
  // the default degree is 16384. These six primes total 349 bits, under the
  // 438-bit security limit for degree 16384, and leave a measured ~52-bit
  // budget after the full pipeline. Raising key_bits deepens the AND tree; add
  // primes here if it does.
  std::vector<int> coeff_modulus_bits = {60, 60, 60, 60, 60, 49};
  // The match key is binary-expanded into key_bits SIMD slots per record. The
  // key universe is 2^key_bits: an INT key must fit (exact match), a TEXT key
  // is FNV-hashed into it. Equality depth is 1 + log2(key_bits); keep it a
  // power of two so the AND tree is exact.
  std::uint32_t key_bits = 16;

  // How many result buckets a multi-match query partitions rows into. A query
  // that asks for more than one row (LIMIT > 1 or OFFSET > 0) returns a window
  // of these buckets; each bucket can hold one matching row. Rows that share a
  // key are spread across distinct buckets, so up to result_buckets rows with
  // the same key can be returned (page with OFFSET to walk all of them). Two
  // matching rows in one bucket collide and that bucket is dropped, so a larger
  // value lowers the collision ceiling at a small cost. Must be a power of two
  // and at most (poly_modulus_degree / 2) / key_bits. Default LIMIT 1 with no
  // OFFSET ignores this and uses a single bucket (the original single-match
  // path). Keep it modest for performance.
  std::uint32_t result_buckets = 16;

  // Payload bytes packed into one BatchEncoder slot. Each slot holds a value
  // below the plaintext modulus, so we keep one bit of headroom and pack
  // floor((plain_modulus_bits - 1) / 8) bytes, clamped to [1, 7]. This is the
  // single source for the slot width, used by storage layout and the client.
  std::uint32_t BytesPerSlot() const {
    const std::uint32_t bytes =
        plain_modulus_bits > 8 ? (plain_modulus_bits - 1) / 8 : 1;
    if (bytes < 1)
      return 1;
    if (bytes > 7)
      return 7;
    return bytes;
  }
};

struct StorageConfig {
  std::uint32_t record_bytes = 128;
  // Empty means data_dir/epochs.
  std::string epoch_dir;
};

struct AuthConfig {
  AuthMode mode = AuthMode::kToken;
  // Must be true to allow mode = none.
  bool enable_insecure = false;
  std::string token_file = "/etc/opaquedb/tokens";
  // Client CA for mtls.
  std::string ca_cert;
  // In mtls mode, the verified client certificate identities granted the Admin
  // role. Any other verified identity is a Query principal. Empty means every
  // mTLS client is Query only.
  std::vector<std::string> admin_identities;
};

struct BlobStoreConfig {
  BlobStoreKind kind = BlobStoreKind::kLocal;
  // Where the persistent keyring lives. Empty means data_dir/keys, so the keys
  // sit beside the epochs on the same volume by default (see KeyringDir).
  std::string path;
};

struct MetricsConfig {
  std::string listen = "0.0.0.0:9090";
};

struct LoggingConfig {
  // One of: trace, debug, info, warn, error, critical, off.
  std::string level = "info";
  LogFormat format = LogFormat::kJson;
  // Destination file. Empty means write to standard output.
  std::string file;
};

struct Config {
  NodeConfig node;
  ClusterConfig cluster;
  ServerConfig server;
  ClientConfig client;
  CryptoConfig crypto;
  StorageConfig storage;
  AuthConfig auth;
  BlobStoreConfig blobstore;
  MetricsConfig metrics;
  LoggingConfig logging;

  // The effective epoch directory, resolving the empty default to
  // data_dir/epochs. This is the legacy single-table root; multi-table
  // deployments use EpochRootFor.
  std::string EffectiveEpochDir() const;

  // The epoch repository root for one table, under
  // <base>/db/<database>/<table>/epochs, where <base> is storage.epoch_dir if
  // set, otherwise data_dir. The repository class is unchanged; it just opens
  // at this per-table root.
  std::string EpochRootFor(std::string_view database,
                           std::string_view table) const;

  // The directory that holds the per-database subtrees, <base>/db. The catalog
  // scans this to enumerate databases and tables.
  std::string DatabasesDir() const;

  // The persistent keyring directory, resolving the empty default to
  // data_dir/keys so the keys live on the same volume as the epochs.
  std::string KeyringDir() const;
};

// String conversions for the enums, used by parsing, printing, and error
// messages. They are the single place the wire spelling of each enum lives.
std::string ToString(AuthMode mode);
std::string ToString(BlobStoreKind kind);
std::string ToString(LogFormat format);

} // namespace opaquedb::config

#endif // OPAQUEDB_CONFIG_CONFIG_H_
