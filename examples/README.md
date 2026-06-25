# Examples

Three programs. `direct_engine` and `e2e_client` exercise the private query path
end to end over the weather table in `weather.sql` and `weather.csv`;
`crypto_bench` times the SEAL primitives the matcher uses.

## direct_engine (in-process, no network)

Drives the Engine directly: declares the weather table, loads a few rows,
encrypts a key, runs a private SELECT, and decodes the typed result.

```sh
build/dev-local/examples/direct_engine
# Amsterdam -> 1 NL 18
```

This is the shortest path that touches the SQL, crypto, storage, and PIR layers
together.

## e2e_client (over gRPC)

A standalone gRPC client. It generates a keypair, registers its public keys,
encrypts the WHERE value, calls Execute, and decrypts the result. Run it against
a node that already has data loaded.

```sh
# In one shell: load the weather table and start a node.
opaquedb --set auth.mode=none --set auth.enable_insecure=true \
  load --schema examples/weather.sql --csv examples/weather.csv
opaquedb --set auth.mode=none --set auth.enable_insecure=true run

# In another shell:
build/dev-local/examples/e2e_client \
  --target 127.0.0.1:50051 \
  --client-id myapp \
  --schema examples/weather.sql \
  --sql 'SELECT country, temperature, conditions FROM weather WHERE city = "Amsterdam"'
```

The crypto parameters and `--record-bytes` must match what the node used at load
time, since they come from the same config defaults.

## crypto_bench (microbenchmark)

Times the SEAL operations the bit-sliced scan relies on (rotation, square,
ciphertext-times-ciphertext multiply) at the default FHE parameters. The matcher
parameters were chosen against these numbers. Build in release; Debug timings are
meaningless.

```sh
build/release/examples/crypto_bench
```

## Building

The examples build by default with the rest of the tree (`OPAQUEDB_BUILD_EXAMPLES`).
After `cmake --build build/dev-local` they appear under `build/dev-local/examples/`.

They link the internal libraries and the generated protos. That is the supported
way to drive the client-side crypto and the wire contract until a separate
client SDK exists.

## Notes

- The wire contract is the `.proto` files.
- All crypto lives in `opaquedb::crypto` and `opaquedb::core`.
- The client never sends its secret key. The operator sees only ciphertexts and
  the authenticated principal.
