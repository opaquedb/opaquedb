#include "opaquedb/server/admin_grpc_service.h"

#include <cstdint>
#include <string>
#include <vector>

#include "opaquedb/core/wire.h"
#include "opaquedb/server/ingest.h"
#include "opaquedb/server/query_service.h"

namespace opaquedb::server {
namespace {

// Authorizes an admin call and checks the wire version. Returns OK or the gRPC
// status to return to the client.
grpc::Status AdmitAdmin(RequestGate *gate, const grpc::ServerContext &context,
                        std::uint32_t wire_version) {
  if (wire_version != core::kWireVersion) {
    return ToGrpcStatus(absl::FailedPreconditionError(
        absl::StrCat("wire version ", wire_version, " not supported")));
  }
  absl::StatusOr<auth::Principal> principal =
      gate->Check(ExtractAuthInputs(context), auth::Role::kAdmin);
  if (!principal.ok())
    return ToGrpcStatus(principal.status());
  return grpc::Status::OK;
}

} // namespace

grpc::Status AdminGrpcService::Status(grpc::ServerContext *context,
                                      const proto::StatusRequest *request,
                                      proto::StatusReply *reply) {
  if (grpc::Status s = AdmitAdmin(gate_, *context, request->wire_version());
      !s.ok()) {
    return s;
  }
  absl::StatusOr<admin::AdminService::NodeStatus> status = admin_->GetStatus();
  if (!status.ok())
    return ToGrpcStatus(status.status());
  reply->set_node_id(status->node_id);
  reply->set_role(status->role);
  reply->set_has_epoch(status->has_epoch);
  reply->set_epoch_version(status->epoch_version);
  reply->set_row_count(status->row_count);
  reply->set_auth_mode(status->auth_mode);
  for (const std::string &backend : status->backends) {
    reply->add_backends(backend);
  }
  return grpc::Status::OK;
}

grpc::Status
AdminGrpcService::InspectSchema(grpc::ServerContext *context,
                                const proto::SchemaRequest *request,
                                proto::SchemaReply *reply) {
  if (grpc::Status s = AdmitAdmin(gate_, *context, request->wire_version());
      !s.ok()) {
    return s;
  }
  absl::StatusOr<core::Schema> schema = admin_->InspectSchema();
  if (!schema.ok())
    return ToGrpcStatus(schema.status());
  reply->set_table(schema->table());
  for (const core::Column &column : schema->columns()) {
    proto::ColumnInfo *info = reply->add_columns();
    info->set_name(column.name);
    info->set_encoding(core::ToString(column.encoding));
  }
  return grpc::Status::OK;
}

grpc::Status
AdminGrpcService::ListEpochs(grpc::ServerContext *context,
                             const proto::ListEpochsRequest *request,
                             proto::ListEpochsReply *reply) {
  if (grpc::Status s = AdmitAdmin(gate_, *context, request->wire_version());
      !s.ok()) {
    return s;
  }
  absl::StatusOr<std::vector<std::uint64_t>> versions = admin_->ListEpochs();
  if (!versions.ok())
    return ToGrpcStatus(versions.status());
  for (std::uint64_t v : *versions)
    reply->add_versions(v);
  if (absl::StatusOr<admin::AdminService::NodeStatus> status =
          admin_->GetStatus();
      status.ok() && status->has_epoch) {
    reply->set_has_current(true);
    reply->set_current(status->epoch_version);
  }
  return grpc::Status::OK;
}

grpc::Status
AdminGrpcService::RollbackEpoch(grpc::ServerContext *context,
                                const proto::RollbackRequest *request,
                                proto::RollbackReply *reply) {
  if (grpc::Status s = AdmitAdmin(gate_, *context, request->wire_version());
      !s.ok()) {
    return s;
  }
  if (absl::Status s = admin_->RollbackEpoch(request->version()); !s.ok()) {
    return ToGrpcStatus(s);
  }
  reply->set_current(request->version());
  return grpc::Status::OK;
}

grpc::Status
AdminGrpcService::Load(grpc::ServerContext *context,
                       grpc::ServerReader<proto::LoadChunk> *reader,
                       proto::LoadReply *reply) {
  // The first chunk carries the table and column names; rows follow.
  std::string table;
  std::string key_column;
  std::string value_column;
  bool header = false;
  std::vector<IngestRow> rows;

  proto::LoadChunk chunk;
  while (reader->Read(&chunk)) {
    if (chunk.wire_version() != core::kWireVersion) {
      return ToGrpcStatus(absl::FailedPreconditionError("bad wire version"));
    }
    if (!header) {
      // Authorize once we have a context; do it on the first chunk.
      if (grpc::Status s = AdmitAdmin(gate_, *context, chunk.wire_version());
          !s.ok()) {
        return s;
      }
      table = chunk.table();
      key_column = chunk.key_column();
      value_column = chunk.value_column();
      header = true;
    } else if (chunk.table() != table || chunk.key_column() != key_column ||
               chunk.value_column() != value_column) {
      return ToGrpcStatus(absl::InvalidArgumentError(
          "load chunks disagree on the table or columns"));
    }
    IngestRow row;
    // The Load RPC carries an integer key; TEXT keys load through the CSV path.
    row.key = core::Value{static_cast<std::int64_t>(chunk.key())};
    row.payload.push_back(core::Value{std::string(chunk.value())});
    rows.push_back(std::move(row));
  }
  if (!header) {
    return ToGrpcStatus(
        absl::InvalidArgumentError("load stream carried no chunks"));
  }

  // The streaming Load RPC carries a simple key/value pair per chunk, so it
  // ingests under a two-column schema: an integer key and a text value. Richer
  // typed schemas come from a CREATE TABLE statement on the file load path.
  core::Schema schema(table,
                      {core::Column{key_column, core::ColumnEncoding::kEq,
                                    core::ColumnType::kInt},
                       core::Column{value_column, core::ColumnEncoding::kRaw,
                                    core::ColumnType::kText}});
  absl::StatusOr<storage::StagingEpoch> staging =
      BuildStagingFromRows(admin_->config(), schema, rows);
  if (!staging.ok())
    return ToGrpcStatus(staging.status());
  absl::StatusOr<std::uint64_t> version = admin_->Publish(*staging);
  if (!version.ok())
    return ToGrpcStatus(version.status());
  reply->set_epoch_version(*version);
  reply->set_rows(rows.size());
  return grpc::Status::OK;
}

grpc::Status
AdminGrpcService::ListPrincipals(grpc::ServerContext *context,
                                 const proto::PrincipalsRequest *request,
                                 proto::PrincipalsReply *reply) {
  if (grpc::Status s = AdmitAdmin(gate_, *context, request->wire_version());
      !s.ok()) {
    return s;
  }
  for (const std::string &id : admin_->ListPrincipals()) {
    reply->add_client_ids(id);
  }
  return grpc::Status::OK;
}

} // namespace opaquedb::server
