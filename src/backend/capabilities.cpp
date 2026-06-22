#include "opaquedb/backend/capabilities.h"

namespace opaquedb::backend {

std::string ToString(Scheme scheme) {
  switch (scheme) {
  case Scheme::kBfv:
    return "bfv";
  case Scheme::kBgv:
    return "bgv";
  case Scheme::kCkks:
    return "ckks";
  case Scheme::kFss:
    return "fss";
  }
  return "unknown";
}

std::string ToString(ServerModel model) {
  switch (model) {
  case ServerModel::kSingleServer:
    return "single_server";
  case ServerModel::kMultiServer:
    return "multi_server";
  }
  return "unknown";
}

std::string ToString(SecurityModel model) {
  switch (model) {
  case SecurityModel::kSemiHonest:
    return "semi_honest";
  case SecurityModel::kMalicious:
    return "malicious";
  }
  return "unknown";
}

std::string ToString(Privacy privacy) {
  switch (privacy) {
  case Privacy::kQueryPrivate:
    return "query_private";
  case Privacy::kDataPrivate:
    return "data_private";
  case Privacy::kFullyPrivate:
    return "fully_private";
  }
  return "unknown";
}

std::string ToString(Op op) {
  switch (op) {
  case Op::kEq:
    return "eq";
  case Op::kNe:
    return "ne";
  case Op::kIn:
    return "in";
  case Op::kPrefix:
    return "prefix";
  case Op::kRange:
    return "range";
  case Op::kSubstring:
    return "substring";
  }
  return "unknown";
}

} // namespace opaquedb::backend
