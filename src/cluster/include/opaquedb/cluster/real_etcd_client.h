#ifndef OPAQUEDB_CLUSTER_REAL_ETCD_CLIENT_H_
#define OPAQUEDB_CLUSTER_REAL_ETCD_CLIENT_H_

#include <memory>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "opaquedb/cluster/etcd_client.h"

// The real etcd-backed EtcdClient, built on etcd-cpp-apiv3's synchronous,
// core-only client. It is compiled only when the dependency is available
// (OPAQUEDB_HAVE_ETCD); otherwise the cluster runs against the fake. Its
// runtime behaviour is exercised by the multi-node docker test against a real
// etcd, not the unit tests.

namespace opaquedb::cluster {

#ifdef OPAQUEDB_HAVE_ETCD
// How this node authenticates to etcd. All fields are optional. When username
// is set, the client uses etcd password auth. When ca_cert is set, the client
// connects over TLS (client_cert/client_key add mutual TLS, tls_name overrides
// the certificate host name). TLS takes precedence if both groups are set; the
// underlying client does not combine them in one channel.
struct EtcdAuth {
  std::string username;
  std::string password;
  std::string ca_cert;
  std::string client_cert;
  std::string client_key;
  std::string tls_name;
};

// Connects to the first endpoint in the list. Leases are kept alive by a
// background renewer for as long as the client holds them, so a node that exits
// (or crashes) drops its lease and the cluster reacts.
absl::StatusOr<std::unique_ptr<EtcdClient>>
MakeRealEtcdClient(const std::vector<std::string> &endpoints,
                   const EtcdAuth &auth = {});
#endif

} // namespace opaquedb::cluster

#endif // OPAQUEDB_CLUSTER_REAL_ETCD_CLIENT_H_
