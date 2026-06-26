# Contributing

## Build and test

Common tasks live in the `Makefile` so CI and contributors run the same
commands. `make help` lists them.

```sh
export VCPKG_ROOT=/opt/vcpkg
make configure
make build
make test
```

The build must be warning-clean. The `dev` preset sets `OPAQUEDB_WERROR=ON`, so
warnings in first-party code fail the build. Run `make lint` before pushing to
match the CI style and clang-tidy checks.

## Layering

The project enforces a strict dependency direction. Lower layers may not depend
on higher ones. CMake target visibility encodes the rule, so a forbidden include
fails to link.

```
config, core            foundational, everything may depend on them
core  <-  crypto  <-  backend, backends
core  <-  storage
core  <-  sql  <-  planner
all of the above  <-  auth, admin, cluster, server
```

crypto knows nothing about SQL. storage stores opaque bytes and knows nothing
about crypto. backends depend on crypto and on the columns handed to them, not
on SQL or transport.

## Style

- Google C++ style, enforced by `.clang-format`. CI runs it in check mode.
- `.clang-tidy` runs in CI. Keep it clean.
- Errors use `absl::Status` and `absl::StatusOr` at fallible boundaries.
  Exceptions are reserved for programmer errors.
- Plain, direct English in comments and messages. Comments explain why.
- Never log secret key material, plaintext query values, or auth tokens.

## Wire and disk formats

The `.proto` files are the single source of truth for the wire format. The epoch
manifest is the single source of truth for the schema. Both carry a version
field. Bump it when the format changes.
