# Open

- can we utilize etcd better? maybe some other features can help us in some problems? analyze it
- .h and .cpp files mix. for instnace storage/ some .h files in there. refactor.
- why we need epoch rollback? is that important feature?
- how we are using token mint? more explanation -> docs
- SELECT COUNT(*) / scan with no WHERE on a sharded cluster (fan out the scan; single-node only today)
- maybe remove insert and always use load? load will distr

# Done (feature/persistent-keyring)

- application implementation: client_id + key storage. Keys persist per client id
  in a file-backed keyring (admin::FileKeyringStore, one keyset per client id,
  overwrite on re-register), so a client registers once and survives restarts.
  Client-side reuse via ClientKeyring::SerializeAll / QueryClient::CreateWithKeyset.
  client_id is a client-supplied string; an application owns many client_ids.
- test client/admin api: every privileged RPC enforces its role; auth=none gives
  the Query role only and cannot reach any Admin RPC (server_test).
- update the CLI banner (post-quantum private database) + bare `opaquedb` prints help.
- where we are storing client_id / keys: file-backed keyring on each node volume,
  not etcd, not minio.
