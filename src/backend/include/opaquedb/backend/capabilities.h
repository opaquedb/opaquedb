#ifndef OPAQUEDB_BACKEND_CAPABILITIES_H_
#define OPAQUEDB_BACKEND_CAPABILITIES_H_

#include <set>
#include <string>

// Capability vocabulary for PIR backends. The planner reads these to choose a
// backend by scheme, server model, operator, and privacy mode. The enums are
// deliberately wider than the one reference backend so a SealPIR, Spiral, or
// multi-server FSS backend slots in without changing this header.

namespace opaquedb::backend {

// The cryptographic scheme a backend builds on. Extensible: a backend that does
// not use BFV simply reports its own scheme.
enum class Scheme { kBfv, kBgv, kCkks, kFss };

// Whether privacy needs one server or several non-colluding servers. OpaqueDB's
// reference backend is single server: privacy holds against one semi-honest
// operator with no non-collusion assumption.
enum class ServerModel { kSingleServer, kMultiServer };

// The adversary a backend defends against. The reference backend is
// semi-honest.
enum class SecurityModel { kSemiHonest, kMalicious };

// Which side of the query is hidden. QueryPrivate hides the query value over
// plaintext data. DataPrivate hides the data. FullyPrivate hides both. Only
// QueryPrivate is built now; the others are extension points.
enum class Privacy { kQueryPrivate, kDataPrivate, kFullyPrivate };

// The predicate operators a backend can evaluate. Only Eq is implemented now.
// The rest are declared so the planner can route to them once a backend and the
// matching column encoding support them; adding one touches only the planner
// and a backend, never transport or storage.
enum class Op { kEq, kNe, kIn, kPrefix, kRange, kSubstring };

// A backend's self-description. The registry returns these so the planner can
// select without constructing every backend.
struct BackendCapabilities {
  std::string name;
  Scheme scheme = Scheme::kBfv;
  ServerModel server_model = ServerModel::kSingleServer;
  SecurityModel security = SecurityModel::kSemiHonest;
  std::set<Op> operators;
  std::set<Privacy> privacy_modes;
  // Whether the backend can return more than one matching item. False now;
  // batch PIR with cuckoo buckets is a future backend feature, and the
  // shard/combine topology already allows it without a breaking change.
  bool supports_batch = false;

  bool Supports(Op op) const { return operators.count(op) > 0; }
  bool Supports(Privacy privacy) const {
    return privacy_modes.count(privacy) > 0;
  }
};

std::string ToString(Scheme scheme);
std::string ToString(ServerModel model);
std::string ToString(SecurityModel model);
std::string ToString(Privacy privacy);
std::string ToString(Op op);

} // namespace opaquedb::backend

#endif // OPAQUEDB_BACKEND_CAPABILITIES_H_
