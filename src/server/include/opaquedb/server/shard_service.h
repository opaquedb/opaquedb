#ifndef OPAQUEDB_SERVER_SHARD_SERVICE_H_
#define OPAQUEDB_SERVER_SHARD_SERVICE_H_

#include "grpcpp/grpcpp.h"
#include "internal.grpc.pb.h"
#include "opaquedb/server/engine.h"

// The node-to-node Evaluate service. A coordinating node calls this on each
// shard node; the shard runs the backend over its own segment at the pinned
// epoch and returns encrypted partials. It returns only ciphertexts, never
// plaintext, so it leaks nothing about the data.
//
// Access control: the Internal listener runs mutual TLS with the cluster CA, so
// the transport already proves a caller holds a cluster-CA-signed certificate.
// When require_peer_cert is set the service additionally checks that a verified
// peer identity is present and refuses the call otherwise, defense in depth
// against a misconfigured listener. require_peer_cert is false only for local
// development (cluster.allow_insecure), where there is no cluster TLS.

namespace opaquedb::server {

class ShardService final : public proto::Internal::Service {
public:
  explicit ShardService(Engine *engine, bool require_peer_cert = false)
      : engine_(engine), require_peer_cert_(require_peer_cert) {}

  grpc::Status Evaluate(grpc::ServerContext *context,
                        const proto::ShardQuery *request,
                        proto::ShardPartial *reply) override;

  grpc::Status RegisterKeys(grpc::ServerContext *context,
                            grpc::ServerReader<proto::KeyUploadChunk> *reader,
                            proto::KeyUploadReply *reply) override;

private:
  // Verifies the caller is an authenticated cluster peer. Returns OK to proceed
  // or a PermissionDenied status to reject. No-op when require_peer_cert_ is
  // false (insecure local development).
  grpc::Status AuthorizePeer(const grpc::ServerContext &context) const;

  Engine *engine_;
  bool require_peer_cert_;
};

} // namespace opaquedb::server

#endif // OPAQUEDB_SERVER_SHARD_SERVICE_H_
