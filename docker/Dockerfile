# syntax=docker/dockerfile:1
#
# Multi-stage build. The builder stage compiles the dependencies with vcpkg and
# builds only the server binary (no tests, no examples). The runtime stage is
# slim and carries the stripped binary, the example config, and just the shared
# libraries it needs.
#
# Build (BuildKit required; it is the default on modern Docker):
#   docker build -f docker/Dockerfile -t opaquedb:latest .
#
# Pin VCPKG_REF to the same commit as builtin-baseline in vcpkg.json so the
# dependency builds are reproducible.

ARG UBUNTU_VERSION=24.04
ARG VCPKG_REF=930ecc42b512b564571d767f70775d284a6fa307

# ---------------------------------------------------------------------------
FROM ubuntu:${UBUNTU_VERSION} AS builder
ARG VCPKG_REF
ENV DEBIAN_FRONTEND=noninteractive

# Cache the apt lists/archives across builds; the cache mount keeps them out of
# the image layer so there is nothing to clean up afterward.
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt/lists,sharing=locked \
    apt-get update && apt-get install -y --no-install-recommends \
      autoconf autoconf-archive automake bison build-essential ca-certificates \
      clang cmake curl flex git libtool libtool-bin m4 nasm ninja-build perl \
      pkg-config python3 tar unzip zip zlib1g-dev

ENV VCPKG_ROOT=/opt/vcpkg
ENV PATH=${VCPKG_ROOT}:${PATH}
RUN git clone https://github.com/microsoft/vcpkg "${VCPKG_ROOT}" \
  && git -C "${VCPKG_ROOT}" checkout "${VCPKG_REF}" \
  && "${VCPKG_ROOT}/bootstrap-vcpkg.sh" -disableMetrics

# Persist the vcpkg binary cache between builds so the heavy ports (SEAL, grpc,
# protobuf, boost, etcd) build from source once and are reused after that.
ENV VCPKG_DEFAULT_BINARY_CACHE=/vcpkg-cache

WORKDIR /src
# Copy the manifest and build inputs. Tests and examples are intentionally not
# copied: the image build disables them, so a change there does not invalidate
# this layer and they are never compiled.
COPY vcpkg.json ./
COPY ports ports
COPY CMakeLists.txt CMakePresets.json ./
COPY cmake cmake
COPY proto proto
COPY src src

# Configure, build only the server target, install stripped. Disabling tests and
# examples roughly halves the build: no test/example binaries are compiled and
# the SEAL-heavy test suite is not run inside the image. --strip drops the
# RelWithDebInfo debug info from the installed binary (it stays large because
# SEAL, gRPC and protobuf are linked statically, but much smaller than unstripped).
RUN --mount=type=cache,target=/vcpkg-cache,sharing=locked \
    cmake --preset release \
      -D OPAQUEDB_BUILD_TESTS=OFF \
      -D OPAQUEDB_BUILD_EXAMPLES=OFF \
  && cmake --build --preset release --target opaquedb \
  && cmake --install build/release --prefix /out --strip

# ---------------------------------------------------------------------------
FROM ubuntu:${UBUNTU_VERSION} AS runtime
ARG VCPKG_REF

LABEL org.opencontainers.image.title="OpaqueDB" \
      org.opencontainers.image.description="Distributed PIR database answering SQL-like queries over homomorphically encrypted data (Microsoft SEAL, BFV)." \
      org.opencontainers.image.source="https://github.com/opaquedb/opaquedb" \
      org.opencontainers.image.licenses="Apache-2.0"

ENV DEBIAN_FRONTEND=noninteractive
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt/lists,sharing=locked \
    apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates libstdc++6

COPY --from=builder /out/bin/opaquedb /usr/bin/opaquedb
COPY --from=builder /out/share/doc/OpaqueDB/opaquedb.toml.example \
                    /etc/opaquedb/opaquedb.toml.example

# Run as an unprivileged user, matching the system package.
RUN useradd --system --no-create-home --shell /usr/sbin/nologin opaquedb \
  && mkdir -p /var/lib/opaquedb /etc/opaquedb \
  && chown opaquedb:opaquedb /var/lib/opaquedb

USER opaquedb
# Default client/server gRPC listener (see OPAQUEDB_SERVER_LISTEN).
EXPOSE 50051
ENTRYPOINT ["/usr/bin/opaquedb"]
CMD ["--version"]
