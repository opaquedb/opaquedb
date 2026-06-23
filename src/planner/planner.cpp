#include "opaquedb/planner/planner.h"

#include "absl/strings/str_cat.h"

namespace opaquedb::planner {

absl::StatusOr<backend::Op> RouteOperator(core::ColumnEncoding encoding,
                                          sql::CompareOp op) {
  switch (encoding) {
  case core::ColumnEncoding::kEq:
    switch (op) {
    case sql::CompareOp::kEq:
      return backend::Op::kEq;
    case sql::CompareOp::kNe:
      return backend::Op::kNe;
    default:
      return absl::InvalidArgumentError(
          "an EQ column supports only '=' and '<>'; declare a RANGE "
          "encoding for ordered comparisons");
    }
  case core::ColumnEncoding::kPrefix:
    return absl::UnimplementedError(
        "PREFIX column matching is not yet evaluated");
  case core::ColumnEncoding::kRange:
    return absl::UnimplementedError(
        "RANGE column matching is not yet evaluated");
  case core::ColumnEncoding::kRaw:
    return absl::InvalidArgumentError(
        "a RAW column is payload only and cannot be matched");
  }
  return absl::InternalError("unknown column encoding");
}

absl::StatusOr<std::string>
Planner::SelectBackend(backend::Op op, const std::string &backend_hint) const {
  const std::vector<backend::BackendCapabilities> all = registry_.list();
  if (!backend_hint.empty()) {
    for (const backend::BackendCapabilities &caps : all) {
      if (caps.name != backend_hint)
        continue;
      if (!caps.Supports(op)) {
        return absl::FailedPreconditionError(
            absl::StrCat("backend '", backend_hint, "' does not support ",
                         backend::ToString(op)));
      }
      if (!caps.Supports(backend::Privacy::kQueryPrivate)) {
        return absl::FailedPreconditionError(absl::StrCat(
            "backend '", backend_hint, "' does not support QueryPrivate mode"));
      }
      return caps.name;
    }
    return absl::NotFoundError(
        absl::StrCat("no backend named '", backend_hint, "' is registered"));
  }

  for (const backend::BackendCapabilities &caps : all) {
    if (caps.Supports(op) && caps.Supports(backend::Privacy::kQueryPrivate)) {
      return caps.name;
    }
  }
  return absl::NotFoundError(absl::StrCat("no registered backend supports ",
                                          backend::ToString(op),
                                          " in QueryPrivate mode"));
}

absl::StatusOr<PhysicalPlan>
Planner::Plan(const core::Schema &schema, const sql::LogicalPlan &logical,
              const std::string &backend_hint) const {
  if (logical.table != schema.table()) {
    return absl::InvalidArgumentError(
        absl::StrCat("query names table '", logical.table,
                     "' but the schema is '", schema.table(), "'"));
  }

  const std::optional<std::size_t> match_index =
      schema.IndexOf(logical.match_column);
  if (!match_index) {
    return absl::InvalidArgumentError(
        absl::StrCat("unknown match column '", logical.match_column, "'"));
  }
  const core::ColumnEncoding encoding = schema.columns()[*match_index].encoding;
  absl::StatusOr<backend::Op> op = RouteOperator(encoding, logical.op);
  if (!op.ok())
    return op.status();

  PhysicalPlan plan;
  plan.table = logical.table;
  plan.op = *op;
  plan.match_column = logical.match_column;
  plan.match_column_index = *match_index;
  plan.parameter = logical.parameter;

  if (logical.select_all) {
    // SELECT * expands to every payload (RAW) column in schema order. The match
    // key is stored as a key value, not in the payload, so it is not
    // projectable.
    for (std::size_t i = 0; i < schema.columns().size(); ++i) {
      if (schema.columns()[i].encoding != core::ColumnEncoding::kRaw)
        continue;
      plan.projection.push_back(schema.columns()[i].name);
      plan.projection_indices.push_back(i);
    }
  } else {
    for (const std::string &column : logical.projection) {
      const std::optional<std::size_t> idx = schema.IndexOf(column);
      if (!idx) {
        return absl::InvalidArgumentError(
            absl::StrCat("unknown projected column '", column, "'"));
      }
      plan.projection.push_back(column);
      plan.projection_indices.push_back(*idx);
    }
  }

  absl::StatusOr<std::string> backend_name = SelectBackend(*op, backend_hint);
  if (!backend_name.ok())
    return backend_name.status();
  plan.backend_name = *std::move(backend_name);

  return plan;
}

} // namespace opaquedb::planner
