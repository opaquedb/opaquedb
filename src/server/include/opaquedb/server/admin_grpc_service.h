#ifndef OPAQUEDB_SERVER_ADMIN_GRPC_SERVICE_H_
#define OPAQUEDB_SERVER_ADMIN_GRPC_SERVICE_H_

#include "admin.grpc.pb.h"
#include "grpcpp/grpcpp.h"
#include "opaquedb/admin/service.h"
#include "opaquedb/server/request_gate.h"

// The gRPC adapter over the admin::AdminService facade. It authorizes every
// call with the Admin role through the shared request gate, then translates
// wire messages to facade calls. No management logic lives here.

namespace opaquedb::server {

class AdminGrpcService final : public proto::Admin::Service {
public:
  AdminGrpcService(admin::AdminService *admin, RequestGate *gate)
      : admin_(admin), gate_(gate) {}

  grpc::Status Status(grpc::ServerContext *context,
                      const proto::StatusRequest *request,
                      proto::StatusReply *reply) override;
  grpc::Status InspectSchema(grpc::ServerContext *context,
                             const proto::SchemaRequest *request,
                             proto::SchemaReply *reply) override;
  grpc::Status ListEpochs(grpc::ServerContext *context,
                          const proto::ListEpochsRequest *request,
                          proto::ListEpochsReply *reply) override;
  grpc::Status RollbackEpoch(grpc::ServerContext *context,
                             const proto::RollbackRequest *request,
                             proto::RollbackReply *reply) override;
  grpc::Status Load(grpc::ServerContext *context,
                    grpc::ServerReader<proto::LoadChunk> *reader,
                    proto::LoadReply *reply) override;
  grpc::Status ListPrincipals(grpc::ServerContext *context,
                              const proto::PrincipalsRequest *request,
                              proto::PrincipalsReply *reply) override;

private:
  admin::AdminService *admin_;
  RequestGate *gate_;
};

} // namespace opaquedb::server

#endif // OPAQUEDB_SERVER_ADMIN_GRPC_SERVICE_H_
