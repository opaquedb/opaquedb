#ifndef OPAQUEDB_SERVER_QUERY_SERVICE_H_
#define OPAQUEDB_SERVER_QUERY_SERVICE_H_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "grpcpp/grpcpp.h"
#include "opaquedb.grpc.pb.h"
#include "opaquedb/auth/authenticator.h"
#include "opaquedb/server/engine.h"
#include "opaquedb/server/request_gate.h"

// The gRPC adapter over the Engine for the client query API. Any node can
// coordinate: when the node has shard peers, Execute fans the encrypted query
// out to them and combines the encrypted partials; with no peers it answers
// from its own shard. Register stores the client's keys locally and forwards
// them to every peer so each shard can evaluate. All query logic lives in the
// engine and the coordinator.

namespace opaquedb::server {

// Converts an absl::Status to a gRPC status, preserving the code and message.
grpc::Status ToGrpcStatus(const absl::Status &status);

// Extracts the bearer token and verified peer identity from a gRPC context, so
// the gate can authenticate without the rest of the code touching gRPC.
auth::AuthInputs ExtractAuthInputs(const grpc::ServerContext &context);

class QueryService final : public proto::OpaqueDB::Service {
public:
  // peers() returns the addresses of the other shard nodes (empty for a lone
  // node). epoch() returns the global epoch to pin a query to (0 = this node's
  // current epoch). max_message_bytes sizes the peer channels.
  using PeerResolver = std::function<std::vector<std::string>()>;
  using EpochResolver = std::function<std::uint64_t()>;

  QueryService(Engine *engine, RequestGate *gate, PeerResolver peers,
               EpochResolver epoch, std::uint64_t max_message_bytes,
               std::shared_ptr<grpc::ChannelCredentials> peer_creds = nullptr)
      : engine_(engine), gate_(gate), peers_(std::move(peers)),
        epoch_(std::move(epoch)), max_message_bytes_(max_message_bytes),
        peer_creds_(std::move(peer_creds)) {}

  grpc::Status Register(grpc::ServerContext *context,
                        grpc::ServerReader<proto::RegisterChunk> *reader,
                        proto::RegisterReply *reply) override;

  grpc::Status Execute(grpc::ServerContext *context,
                       const proto::QueryRequest *request,
                       proto::QueryReply *reply) override;

  grpc::Status Insert(grpc::ServerContext *context,
                      const proto::InsertRequest *request,
                      proto::InsertReply *reply) override;

  grpc::Status DescribeTable(grpc::ServerContext *context,
                             const proto::DescribeRequest *request,
                             proto::DescribeReply *reply) override;

private:
  Engine *engine_;
  RequestGate *gate_;
  PeerResolver peers_;
  EpochResolver epoch_;
  std::uint64_t max_message_bytes_;
  std::shared_ptr<grpc::ChannelCredentials> peer_creds_;
};

} // namespace opaquedb::server

#endif // OPAQUEDB_SERVER_QUERY_SERVICE_H_
