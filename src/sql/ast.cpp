#include "opaquedb/sql/ast.h"

namespace opaquedb::sql {

std::string ToString(CompareOp op) {
  switch (op) {
  case CompareOp::kEq:
    return "=";
  case CompareOp::kNe:
    return "<>";
  case CompareOp::kLt:
    return "<";
  case CompareOp::kLe:
    return "<=";
  case CompareOp::kGt:
    return ">";
  case CompareOp::kGe:
    return ">=";
  }
  return "?";
}

} // namespace opaquedb::sql
