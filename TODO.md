# TODO

- multi-key query, or query without setting KEY
- encrypted content
- encrypted insert (client encrypts payload, sends over the existing Insert RPC)
- LIKE %test query
- other ops IN (a, b, c)
- multi thread scan in single node

## Done

- multiple result return + LIMIT/OFFSET: rows are partitioned into
  crypto.result_buckets buckets; a row that is the i-th of its key goes to
  bucket (Mix(key)+i) % buckets, so rows sharing a key spread to distinct
  buckets (collision-free until a key has more than `buckets` rows). The matcher
  stops the block-sum early to keep one partial sum per bucket and packs the
  whole partition into one result blob. The client decodes every bucket and
  applies LIMIT/OFFSET as a row skip/take: LIMIT counts rows (not buckets),
  OFFSET pages through matches, default is LIMIT 1. A key with up to
  result_buckets rows comes back in one query; raise result_buckets for more.
  Buckets with two matching rows are reported as collided and dropped, not
  returned as garbage.
- insert command (plaintext): Insert RPC + CLI `insert`, appends a row and
  republishes the epoch.
- etcd authentication: cluster.etcd_username/password and TLS (etcd_ca_cert,
  etcd_client_cert/key, etcd_tls_name).
- more info logging: server bind, client register, key distribution, query
  execute/result, cluster lease/leadership/shard-map, etcd connect.