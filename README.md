<div align="center">

<img src="assets/logo.svg" width="140" alt="OpaqueDB">

# OpaqueDB

**Run SQL over encrypted data without revealing the query.**

<p>
  <a href="LICENSE">
    <img src="https://img.shields.io/badge/License-Apache_2.0-blue.svg" alt="License">
  </a>
  <img src="https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white" alt="C++20">
  <img src="https://img.shields.io/badge/Microsoft_SEAL-4.x-0078D4?logo=microsoft" alt="Microsoft SEAL">
  <img src="https://img.shields.io/badge/BFV-FHE-6f42c1" alt="BFV Scheme">
  <img src="https://img.shields.io/badge/gRPC-Enabled-2496ED?logo=grpc" alt="gRPC">
</p>

<p>
  <a href="#what-it-is">What it is</a> &middot;
  <a href="#quickstart">Quickstart</a> &middot;
  <a href="#what-it-is-not">What it is not</a> &middot;
  <a href="#building">Building</a> &middot;
  <a href="#multi-node-cluster">Cluster</a> &middot;
  <a href="#references">References</a> &middot;
  <a href="#license">License</a> &middot;
  <a href="#donate">Donate</a>
</p>

</div>

---

## What it is

OpaqueDB answers SQL queries over data without learning what you asked for. A
client encrypts the value it is searching for under its own key. The server
evaluates the match over encrypted data and returns an encrypted result that
only the client can decrypt. The operator runs the query but never sees the
query value or the secret key.

It is a computational private information retrieval (PIR) system built on
Microsoft SEAL with the BFV scheme. The privacy guarantee is precise:

- Privacy rests on Ring-LWE, a lattice assumption with no known quantum attack.
- It holds against a semi-honest operator. There is no non-collusion assumption
  between servers, so the whole cluster is one trust domain.
- Today the system is QueryPrivate: the operator never learns the query value.
  DataPrivate mode, where the operator also never learns the stored data, is a
  later release.

The deployed unit is a sharded cluster of identical nodes. Sharding spreads the
linear scan that PIR requires across many machines.

### How matching works

The match key is binary-expanded into `key_bits` SIMD slots per record and many
records are packed into the slots of one ciphertext. For each query the server
computes, in parallel across the whole batch:

1. `diff = query - key` then `sq = diff * diff`, giving `(q-k)^2`, which is 0
   where bits match and 1 where they differ;
2. `eq = 1 - sq`, the per-bit XNOR, then an AND across each record's bits by a
   rotate-and-multiply tree of depth `log2(key_bits)`, leaving the equality
   indicator per record;
3. a plaintext-masked retrieve: multiply the indicator by the packed payload and
   sum, so only the matching record's payload survives, plus an encrypted
   presence count so a no-match query returns an empty result.

This costs only `1 + log2(key_bits)` ciphertext-times-ciphertext multiplies per
batch (the dominant FHE cost) regardless of how many records the batch holds, so
the expensive multiplies amortize across thousands of rows. The parameters were
chosen by measuring against `examples/crypto_bench`: `poly_modulus_degree`
16384 (required to fit the multiplicative depth in the noise budget), a 349-bit
coefficient modulus, and `key_bits` 16 by default (all configurable). The query
travels as a single ciphertext.

## Quickstart

Build the binary (see [Building](#building)), then declare a table, load a CSV,
serve it, and run a private query.

Declare a schema with `CREATE TABLE`. One column is the match `KEY`; the rest
are typed payload columns:

```sql
-- weather.sql
CREATE TABLE weather (
  city TEXT KEY,
  id INT,
  country TEXT,
  temperature INT,
  humidity INT,
  conditions TEXT
);
```

The CSV's header names the columns:

```
id,city,country,temperature,humidity,conditions
1,Amsterdam,NL,18,72,Cloudy
2,Tokyo,JP,27,61,Clear
```

Load it, start a node, and query by the key. The example files live in
`examples/`:

Load the example data and start a node (local insecure dev mode):

```sh
opaquedb load --schema examples/weather.sql --csv examples/weather.csv
opaquedb run --set auth.mode=none --set auth.enable_insecure=true &
```

Then open the interactive shell and run private queries by the key:

```console
$ opaquedb repl --schema examples/weather.sql
OpaqueDB shell. \help for commands, \quit to exit.
opaquedb(default)> SELECT country, temperature, conditions FROM weather WHERE city = "Amsterdam"
country=NL temperature=18 conditions=Cloudy
opaquedb(default)> SELECT country, temperature FROM weather WHERE city = "Tokyo"
country=JP temperature=27
opaquedb(default)> SELECT country FROM weather WHERE city = "Atlantis"
(no rows)
opaquedb(default)> \quit
```

A one-shot query works the same way and prints the decoded row:

```console
$ opaquedb query 'SELECT country, temperature, conditions FROM weather WHERE city = "Amsterdam"' \
    --schema examples/weather.sql
country=NL temperature=18 conditions=Cloudy
```

`"Amsterdam"` is encrypted before it leaves the client. The node scans every
row under encryption and returns only the encrypted match.

## What it is not

- Not a full SQL engine yet. Today the evaluated query is
  `SELECT <cols> FROM <table> WHERE <key> = :param`. Other operators (IN, LIKE,
  ranges, AND/OR) already parse but are not evaluated under encryption yet.
  Widening the set of operators the engine can evaluate privately is active
  work, so expect more SQL support over time.
- Not a way to skip work. PIR requires a full linear scan. Sharding improves
  latency and throughput, not total work.
- Not anonymity. Authentication is access control: OpaqueDB hides the query
  value, never who is asking. The server always learns which authenticated
  principal sent a query. You can build anonymous authentication on top of
  OpaqueDB (for example an anonymizing proxy, a mix layer, or anonymous
  credentials in your application), but the database alone will not provide it.
  Client anonymity is an extra layer you add above the database, not a property
  it gives you.
- It ships no client SDK. The gRPC `.proto` files are the wire contract. The
  `query` subcommand is a dev test client.

## Features

- `CREATE TABLE` schemas with typed columns (int, real, text) and a match key
- Private equality lookup over encrypted data via Microsoft SEAL (BFV)
- Sharded cluster with etcd leader election, membership, and query fan-out
- Versioned immutable epochs: write-ahead log, atomic publish, rollback
- Token, mTLS, and no-auth modes with constant-time token comparison
- One management facade (AdminService) shared by the gRPC API and the CLI
- Unified logging to stdout or a file, in text or JSON, set from config
- Strict layering enforced by CMake target visibility

## Building

The build uses CMake with Ninja and vcpkg in manifest mode. Dependencies are
pinned by the `builtin-baseline` in `vcpkg.json`.

```sh
export VCPKG_ROOT=/opt/vcpkg   # provided by the dev container
cmake --preset dev             # first configure builds dependencies
cmake --build build/dev
cd build/dev && ctest
```

The dev container in `.devcontainer/` provides the C++20 toolchain and vcpkg.
The first configure is slow because vcpkg builds dependencies from source; later
builds use the binary cache.

## Multi-node cluster

The Docker Compose setup in `docker/` brings up one etcd and three nodes that
elect a leader, load a disjoint shard each, and answer a cross-shard private
query:

```sh
docker compose -f docker/docker-compose.yml up --build -d
docker compose -f docker/docker-compose.yml run --rm tools \
  query 'SELECT country, temperature FROM weather WHERE city = "Santiago"' \
  --target node1:50051
```

Any node can be the target; each coordinates the query across all shards.

## References

OpaqueDB's private matching builds on the homomorphic-encryption techniques for
querying over encrypted data described in the following paper. If you use
OpaqueDB in academic work, please cite it:

```bibtex
@article{karacay2020intrusion,
  title   = {Intrusion Detection Over Encrypted Network Data},
  author  = {Kara{\c{c}}ay, Leyli and Sava{\c{s}}, Erkay and Alptekin, Halit},
  journal = {The Computer Journal},
  volume  = {63},
  number  = {4},
  pages   = {604--619},
  year    = {2020},
  publisher = {Oxford University Press},
  doi     = {10.1093/comjnl/bxz111}
}
```

## License

Apache-2.0. See [LICENSE](LICENSE).

## Donate

If OpaqueDB is useful to you, donations are welcome in Monero (XMR):

```
87MykA3xCxzAEUR8G4bQdvG6rpcuM9F8rTudD95MUX2cMSCVZPHyD45hq5uyrujKoCcx8jeYWTXC6ASemZ22AepD4SsfKWu
```
