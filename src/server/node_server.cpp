#include "opaquedb/server/node_server.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

#include "absl/strings/str_cat.h"
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

  keyring_ = std::make_unique<admin::InMemoryKeyringStore>();

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
  authenticator_ = *std::move(authenticator);
  if (config_.auth.mode == config::AuthMode::kNone) {
    spdlog::warn("auth mode is none; queries are unauthenticated");
  }
  gate_ = std::make_unique<RequestGate>(authenticator_.get());

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
  // fanning out to this node's shard.
  shard_service_ = std::make_unique<ShardService>(engine_.get());

  absl::StatusOr<std::shared_ptr<grpc::ServerCredentials>> creds =
      MakeServerCredentials(config_);
  if (!creds.ok())
    return creds.status();

  grpc::ServerBuilder builder;
  int selected_port = 0;
  builder.AddListeningPort(config_.server.listen, *creds, &selected_port);
  // Sized from config so the large evaluation key streams fit.
  const int max_bytes = static_cast<int>(
      std::min<std::uint64_t>(config_.server.max_message_bytes, 2147483647ULL));
  builder.SetMaxReceiveMessageSize(max_bytes);
  builder.SetMaxSendMessageSize(max_bytes);
  builder.RegisterService(service_.get());
  builder.RegisterService(admin_service_.get());
  builder.RegisterService(shard_service_.get());

  server_ = builder.BuildAndStart();
  if (server_ == nullptr || selected_port == 0) {
    return absl::UnavailableError(
        absl::StrCat("failed to start gRPC server on ", config_.server.listen));
  }
  // Reflect the actually-bound port, which matters when the config asked for
  // port 0 (an ephemeral port, used by tests).
  const std::string &listen = config_.server.listen;
  const std::size_t colon = listen.rfind(':');
  const std::string host =
      colon == std::string::npos ? listen : listen.substr(0, colon);
  listen_address_ = absl::StrCat(host, ":", selected_port);
  // The address peers reach this node on. Defaults to the listen address, but a
  // node that binds a wildcard must advertise a routable address for peers.
  advertise_address_ = config_.server.advertise.empty()
                           ? listen_address_
                           : config_.server.advertise;
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
    opts.node_id =
        config_.node.id.empty() ? advertise_address_ : config_.node.id;
    opts.node_address = advertise_address_;
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
      if (node.address != advertise_address_)
        view.peers.push_back(node.address);
    }
  }
  if (absl::StatusOr<std::uint64_t> epoch = cluster_->CurrentEpoch(); epoch.ok())
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
}

absl::Status NodeServer::WaitForReady(std::chrono::milliseconds timeout) {
  if (!server_)
    return absl::FailedPreconditionError("server not started");
  if (listen_address_.empty())
    return absl::FailedPreconditionError("no listen address");

  auto deadline = std::chrono::system_clock::now() + timeout;
  // Create a throwaway channel and wait for it to be connected.
  auto ch = grpc::CreateCustomChannel(listen_address_,
                                      grpc::InsecureChannelCredentials(),
                                      grpc::ChannelArguments());
  if (!ch->WaitForConnected(deadline))
    return absl::DeadlineExceededError("server not ready in time");
  return absl::OkStatus();
}

} // namespace opaquedb::server
