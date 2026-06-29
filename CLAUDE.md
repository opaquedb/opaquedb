# CLAUDE.md

Guidance for working in this repository. Keep it current: update it when
layering, build/release commands, conventions, or step status change.

## What this is

OpaqueDB is a distributed database that answers SQL-like queries over
homomorphically encrypted data using Microsoft SEAL (BFV). It is a computational
PIR scheme. Privacy holds against a semi-honest operator with no non-collusion
assumption (single trust domain), resting on Ring-LWE. The deployed unit is a
sharded cluster of identical nodes. A cluster holds many databases, each holding
many tables; a query routes to the table named in its SQL within the database
the request selects (default "default").

## Build, test, lint

The dev container has cmake, ninja, vcpkg (`$VCPKG_ROOT=/opt/vcpkg`), and the
lint tools. All dependencies flow through vcpkg in manifest mode. The `Makefile`
is the single soCommon tasks run through the `Makefile`, which is the single source of truth for
build, test, lint, and packaging commands (CI invokes the same targets):

```sh
export VCPKG_ROOT=/opt/vcpkg   # provided by the dev container
make configure                 # first run builds dependencies via vcpkg
make build
make test
```

Run `make help` to list every target. Useful ones: `make lint` (clang-format and
clang-tidy), `make package` (release `.deb` and `.tar.gz`), and `PRESET=release`
on `configure`/`build` for an optimized build.

The dev container in `.devcontainer/` provides the C++20 toolchain and vcpkg.
The first configure is slow because vcpkg builds dependencies from source; later
builds use the binary cache.urce of truth for common tasks; CI and the docs call the same
targets. `make help` lists them.

```
make configure   # cmake --preset dev (Debug, -Werror); first run builds deps
make build       # cmake --build --preset dev
make test        # ctest --preset $(PRESET) (dev by default)
make test-fast   # configure+build+test the release preset (fast SEAL)
make coverage    # instrumented run -> build/coverage/coverage.html
make hooks       # enable .githooks (pre-commit clang-formats staged C++)
make lint        # clang-format check + clang-tidy (CI runs format only)
make package     # CPack .deb (full) + binary-only .tar.gz
```

Formatting on commit: the tracked `.githooks/pre-commit` runs clang-format over
the staged C/C++ files and restages them, so a commit always satisfies CI's
`make format-check`. Enable it once per clone with `make hooks` (sets
`core.hooksPath .githooks`); it no-ops if clang-format is absent.

Test speed: Debug links debug-SEAL, so the heavy matcher tests take 50-120s
each; the full Debug suite is over ten minutes. The release preset links the
optimized SEAL and runs the same suite in ~35s, the fast tier (`ctest -LE slow`)
in under a second. Prefer `make test PRESET=release` (or `make test-fast`) for
the inner loop; keep a Debug run for the `-Werror` / `-O0` checks. The SEAL-heavy
tests (crypto, reference backend, server, distributed) carry the `slow` ctest
label: `ctest -LE slow` skips them, `ctest -L slow` runs only them.

Coverage: `make coverage` uses the `coverage` preset (optimized deps so SEAL is
fast, first-party code at `-O0` with `--coverage` so line attribution is exact)
and renders a gcovr report. The vcpkg deps are never instrumented because they
do not link the `opaquedb_compile_options` interface target that carries the
flags. Baseline at the time this was wired: 63% lines, 79% functions, 31%
branches over `src/`; the CLI commands, `log`, and `real_etcd_client` are near
0% (no harness yet) and branch coverage is low because error/validation paths
are largely untested. The `coverage` preset passes the full suite from a clean
build; an incremental reconfigure that relinks libraries without recompiling a
test binary can leave it stale and trip spurious failures, so run `make coverage`
against a clean `build/coverage` (rm it first if you changed flags).

Override the preset with `PRESET=release` (e.g. `make build PRESET=release`). On
a fresh build dir use the gitignored `dev-local` preset (it sets the buildtrees
relocation, below). The raw `cmake --preset dev` / `cmake --build` / `ctest`
commands still work; the Makefile just wraps them.

Performance: `dev` is Debug (`-O0`), where SEAL arithmetic is ~30x slower. At
poly 16384 a small query is tens of seconds in Debug versus well under a second
warm in Release; a cold run is dominated by one-time key setup (the ~125 MB
Galois set), not the matching. Always measure real workloads with
`make build PRESET=release`; the binary is `build/release/opaquedb`.

### Versioning and release

Git-flow (AVH) drives branches: `main` is production, `develop` is integration,
work merges through `release/*` and `hotfix/*`; tags are `vX.Y.Z`. The version
lives only in CMake `project(VERSION)` (checked against `vcpkg.json` at configure
time) and flows to `OPAQUEDB_VERSION`, `--version`, and CPack. Finishing a
release (`git flow release finish X.Y.Z`) tags `vX.Y.Z` on `main`. `make
package` produces two artifacts, both named Debian-style
(`opaquedb_<version>_<arch>`): the `.deb` (CPack, full install tree: binary,
systemd unit, example config) and a binary-only `.tar.gz` (the `tarball` CMake
target, just the bare executable for a drop-in install). Pushing a `vX.Y.Z` tag
fires `.github/workflows/release.yml`, which builds, runs `make package`, and
publishes a GitHub Release with both artifacts; the release notes are the commit
messages since the previous tag. `.github/workflows/ci.yml` runs the format gate
then build/test on pushes and PRs, all through `make`; clang-tidy is not run in
CI (it doubled the job time), so run `make tidy` locally. First release: v0.1.0.

### Build environment gotcha (important)

`$VCPKG_ROOT` and `/` are on overlayfs. Building heavy vcpkg ports there makes
Ninja loop with `manifest 'build.ninja' still dirty after 100 tries` (overlayfs
copy-up gives generated files stale mtimes; not a clock problem). Fix: put
buildtrees off overlayfs via `--x-buildtrees-root`. This is wired into
`CMakeUserPresets.json` (`dev-local`, gitignored) and points at
`/workspaces/opaquedb/.vcpkg-bt` (virtiofs, big; tmpfs was too small for
boost+etcd+grpc). It is cached on `build/dev`; once a port builds it stays in the
vcpkg binary cache and will not rebuild.

## Architecture and strict layering

Each `src/<dir>` is its own CMake library target. Dependencies may only point
downward; CMake target visibility enforces the direction.

```
config, core      foundational; everything may depend on them
config        <-  log     (spdlog; the one logging entry point)
core  <-  crypto  <-  backend (interface + concrete backends under backend/<name>)
core  <-  storage
core  <-  sql  <-  planner          (sql parses CREATE TABLE into core::Schema)
config, core, storage, crypto  <-  admin   (AdminService + KeyringStore)
all of the above  <-  auth, cluster, server, client, cli
```

Rules that must hold:
- `crypto` is the only SEAL boundary. It reads FHE parameters from config; it
  does not define them. It knows nothing about SQL.
- `storage` stores opaque bytes. It knows nothing about crypto and does not link
  SEAL. Slot geometry reaches it as plain integers, never SEAL types.
- Concrete backends live under `src/backend/<name>/` (e.g. `backend/reference`),
  each its own target that links the `backend` interface; they depend on
  `crypto` and on columns handed to them, not on SQL or transport.
- Public headers live under `src/<lib>/include/opaquedb/<lib>/`. Private headers
  live beside their source and are included by relative path.
- Namespaces mirror directories (`opaquedb::crypto`, `opaquedb::backend`, ...).
- `main.cpp` only wires things together; it holds no logic.
- Generated proto code stays in the build tree and is never committed.

## Single sources of truth (DRY)

- The `.proto` files define the wire types. Nothing hand-duplicates them.
- Common build, test, lint, and packaging commands live once in the `Makefile`;
  CI and the docs invoke its targets, never raw cmake/clang commands.
- The `Config` struct (`src/config`) is the only place settings live. Resolution
  order, later wins: defaults, file, env (`OPAQUEDB_<SECTION>_<KEY>`), flags.
  Resolved once into one immutable Config and injected.
- The bit-sliced key codec (`core::key_codec`) and the SIMD plane layout
  (`core::slot_codec`) live once in `core`.
- The default config is one file, `config/opaquedb.default.toml`, embedded at
  build time (generated `default_config_data.h`); `config init`, the packaging
  example, and a DRY test all use it.
- The version is the CMake `project(VERSION)` only (see Versioning above).
- The schema lives once in the epoch manifest, read by planner, storage,
  backends. The FHE parameter set lives once in config, read by crypto.
- Management logic lives once in the `AdminService` (`src/admin`); the gRPC
  adapter and the CLI are thin clients over it.
- The user-facing schema is one `CREATE TABLE`, parsed once by
  `sql::ParseCreateTable` into `core::Schema`. The typed record layout lives once
  in `core::record_codec`.
- Logging is configured once by `log::Init`; everything writes through spdlog
  (CLI data output still goes to stdout because it is data, not a log line).
- The wire query client lives once in `client::QueryClient`.

## Error handling

Use `absl::Status` / `absl::StatusOr` at every fallible API boundary. Reserve
exceptions for programmer errors. SEAL throws on bad input; catch at the crypto
boundary and convert to a status so untrusted input never crashes a node. No
silent failures, no ad hoc error codes.

## Security requirements (hard constraints)

This is a privacy system. Treat all client-supplied bytes (ciphertexts, keys,
SQL templates, parameters) and all on-disk bytes (manifests, segments, WAL) as
untrusted.

- Validate every size and bound before use. Never index, allocate, mmap, or copy
  based on a length taken from input or a file without first checking it against
  the actual available bytes. Use checked arithmetic for size math; never let
  `count * stride` wrap.
- Validate client SEAL ciphertexts and keys against the expected parameters
  (`parms_id`, sizes) before use. Reject malformed input with a status; do not
  crash.
- No memory-safety bugs. No OOB reads/writes, no use-after-free, no unchecked
  `reinterpret_cast` over input, no raw `memcpy` without a verified length.
  Prefer spans with explicit bounds, RAII for every resource, obvious ownership.
- mmap only immutable, published, read-only epoch snapshots. Never mmap a path
  that can be written concurrently. Validate the mapped length against the
  manifest before reading records.
- Never log secret key material, plaintext query values, or auth tokens.
- Compare auth tokens in constant time. Secret files are readable only by the
  service user (0640 or stricter).
- Authentication is access control, not anonymity. The operator may learn which
  principal queried; it must never see the query value.

## Writing style

Plain direct English in code, comments, docs, logs. Short sentences. No em
dashes. No marketing words. Comments explain why, not what an obvious line does.

## Notes worth remembering

**Data model and codecs.** Schemas: `CREATE TABLE name (col TYPE [KEY|INDEX],
...)`, types INT/REAL/TEXT/JSON, exactly one match `KEY` (INT or TEXT, not REAL
or JSON; no `--` comments). JSON is a payload-only alias for TEXT: stored, packed,
and decoded byte-identically (the same 2-byte-length + bytes layout), but
`core::ParseValue` validates it as well-formed JSON at the single text -> Value
ingest boundary (rejecting malformed JSON), so a client gets back parseable JSON.
A JSON value lives in the same `std::string` variant alternative as TEXT, so the
type is known only from the schema, never from a `Value`; it can never be
searchable (`Schema::Validate` rejects it as kEq/kIndex, like REAL). A column may instead be marked `INDEX`: a secondary searchable
column (a secondary index). `core::ColumnEncoding` carries the role: `kEq` is the
one primary key (matched, shards the data, NOT stored in payload), `kIndex` is a
secondary index (matched like the key AND stored in payload so it is
returnable), `kRaw` is payload only. `core::IsSearchable` (kEq or kIndex) is the
single predicate deciding which columns get a match sub-key.
`core::EncodeKeyValue` is the single mapping of a value into the `2^key_bits`
universe: INT maps directly (range-checked), TEXT hashes via deterministic FNV,
so a TEXT match is a candidate (collisions possible but unlikely; larger key_bits
lowers the rate; client-side verification is a tracked TODO). The matcher
bit-expands one key into `key_bits` SIMD slots (`core::KeyToBits`). Storage packs
ALL searchable columns' keys side-by-side in one `match.seg` record
(`core::EncodeMatchRecord`): `SearchableCount() * ceil(key_bits/8)` bytes/row, in
schema order. A single-key table has one searchable column, so the record is
byte-identical to before. At query time the engine slices out the queried
column's sub-key by its `Schema::SearchableRank`, so the matcher stays single-key
and there is no crypto/depth change for a secondary-index query. Payload columns
(everything except `kEq`, so kRaw and every kIndex) are returned, not matched;
`core::record_codec` packs them (int/real 8 bytes, text 2-byte length + bytes) at
ingest and decodes on the client. The primary key itself is not in payload, so it
is not projectable. `load --schema f.sql --csv f.csv [--database D]` ingests (CSV
header names columns); `examples/weather.{sql,csv}` (`id` INT key; `city`,
`country`, `conditions` TEXT INDEX) is the worked example. A secondary-index query
fans out to every shard and combines exactly like a key query (data is sharded by
the primary key, so each row lives on one shard and the sum never double-counts).
KNOWN BUG (tracked, `tests/matcher_bug_repro_test.cpp`
`DISABLED_CrossShardSameValueRowsAreNotLost`): buckets are placed by `Mix(matched
sub-key) + per-key-sequence`, and the sequence counter is local to each shard's
`evaluate`. For a secondary-index query the matched sub-key is the shared index
VALUE, so two rows with the same index value on different shards both land at
bucket `Mix(value)` and `CombinePartials` sums them into one bucket: both rows are
lost (decoded as a collision). "Never double-counts" holds only when placement is
keyed by the primary key (each row on one shard). The fix is a design change
(place buckets by the primary key even on an index query, or verify rows
client-side), not a local patch.
`WHERE col IN (...)` and a flat same-column `OR` (`col=a OR col=b`) are supported
as a multi-operand union on one column (see the matcher note); `OR` across
different columns and conjunctive `col1=a AND col2=b` are not yet supported (the
planner rejects cross-column OR and AND). `<>`/`!=` is supported on a searchable
column: the matcher flips the equality indicator (see the crypto note), so it
costs exactly what `=` costs. Ordered comparisons (`<`, `>`, `BETWEEN`) and
`LIKE` parse but the plan builder still rejects them. A `SELECT` with no `WHERE`
is a full scan (see the scan note below), not run through `BuildLogicalPlan`.

**Query path and CLI.** WHERE takes a bound param (`:name`) or an inline literal
(`= "London"`). A literal is the secret value, so it is client-side sugar:
`sql::PrepareClientQuery` (in `client::QueryClient`) lifts it out, encrypts it,
and rewrites to `:v`; the server's `BuildLogicalPlan` rejects any literal, so the
operator only ever sees the parameterized template. `LIMIT n`, `OFFSET m`,
`ORDER BY col [ASC|DESC]`, `DISTINCT`, and column aliases (`col AS name`) are
optional, public, and applied CLIENT-SIDE over the decoded rows, not pushed into
the encrypted scan; the default LIMIT is `sql::kDefaultSelectLimit` (10, was 1),
still bounded by `crypto.result_buckets`. So ORDER BY / DISTINCT sort and dedup
only the rows that came back (at most `result_buckets` for a match query), not
the whole table. There is one wire client,
`client::QueryClient::Query(client_id, sql, value, backend_hint, database,
collided_out)` (table from SQL, db default "default"); the `query` and `repl`
commands and both examples use it (the old `server::DevClient` was deleted). For
`IN`/`OR` the client lifts every inline literal (rewriting to `:v0, :v1, ...`)
and encrypts one operand per value via `crypto::BuildEncryptedOperands`.
`SELECT COUNT(*)` goes through `QueryClient::QueryCount`, which sums the
per-bucket presence counts (exact even under bucket collisions) and the CLI
renders the single number. A `SELECT` with no `WHERE` is a plaintext full scan:
`QueryClient::Scan(db, table, max_rows) -> Engine::Scan` reads `payload.seg`
rows directly (no FHE, since a no-value query hides nothing) and returns up to
`min(max_rows, Engine::kScanRowCap=10000)` rows plus the true `total_rows` (so a
no-WHERE `COUNT(*)` is exact without returning rows). The Scan RPC is single-node:
`QueryService::Scan` rejects a scan with an `UnimplementedError` when the node has
shard peers (a sharded scan must fan out, a tracked follow-up). All CLI parsing is
CLI11; command files live under `src/cli/commands/`; shared helpers live in
`src/cli/util.{h,cpp}`: `LoadConfig`, `ReadFile`, `RenderRaw`/`RenderWithSchema`,
`RenderTable` (aligned table), `BuildResultTable` (decode + project/alias +
DISTINCT + ORDER BY + window, the testable pure pipeline), and `RunSelect` (the
one orchestrator both `query` and `repl` use: parse, route to Scan/QueryCount/
QueryClean, then render). `QueryClient::QueryClean` returns every clean matched
row with no window so `RunSelect` can order/dedup/window over decoded rows;
`QueryClient::Query` still applies the window for direct API/test callers. `repl`
registers keys once then runs each statement; replxx gives line editing,
persistent history (`$OPAQUEDB_HISTORY` or `~/.opaquedb_history`), and tab
completion of keywords plus cached table/column names; statements span lines and
end with `;`; meta-commands `\use <db>`, `\tables`, `\d <table>`, `\timing`,
`\help`, `\quit`. `query`/`repl` no longer take a `--schema`
file: the client fetches the table schema from the node via the `DescribeTable`
RPC (public, Query role) and decodes rows from it (caching per `db.table` in the
repl), falling back to `RenderRaw` hex if the fetch fails; only `load` still
needs `--schema` (it is the DDL that defines the table). `insert --table T
[--database D] v1 v2 ...` appends one row over the `Insert` RPC
(`QueryClient::Insert` -> `Engine::InsertRow` -> `server::InsertRowAndPublish`):
epochs are immutable, so it copies existing rows' raw bytes, appends the encoded
new row, and publishes a new version reusing schema/key_bits/geometry. Plaintext
today and single-node oriented (it does not advance a global cluster epoch, TODO).

**Crypto and matching.** Matching is bit-sliced equality. Per batch ciphertext:
`diff = query - key` (sub_plain), `sq = diff^2` (square), `eq = 1 - sq` (per-bit
XNOR), an AND across each record's bits by a rotate-and-multiply tree of depth
`log2(key_bits)`, a plaintext occupancy mask to occupied block starts (gates out
empty padding blocks, which hold key 0 and would otherwise match a query for 0),
a doubling masked-broadcast (no plaintext multiply, so no noise spent and no
cross-block bleed), then per payload plane a `multiply_plain` by the packed
payload and a per-bucket block-sum. `<>` (`Op::kNe`) reuses this whole pipeline:
right after the occupancy mask (when `sel` holds `eq` at occupied starts, 0
elsewhere) it computes `ne = start_mask - eq` (`negate` then `add_plain` the
mask), which is `1 - eq` at occupied starts and 0 elsewhere. That is plaintext
arithmetic on an already-final ciphertext, so `<>` adds NO multiplicative depth
and costs exactly what `=` costs. A `<>` usually matches many rows, so they
spread across buckets by `Mix(key)` and collide like any multi-row result (the
presence count stays exact; row retrieval drops collided buckets). The two slot rows are not merged with
rotate_columns; the matched payload lands in a bucket of one row and the client
sums the two rows' bucket. Only `1 + log2(key_bits)` ct*ct multiplies per batch
(depth 5 at key_bits 16), independent of batch size. `EncryptedQuery` carries a
primary operand plus `extra_operands`: for `IN`/same-column `OR` the matcher
computes one equality indicator per operand and SUMS them (a key equals at most
one listed value, so the indicators are disjoint and the union adds no
multiplicative depth, only one square+AND-tree per extra operand). The
disjointness assumption requires the operands to be distinct: a repeated value
(`IN (5, 5)`) would make a matching row's indicator 2, doubling its payload and
dropping it as a false collision, so `crypto::BuildEncryptedOperands` dedups the
operand list (the single point every operand flows through; covered by
`serialize_test` and `matcher_bug_repro_test`). Each operand is ONE ciphertext
(value bit-expanded and tiled across all slots).
Performance: the multiplicative depth is spent once the indicator is built, so
the matcher drops one prime (`mod_switch_to_next`) before the occupancy mask,
broadcast, and block-sum; and it runs the independent batches across worker
threads (one per core) with per-thread accumulators reduced at the end. Because
partials/results are then below the top of the modulus chain,
`DeserializeCiphertexts` takes a `require_top_level` flag (true only for the
fresh client operand). `combine`/`CombinePartials` add shard partials
plane-wise. Empty-result control: the backend appends a "presence" ciphertext
(`sum_r b_r` per bucket, the match count); the client reads it first and returns
no record for a zero-count bucket, so a no-match is an encrypted empty result and
the operator never learns whether a query matched. `SELECT COUNT(*)` reuses this
presence: the client sums it across buckets.

**Multi-result, LIMIT/OFFSET (`crypto.result_buckets`, default 16).** Rows are
partitioned into `result_buckets` buckets (a power of two, at most
`(poly/2)/key_bits`). A row that is the i-th of its key goes to bucket
`(Mix(key)+i) % buckets` (`Mix` is a splitmix64 finalizer in the reference
backend), so rows that SHARE a key land in distinct buckets and different keys
spread evenly for dense packing. Collisions can only happen between same-key
rows, and since sharding is by key all of a key's rows live on one shard, so
placement is purely local, computed at query time, no storage change. The
matcher stops the block-sum at the bucket width (`core::BucketStride`), leaving
one partial sum per bucket; bucket g is read at slot `g*stride` in each row. All
buckets share one ciphertext per plane, so the whole partition rides in one
result blob and result size does not grow with LIMIT. The matcher ALWAYS
partitions into `result_buckets` (the engine sets `query.result_buckets =
result_buckets`, independent of the query's LIMIT/OFFSET); a deployment that sets
`result_buckets = 1` opts back into single-bucket collapse (unique-key only).
`crypto::DecryptResults` decodes every bucket, per-bucket presence telling empty
(0) from clean (1) from collided (>=2, dropped and counted into the client's
`collided_buckets` out-param, surfaced as a CLI warning). LIMIT/OFFSET are then
applied CLIENT-SIDE as a row skip/take over the decoded clean rows (in bucket
order, stable across queries), so LIMIT counts ROWS not bucket slots and OFFSET
pages through matches; default is `LIMIT 1`, `OFFSET 0`. A key with up to
`result_buckets` rows returns all of them in one query; raise `result_buckets`
for more. `crypto::DecryptRecord` is the buckets=1 wrapper kept for the
single-match callers.

**FHE params and keys.** Defaults: `poly_modulus_degree=16384`,
`coeff_modulus_bits=[60,60,60,60,60,49]` (349 bits, under the 438-bit degree-16384
limit), `plain_modulus_bits=20`, `key_bits=16`. poly 16384 is REQUIRED: the
depth-5 pipeline exhausts the budget at 8192 (the 349-bit modulus leaves a
measured ~52-bit budget). Raising key_bits deepens the AND tree and needs more
primes. KNOWN BUG (tracked, `matcher_bug_repro_test`
`DISABLED_DefaultModulusIsTooShallowForKeyBits64`): config validates that
key_bits is a power of two <= 64 and fits the slot geometry, but NOT that the
modulus chain is deep enough for the matcher (depth 1 + log2(key_bits)).
key_bits 32 (depth 6) still decrypts on the default modulus; key_bits 64 (depth
7) exhausts it and decrypts to garbage with a 0 noise budget and no error. The
fix is a configure-time guard or a startup self-test that probes the budget;
until then do not raise key_bits past 32 without widening coeff_modulus_bits.
Tune against `examples/crypto_bench`; the reference backend test asserts
a positive noise budget. Galois keys are generated but only the steps the matcher
uses (`core::RequiredGaloisSteps`: +/- power-of-two AND/broadcast plus block-sum,
~17 keys), via `ClientKeyring::Generate(ctx, steps)` (the bool overload makes the
full set for benches). The reduced Galois set is ~125 MB at poly 16384 (relin
~7.5 MB, public ~1.5 MB), too big for one gRPC message: `Register` streams it in
4 MB chunks and the coordinator forwards it to peers over a streaming
`RegisterKeys` RPC. Keys are generated and serialized in SEAL's seeded
`Serializable<T>` form (the second polynomial becomes a 32-byte seed), which
roughly halves these sizes on the wire; the local accessors load the seeded
bytes back. Key distribution is the coordinator's job: `Register` stores the
keys locally and forwards them to every peer. Each node persists its keys with
`admin::FileKeyringStore`: one file per client id under `Config::KeyringDir()`
(`keyring.path`, default `<data_dir>/keys`), atomic temp-then-rename, mode
0600; re-register overwrites, so one client id maps to exactly one keyset. The
store is read-through cached, so a restart reloads keys from disk and a client
need not re-register. A client can also persist its OWN keyset (secret key included) with
`crypto::ClientKeyring::SerializeAll` / `QueryClient::SerializeKeyset` and rebuild
via `QueryClient::CreateWithKeyset` to reuse its identity across runs without a
second `Register`; that blob is the secret and never crosses the wire (the wire
form, `SerializePublic`, still omits the secret key). The client_id is a
client-supplied string (an application is the trusted client and may own many
client_ids, one per end-user context); the server binds it to the registering
auth principal.

**Storage.** Per table:
`<data_dir>/db/<database>/<table>/epochs/<version>/{match.seg,payload.seg,manifest.json}`
with a per-table `CURRENT` pointer; `match.seg` holds one packed key per
searchable column (`SearchableCount() * ceil(key_bits/8)` bytes/row; a single-key
table is one key, byte-identical to before), the manifest carries `key_bits` and
the schema, so the per-column stride and slice offsets derive from it (no new
manifest field, no extra segment files).
`Config::EpochRootFor(db,table)` resolves the root; `LocalEpochRepository` opens
it; `storage::Catalog` enumerates dbs/tables under `Config::DatabasesDir()`. On
the node `server::RepositoryManager` caches one repo per `core::TableId` and
`Engine` routes per query (the legacy single-repo `Engine::Create` overload still
serves tests). Publish writes a temp dir, renames atomically, swaps CURRENT;
epochs are immutable. A writer holds the repo's `write_mutex()` for the whole
read-modify-publish so two concurrent inserts cannot lose an update or race the
next version (insert and `AdminService::Publish` both take it). Insert republishes
the whole table each time, so it calls `EpochRepository::Prune(keep)` afterward
to drop all but the most recent epochs (CURRENT always survives a rollback
window); unlinking an epoch a reader still has mapped is safe on POSIX.
Segments are 64-byte header + fixed records, CRC32-checked;
the mmap reader validates declared geometry against the real file size before
exposing any record. The WAL is length+CRC framed and replays only its valid
prefix (torn-tail safe). The private header is `little_endian.h`, not `endian.h`
(the source dir is on the include path and system `<sys/types.h>` pulls
`<endian.h>`) — do not reintroduce that name.

**Auth, admin, and cluster security.** `Authenticator` (token/mtls/none) is
gRPC-free; the gRPC edge extracts a bearer token and the mTLS peer identity into
`auth::AuthInputs`; `RequestGate` runs auth then a PER-PRINCIPAL token-bucket
rate limit (each principal its own bucket, a global backstop at 64x bounds the
total; rate/burst from `server.rate_limit_per_second`/`_burst`) on every query
RPC, and logs `audit:`-prefixed lines naming the principal. The gate holds the
authenticator behind a mutex so `NodeServer::ReloadAuth` (SIGHUP, wired in the
`run` command) hot-swaps a reloaded token file without a restart. mtls mode maps
verified cert identities in `auth.admin_identities` to the Admin role, everyone
else to Query. `token mint` (CLI) prints a `/dev/urandom` token line. Tests that
aren't about auth set `auth.mode=none`+`enable_insecure=true`. Do NOT link
libsodium into the node: SEAL + gRPC's OpenSSL + libsodium triggers a symbol
conflict that smashes the stack guard ("stack smashing detected" inside
unrelated SEAL code), so `auth` compares high-entropy bearer tokens in constant
time with no crypto library. The CLIENT (`QueryClient`) fails closed: it needs
`client.ca_cert` (verify server) or `client.tls_cert`/`tls_key` (mutual TLS) or
an explicit `client.allow_insecure=true`, else `Create` refuses rather than send
a token over plaintext; `client.server_name` overrides the cert host name when
dialing by IP. Cluster (node-to-node) is its own trust domain
(`cluster.tls_cert`/`tls_key`/`ca_cert`); peer channels present the cluster cert
and verify peers against the cluster CA; a clustered node fails to start without
cluster mTLS or server TLS unless `cluster.allow_insecure=true` (local dev only).
`DeserializeCiphertexts` validates each client ciphertext's `parms_id` and size
before use. `src/admin`
holds the transport-agnostic `AdminService`
(publish/list/rollback/status/schema/principals) and `KeyringStore`;
`AdminGrpcService` (in server, needs gRPC helpers, gated to the Admin role) and
the in-process CLI facade share one publish path.

**Cluster and distributed query.** etcd: `cluster.etcd_username`/`etcd_password`
select password auth, `etcd_ca_cert` selects TLS (`etcd_client_cert`/`key` add
mTLS, `etcd_tls_name` overrides the cert host name); file paths read by
etcd-cpp-apiv3; TLS wins if both groups are set (the SyncClient does not combine
them); threaded via `cluster::EtcdAuth` into `MakeRealEtcdClient`. `etcd-cpp-apiv3`
is not in the pinned baseline, so it is vendored as an overlay port in
`ports/etcd-cpp-apiv3/` with `default-features:false` (core-only, no cpprestsdk).
`EtcdClient` adapter + `FakeEtcd` (virtual clock) drive logic tests;
`RealEtcdClient` compiles when present (`OPAQUEDB_HAVE_ETCD`).
`ClusterManager::Tick()` does lease/keepalive + membership + CAS election +
shard-map publish (a thread in production); `cluster.enabled` (default false)
gates joining. Query path: `Engine::EvaluateShard` (per-shard, no combine) +
`CombinePartials`; `ShardService` is the node-to-node Evaluate + RegisterKeys
RPC. The Internal service runs on its OWN listener when `cluster.listen` is set
(the Elasticsearch transport-port model), isolated from clients on a private
interface; that listener runs mutual TLS with the cluster CA, so only a
cluster-CA-signed peer can reach it, and `ShardService` also rejects any call
with no verified peer cert (`require_peer_cert`, on when the listener verifies
client certs). When `cluster.listen` is empty the Internal service shares the
client listener (single-node and tests). `QueryCoordinator` evaluates the local
shard and fans out to peers concurrently (one worker thread per peer, its own
shard on another), then combines; one slow shard no longer serializes behind the
others. `QueryService` coordinates per request: peers come from etcd membership
(`cluster.enabled`) or a static `SetQueryPeers` (tests), and `Register` forwards
keys to every peer. Peer gRPC channels are pooled (cached by address, reused
across queries instead of rebuilt), and the etcd membership/epoch the resolvers
read is cached for a 1s TTL so a burst of queries does not hit etcd per request.
The coordinator pins an epoch and `EvaluateShard` rejects a shard at a different
version, BUT only when the pinned version is non-zero: in the static-peer path
(`cluster_ == nullptr`, used by tests and any non-etcd deployment) the epoch
resolver returns 0, which disables the check, so shards at inconsistent epochs
return a torn read instead of a `FailedPrecondition` (compounds with the
"does not advance a global cluster epoch" TODO above). `combine` now rejects
shards that disagree on plane count (an inconsistent-epoch symptom) instead of
summing mismatched planes into garbage.
Peers are reached at `cluster.advertise` (the node-to-node listener's address;
falls back to `server.advertise` when the listener is shared; set it when binding
a wildcard). Data must be sharded disjoint (consistent hash) for combine to be
correct; replicating the full set would double-count. `load --shard-id N
--shard-nodes a,b,c` loads a node's shard; `docker/docker-compose.yml` uses this.
Multi-table/DB CLI: `load`/`query --database D`, `tables` (lists `db.table` via
the catalog), and `status`/`schema inspect`/`epoch list|rollback` take
`--database`/`--table`; per-table admin is in-process, the remote admin gRPC is
still single-table.

**Build/link.** Linking is memory-heavy (static SEAL+gRPC+protobuf); the top
CMake serializes link steps via a Ninja `link_pool=1` and uses `ld.gold`, or the
linker is OOM-killed (signal 9) on this ~8 GiB machine.

**Logging.** One library `src/log` (`opaquedb_log`) over spdlog; `log::Init`
reads `config.logging` (level, `format` json|text, `file` path or empty for
stdout) and sets the default logger (the node and CLI call it after loading
config, the CLI via `cli::LoadConfig`). Use `spdlog::info/warn/error`, never ad
hoc streams.
