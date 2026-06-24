#ifndef OPAQUEDB_SERVER_SHARD_SERVICE_H_
#define OPAQUEDB_SERVER_SHARD_SERVICE_H_

#include "grpcpp/grpcpp.h"
#include "internal.grpc.pb.h"
#include "opaquedb/server/engine.h"

// The node-to-node Evaluate service. A coordinating node calls this on each
// shard node; the shard runs the backend over its own segment at the pinned
// epoch and returns encrypted partials. It returns only ciphertexts, never
// plaintext, so it leaks nothing about the data. Today peer channels are
// created with insecure credentials (tests and current compose use no TLS
// between nodes). A production cluster would use separate mTLS creds for the
// Internal service.

namespace opaquedb::server {

class ShardService final : public proto::Internal::Service {
public:
  explicit ShardService(Engine *engine) : engine_(engine) {}

  grpc::Status Evaluate(grpc::ServerContext *context,
                        const proto::ShardQuery *request,
                        proto::ShardPartial *reply) override;

  grpc::Status RegisterKeys(grpc::ServerContext *context,
                            grpc::ServerReader<proto::KeyUploadChunk> *reader,
                            proto::KeyUploadReply *reply) override;

private:
  Engine *engine_;
};

} // namespace opaquedb::server

#endif // OPAQUEDB_SERVER_SHARD_SERVICE_H_
