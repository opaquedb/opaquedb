# Multi-stage build. The builder stage compiles the dependencies with vcpkg and
# builds the binary. The runtime stage is slim and carries only the binary, the
# sample config, and the runtime libraries it needs.
#
# Pin VCPKG_REF to the same commit as builtin-baseline in vcpkg.json so the
# dependency builds are reproducible.

ARG UBUNTU_VERSION=24.04
ARG VCPKG_REF=930ecc42b512b564571d767f70775d284a6fa307

# ---------------------------------------------------------------------------
FROM ubuntu:${UBUNTU_VERSION} AS builder
ARG VCPKG_REF
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      autoconf autoconf-archive automake bison build-essential ca-certificates \
      clang cmake curl flex git libtool libtool-bin m4 nasm ninja-build perl \
      pkg-config python3 tar unzip zip zlib1g-dev \
  && rm -rf /var/lib/apt/lists/*

ENV VCPKG_ROOT=/opt/vcpkg
ENV PATH=${VCPKG_ROOT}:${PATH}
RUN git clone https://github.com/microsoft/vcpkg "${VCPKG_ROOT}" \
  && git -C "${VCPKG_ROOT}" checkout "${VCPKG_REF}" \
  && "${VCPKG_ROOT}/bootstrap-vcpkg.sh" -disableMetrics

WORKDIR /src
# Copy the manifest first so the dependency build caches independently of source
# edits.
COPY vcpkg.json vcpkg.json
COPY ports ports
COPY CMakeLists.txt CMakePresets.json ./
COPY cmake cmake
COPY proto proto
COPY src src
COPY tests tests

RUN cmake --preset release \
  && cmake --build --preset release \
  && ctest --test-dir build/release --output-on-failure
RUN cmake --install build/release --prefix /out

# ---------------------------------------------------------------------------
FROM ubuntu:${UBUNTU_VERSION} AS runtime
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates libstdc++6 \
  && rm -rf /var/lib/apt/lists/*

COPY --from=builder /out/bin/opaquedb /usr/bin/opaquedb

# Run as an unprivileged user, matching the system package.
RUN useradd --system --no-create-home --shell /usr/sbin/nologin opaquedb \
  && mkdir -p /var/lib/opaquedb /etc/opaquedb \
  && chown opaquedb:opaquedb /var/lib/opaquedb

USER opaquedb
ENTRYPOINT ["/usr/bin/opaquedb"]
CMD ["--version"]
