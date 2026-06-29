#include "opaquedb/server/node_server.h"

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

#include "absl/strings/str_cat.h"
#include "opaquedb/admin/file_keyring_store.h"
#include "opaquedb/auth/authenticators.h"
#include "opaquedb/backend/reference/reference.h"
#include "opaquedb/cluster/real_etcd_client.h"
#include "opaquedb/log/log.h"

namespace opaquedb::server {
namespace {

absl::StatusOr<std::string> ReadFile(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return absl::NotFoundError(absl::StrCat("cannot read '", path, "'"));
  }
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

// Builds server credentials from the auth and TLS config. mtls mode requires a
// server certificate and a client CA and verifies client certificates. token
// and none modes use server TLS when a certificate is configured, otherwise an
// insecure channel (acceptable only for local development).
absl::StatusOr<std::shared_ptr<grpc::ServerCredentials>>
MakeServerCredentials(const config::Config &config) {
  const config::ServerConfig &server = config.server;
  if (config.auth.mode == config::AuthMode::kMtls) {
    if (server.tls_cert.empty() || server.tls_key.empty() ||
        config.auth.ca_cert.empty()) {
      return absl::FailedPreconditionError(
          "mtls mode requires server.tls_cert, server.tls_key, and "
          "auth.ca_cert");
    }
    absl::StatusOr<std::string> cert = ReadFile(server.tls_cert);
    if (!cert.ok())
      return cert.status();
    absl::StatusOr<std::string> key = ReadFile(server.tls_key);
    if (!key.ok())
      return key.status();
    absl::StatusOr<std::string> ca = ReadFile(config.auth.ca_cert);
    if (!ca.ok())
      return ca.status();
    grpc::SslServerCredentialsOptions opts(
        GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY);
    opts.pem_root_certs = *ca;
    opts.pem_key_cert_pairs.push_back({*key, *cert});
    return grpc::SslServerCredentials(opts);
  }

  if (!server.tls_cert.empty() && !server.tls_key.empty()) {
    absl::StatusOr<std::string> cert = ReadFile(server.tls_cert);
    if (!cert.ok())
      return cert.status();
    absl::StatusOr<std::string> key = ReadFile(server.tls_key);
    if (!key.ok())
      return key.status();
    grpc::SslServerCredentialsOptions opts;
    opts.pem_key_cert_pairs.push_back({*key, *cert});
    return grpc::SslServerCredentials(opts);
  }

  return grpc::InsecureServerCredentials();
}

// Builds channel credentials for outbound connections to peer nodes (Internal
// service). Cluster mTLS is its own trust domain: when
// cluster.tls_cert/tls_key/ ca_cert are set, the node presents the cluster
// certificate and verifies peers against the cluster CA, independent of the
// client-facing auth certs. If only server TLS is configured, fall back to
// that. Insecure is reached only when the operator set cluster.allow_insecure
// (config validation enforces this), or for a non-clustered node with no peers.
std::shared_ptr<grpc::ChannelCredentials>
MakePeerChannelCredentials(const config::Config &config) {
  const config::ClusterConfig &cl = config.cluster;
  if (!cl.tls_cert.empty() && !cl.tls_key.empty() && !cl.ca_cert.empty()) {
    grpc::SslCredentialsOptions opts;
    auto ca = ReadFile(cl.ca_cert);
    auto cert = ReadFile(cl.tls_cert);
    auto key = ReadFile(cl.tls_key);
    if (ca.ok())
      opts.pem_root_certs = *ca;
    if (cert.ok() && key.ok()) {
      opts.pem_cert_chain = *cert;
      opts.pem_private_key = *key;
    }
    return grpc::SslCredentials(opts);
  }

  const config::ServerConfig &server = config.server;
  if (config.auth.mode == config::AuthMode::kMtls ||
      (!server.tls_cert.empty() && !server.tls_key.empty())) {
    grpc::SslCredentialsOptions opts;
    if (!config.auth.ca_cert.empty()) {
      auto ca = ReadFile(config.auth.ca_cert);
      if (ca.ok())
        opts.pem_root_certs = *ca;
    }
    if (!server.tls_cert.empty() && !server.tls_key.empty()) {
      auto cert = ReadFile(server.tls_cert);
      auto key = ReadFile(server.tls_key);
      if (cert.ok() && key.ok()) {
        opts.pem_cert_chain = *cert;
        opts.pem_private_key = *key;
      }
    }
    return grpc::SslCredentials(opts);
  }
  return grpc::InsecureChannelCredentials();
}

// Whether the cluster has its own mTLS trust domain configured (all three of
// cert, key, and CA). When true the Internal listener runs mutual TLS and the
// shard service requires a verified peer certificate.
bool HasClusterMtls(const config::Config &config) {
  const config::ClusterConfig &cl = config.cluster;
  return !cl.tls_cert.empty() && !cl.tls_key.empty() && !cl.ca_cert.empty();
}

// Server credentials for the separate node-to-node listener. With cluster mTLS
// it presents the cluster certificate and requires and verifies a peer
// certificate signed by the cluster CA, so only a cluster member can reach the
// Internal RPC. Without cluster certs it falls back to the client-facing
// credentials (server TLS or, in local development, insecure).
absl::StatusOr<std::shared_ptr<grpc::ServerCredentials>>
MakeClusterServerCredentials(const config::Config &config) {
  if (!HasClusterMtls(config))
    return MakeServerCredentials(config);
  const config::ClusterConfig &cl = config.cluster;
  absl::StatusOr<std::string> cert = ReadFile(cl.tls_cert);
  if (!cert.ok())
    return cert.status();
  absl::StatusOr<std::string> key = ReadFile(cl.tls_key);
  if (!key.ok())
    return key.status();
  absl::StatusOr<std::string> ca = ReadFile(cl.ca_cert);
  if (!ca.ok())
    return ca.status();
  grpc::SslServerCredentialsOptions opts(
      GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY);
  opts.pem_root_certs = *ca;
  opts.pem_key_cert_pairs.push_back({*key, *cert});
  return grpc::SslServerCredentials(opts);
}

// Rebuilds host:port from a configured listen address and the actually-bound
// port, which matters when the config asked for port 0 (an ephemeral port,
// used by tests).
std::string BoundAddress(const std::string &listen, int port) {
  const std::size_t colon = listen.rfind(':');
  const std::string host =
      colon == std::string::npos ? listen : listen.substr(0, colon);
  return absl::StrCat(host, ":", port);
}

} // namespace

absl::StatusOr<std::unique_ptr<NodeServer>>
NodeServer::Create(config::Config config) {
  return std::unique_ptr<NodeServer>(new NodeServer(std::move(config)));
}

absl::Status NodeServer::Start() {
  // Register the backends this node ships. The node is the composition root, so
  // it names concrete backends here; the engine itself stays backend-agnostic.
  backend::reference::LinkReferenceBackend();

  absl::StatusOr<std::unique_ptr<storage::LocalEpochRepository>> repo =
      storage::LocalEpochRepository::Open(config_.EffectiveEpochDir());
  if (!repo.ok())
    return repo.status();
  repo_ = *std::move(repo);

  // The keyring persists each client's public and evaluation keys so a client
  // registers once and the keys survive a node restart, instead of being held
  // only in memory.
  const std::string keyring_dir = config_.KeyringDir();
  absl::StatusOr<std::unique_ptr<admin::FileKeyringStore>> store =
      admin::FileKeyringStore::Open(keyring_dir);
  if (!store.ok())
    return store.status();
  keyring_ = *std::move(store);
  spdlog::info("keyring persisted to {}", keyring_dir);

  // The engine routes each query to the repository that holds its table, across
  // many databases and tables. The manager opens per-table repositories on
  // demand under data_dir/db/<database>/<table>/epochs.
  repo_manager_ = std::make_unique<RepositoryManager>(config_);
  absl::StatusOr<std::unique_ptr<Engine>> engine = Engine::Create(
      config_.crypto,
      [this](const core::TableId &id)
          -> absl::StatusOr<storage::EpochRepository *> {
        return repo_manager_->Get(id);
      },
      keyring_.get());
  if (!engine.ok())
    return engine.status();
  engine_ = *std::move(engine);

  // Build the authenticator strategy chosen by config. The gate enforces it on
  // every query RPC, so queries require authentication by default.
  absl::StatusOr<std::unique_ptr<auth::Authenticator>> authenticator =
      auth::MakeAuthenticator(config_.auth);
  if (!authenticator.ok())
    return authenticator.status();
  authenticator_ =
      std::shared_ptr<auth::Authenticator>(*std::move(authenticator));
  if (config_.auth.mode == config::AuthMode::kNone) {
    spdlog::warn("auth mode is none; queries are unauthenticated");
  }
  gate_ = std::make_unique<RequestGate>(authenticator_,
                                        config_.server.rate_limit_per_second,
                                        config_.server.rate_limit_burst);

  // Resolvers the query service consults per request: the peer shard addresses
  // (from etcd membership when clustered, otherwise the static list) and the
  // global epoch to pin to.
  auto peer_resolver = [this]() -> std::vector<std::string> {
    if (cluster_ != nullptr)
      return ClusterViewCached().peers;
    std::lock_guard<std::mutex> lock(peers_mutex_);
    return query_peers_;
  };
  auto epoch_resolver = [this]() -> std::uint64_t {
    if (cluster_ != nullptr)
      return ClusterViewCached().epoch;
    return 0;
  };
  peer_creds_ = MakePeerChannelCredentials(config_);
  service_ = std::make_unique<QueryService>(
      engine_.get(), gate_.get(), std::move(peer_resolver),
      std::move(epoch_resolver), config_.server.max_message_bytes, peer_creds_);

  admin_ = std::make_unique<admin::AdminService>(config_, repo_.get(),
                                                 keyring_.get());
  admin_service_ =
      std::make_unique<AdminGrpcService>(admin_.get(), gate_.get());

  // The node-to-node Evaluate service lets any node coordinate a query by
  // fanning out to this node's shard. cluster.listen, when set, gives it its
  // own listener isolated from clients; otherwise it shares the client
  // listener. The shard service requires a verified peer certificate exactly
  // when its listener verifies client certs: cluster mTLS on a separate
  // listener, or client mTLS on the shared one.
  const bool separate_cluster = !config_.cluster.listen.empty();
  const bool require_peer_cert =
      separate_cluster ? HasClusterMtls(config_)
                       : (config_.auth.mode == config::AuthMode::kMtls);
  shard_service_ =
      std::make_unique<ShardService>(engine_.get(), require_peer_cert);

  // Sized from config so the large evaluation key streams fit.
  const int max_bytes = static_cast<int>(
      std::min<std::uint64_t>(config_.server.max_message_bytes, 2147483647ULL));

  // The client-facing listener: query and admin always, plus the shard service
  // when it is not on its own listener.
  absl::StatusOr<std::shared_ptr<grpc::ServerCredentials>> creds =
      MakeServerCredentials(config_);
  if (!creds.ok())
    return creds.status();

  grpc::ServerBuilder builder;
  int selected_port = 0;
  builder.AddListeningPort(config_.server.listen, *creds, &selected_port);
  builder.SetMaxReceiveMessageSize(max_bytes);
  builder.SetMaxSendMessageSize(max_bytes);
  builder.RegisterService(service_.get());
  builder.RegisterService(admin_service_.get());
  if (!separate_cluster)
    builder.RegisterService(shard_service_.get());

  server_ = builder.BuildAndStart();
  if (server_ == nullptr || selected_port == 0) {
    return absl::UnavailableError(
        absl::StrCat("failed to start gRPC server on ", config_.server.listen));
  }
  listen_address_ = BoundAddress(config_.server.listen, selected_port);
  // The address peers reach this node on. Defaults to the listen address, but a
  // node that binds a wildcard must advertise a routable address for peers.
  advertise_address_ = config_.server.advertise.empty()
                           ? listen_address_
                           : config_.server.advertise;

  // The separate node-to-node listener, when configured. It carries only the
  // Internal service, runs cluster mTLS, and is meant for a private interface.
  if (separate_cluster) {
    absl::StatusOr<std::shared_ptr<grpc::ServerCredentials>> cluster_creds =
        MakeClusterServerCredentials(config_);
    if (!cluster_creds.ok())
      return cluster_creds.status();
    grpc::ServerBuilder cluster_builder;
    int cluster_port = 0;
    cluster_builder.AddListeningPort(config_.cluster.listen, *cluster_creds,
                                     &cluster_port);
    cluster_builder.SetMaxReceiveMessageSize(max_bytes);
    cluster_builder.SetMaxSendMessageSize(max_bytes);
    cluster_builder.RegisterService(shard_service_.get());
    cluster_server_ = cluster_builder.BuildAndStart();
    if (cluster_server_ == nullptr || cluster_port == 0) {
      return absl::UnavailableError(absl::StrCat(
          "failed to start node-to-node server on ", config_.cluster.listen));
    }
    cluster_listen_address_ =
        BoundAddress(config_.cluster.listen, cluster_port);
    cluster_advertise_address_ = config_.cluster.advertise.empty()
                                     ? cluster_listen_address_
                                     : config_.cluster.advertise;
    spdlog::info("node-to-node gRPC listening on {} (advertise {}, {})",
                 cluster_listen_address_, cluster_advertise_address_,
                 require_peer_cert ? "mutual TLS" : "INSECURE");
  } else {
    cluster_listen_address_ = listen_address_;
    cluster_advertise_address_ = advertise_address_;
  }
  spdlog::info("gRPC server listening on {} (advertise {})", listen_address_,
               advertise_address_);

  // Join the etcd cluster to self-organize: elect a leader, register
  // membership, and follow the shard map. Off unless explicitly enabled, so a
  // lone node runs standalone with no etcd.
  if (config_.cluster.enabled) {
#ifdef OPAQUEDB_HAVE_ETCD
    cluster::EtcdAuth etcd_auth;
    etcd_auth.username = config_.cluster.etcd_username;
    etcd_auth.password = config_.cluster.etcd_password;
    etcd_auth.ca_cert = config_.cluster.etcd_ca_cert;
    etcd_auth.client_cert = config_.cluster.etcd_client_cert;
    etcd_auth.client_key = config_.cluster.etcd_client_key;
    etcd_auth.tls_name = config_.cluster.etcd_tls_name;
    absl::StatusOr<std::unique_ptr<cluster::EtcdClient>> etcd =
        cluster::MakeRealEtcdClient(config_.cluster.etcd_endpoints, etcd_auth);
    if (!etcd.ok())
      return etcd.status();
    etcd_ = *std::move(etcd);
    cluster::ClusterOptions opts;
    // Peers reach this node's shard over the node-to-node listener, so the
    // address published to etcd is the cluster advertise address (which equals
    // the client advertise address when the listener is shared).
    opts.node_id =
        config_.node.id.empty() ? cluster_advertise_address_ : config_.node.id;
    opts.node_address = cluster_advertise_address_;
    opts.leader_key = config_.cluster.leader_key;
    cluster_ = std::make_unique<cluster::ClusterManager>(etcd_.get(), opts);
    cluster_->Start();
    spdlog::info("joined cluster as node {} (etcd endpoints: {})", opts.node_id,
                 config_.cluster.etcd_endpoints.empty()
                     ? "none"
                     : config_.cluster.etcd_endpoints.front());
#else
    return absl::FailedPreconditionError(
        "cluster.enabled is set but this build has no etcd support");
#endif
  } else {
    spdlog::info("running standalone (cluster disabled)");
  }

  // Wire publish -> global epoch advance (only leader will succeed).
  if (cluster_) {
    admin_->SetPostPublishHook(
        [this](std::uint64_t v) { return cluster_->AdvanceEpoch(v); });
  }

  return absl::OkStatus();
}

void NodeServer::Wait() {
  if (server_ != nullptr)
    server_->Wait();
}

void NodeServer::SetQueryPeers(std::vector<std::string> peers) {
  std::lock_guard<std::mutex> lock(peers_mutex_);
  query_peers_ = std::move(peers);
}

absl::Status NodeServer::ReloadAuth() {
  absl::StatusOr<std::unique_ptr<auth::Authenticator>> authenticator =
      auth::MakeAuthenticator(config_.auth);
  if (!authenticator.ok()) {
    // Keep the old authenticator in place; a bad reload must not open the node.
    spdlog::error("auth reload failed, keeping current credentials: {}",
                  authenticator.status().message());
    return authenticator.status();
  }
  std::shared_ptr<auth::Authenticator> next(*std::move(authenticator));
  if (gate_ != nullptr)
    gate_->SetAuthenticator(next);
  authenticator_ = std::move(next);
  spdlog::info("audit: reloaded authentication credentials");
  return absl::OkStatus();
}

NodeServer::ClusterView NodeServer::ClusterViewCached() {
  // A short TTL: long enough that a burst of queries shares one etcd round
  // trip, short enough that a membership or epoch change propagates within a
  // second. Failure detection still runs on the cluster lease, not here.
  constexpr auto kTtl = std::chrono::milliseconds(1000);
  const auto now = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> lock(cluster_cache_mu_);
    if (cluster_cache_valid_ && now - cluster_cache_at_ < kTtl)
      return cluster_cache_;
  }

  // Refresh outside the lock so a slow etcd read does not block other queries
  // reading the cache. A failed read yields an empty view rather than throwing.
  ClusterView view;
  if (absl::StatusOr<std::vector<cluster::NodeInfo>> members =
          cluster_->Members();
      members.ok()) {
    for (const cluster::NodeInfo &node : *members) {
      if (node.address != cluster_advertise_address_)
        view.peers.push_back(node.address);
    }
  }
  if (absl::StatusOr<std::uint64_t> epoch = cluster_->CurrentEpoch();
      epoch.ok())
    view.epoch = *epoch;

  std::lock_guard<std::mutex> lock(cluster_cache_mu_);
  cluster_cache_ = view;
  cluster_cache_at_ = now;
  cluster_cache_valid_ = true;
  return view;
}

void NodeServer::Shutdown() {
  spdlog::info("node shutting down");
  // Leave the cluster first so the lease is revoked and peers react promptly.
  if (cluster_ != nullptr)
    cluster_->Stop();
  if (server_ != nullptr)
    server_->Shutdown();
  if (cluster_server_ != nullptr)
    cluster_server_->Shutdown();
}

absl::Status NodeServer::WaitForReady(std::chrono::milliseconds timeout) {
  if (!server_)
    return absl::FailedPreconditionError("server not started");
  if (listen_address_.empty())
    return absl::FailedPreconditionError("no listen address");

  // Probe with a plain TCP connect rather than a gRPC channel: the listener is
  // up once Start returns, and a bare connect signals readiness without needing
  // credentials, so this works whether or not the listener runs TLS.
  const std::size_t colon = listen_address_.rfind(':');
  if (colon == std::string::npos)
    return absl::FailedPreconditionError("malformed listen address");
  const std::string host = listen_address_.substr(0, colon);
  const std::string port = listen_address_.substr(colon + 1);

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *res = nullptr;
  if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 ||
      res == nullptr)
    return absl::InvalidArgumentError(
        absl::StrCat("cannot resolve listen address ", listen_address_));

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  bool connected = false;
  while (!connected && std::chrono::steady_clock::now() < deadline) {
    int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd >= 0) {
      if (::connect(fd, res->ai_addr, res->ai_addrlen) == 0)
        connected = true;
      ::close(fd);
    }
    if (!connected)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  freeaddrinfo(res);
  if (!connected)
    return absl::DeadlineExceededError("server not ready in time");
  return absl::OkStatus();
}

} // namespace opaquedb::server
