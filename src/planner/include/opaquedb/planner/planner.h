#ifndef OPAQUEDB_PLANNER_PLANNER_H_
#define OPAQUEDB_PLANNER_PLANNER_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "opaquedb/backend/capabilities.h"
#include "opaquedb/backend/registry.h"
#include "opaquedb/core/schema.h"
#include "opaquedb/sql/logical_plan.h"

// The planner turns a schema-agnostic logical plan into a physical plan: it
// resolves columns against the schema, routes the operator by the match
// column's declared encoding, and selects a backend by capability. Adding an
// operator later touches only the routing here and a backend, never transport
// or storage.

namespace opaquedb::planner {

// The resolved, executable plan the engine runs. Column indices are resolved
// against the schema so the engine reads the right segments without re-parsing.
struct PhysicalPlan {
  std::string table;
  std::string backend_name;
  backend::Op op = backend::Op::kEq;

  std::string match_column;
  std::size_t match_column_index = 0;
  std::string parameter; // the bound parameter naming the encrypted value
  // How many encrypted operands the match carries (1 for a point query, more
  // for IN / same-column OR). The engine deserializes exactly this many.
  std::size_t match_operands = 1;
  // SELECT COUNT(*): the client reads the encrypted match count, not rows.
  bool count = false;

  std::vector<std::string> projection;
  std::vector<std::size_t> projection_indices;

  // Public result window carried from the SQL LIMIT/OFFSET. Unset limit means
  // the default of 1 (single-match, collapse all blocks into one bucket). The
  // engine resolves these against the configured result_buckets.
  std::optional<std::uint64_t> limit;
  std::optional<std::uint64_t> offset;
};

// Maps a column encoding and a SQL operator to a backend operator, or returns a
// status when the pair is illegal (for example a range comparison on an EQ
// column) or not yet evaluable.
absl::StatusOr<backend::Op> RouteOperator(core::ColumnEncoding encoding,
                                          sql::CompareOp op);

class Planner {
public:
  // The registry is injected so tests can supply their own. It must outlive the
  // planner.
  explicit Planner(const backend::BackendRegistry &registry =
                       backend::BackendRegistry::instance())
      : registry_(registry) {}

  // Plans a logical query against a schema. backend_hint, if non-empty, forces
  // a specific backend, which must exist and support the routed operator.
  absl::StatusOr<PhysicalPlan> Plan(const core::Schema &schema,
                                    const sql::LogicalPlan &logical,
                                    const std::string &backend_hint = "") const;

private:
  // Chooses a backend that supports the operator in QueryPrivate mode, honoring
  // a name hint when given.
  absl::StatusOr<std::string>
  SelectBackend(backend::Op op, const std::string &backend_hint) const;

  const backend::BackendRegistry &registry_;
};

} // namespace opaquedb::planner

#endif // OPAQUEDB_PLANNER_PLANNER_H_
