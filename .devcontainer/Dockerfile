# Dev container image for OpaqueDB.
# Provides the C++20 toolchain, vcpkg, Node.js, and the Claude Code CLI.
# The C++ dependencies (SEAL, gRPC, etcd-cpp-apiv3, and the rest) are built by
# vcpkg from the project vcpkg.json on the first CMake configure. They are not
# baked into this image, so the first configure is slow and later ones are fast
# thanks to the vcpkg binary cache volume mounted by devcontainer.json.

FROM mcr.microsoft.com/devcontainers/base:ubuntu-24.04

# Pin these to match the project. Set VCPKG_REF to the same commit as the
# builtin-baseline in vcpkg.json so dependency builds are reproducible.
ARG VCPKG_REF=master
ARG NODE_MAJOR=22

ENV DEBIAN_FRONTEND=noninteractive

# Build toolchain, the system packages vcpkg ports need to compile from source
# (autotools for some ports, perl and nasm for openssl), and the lint tools the
# project CI uses.
RUN apt-get update && apt-get install -y --no-install-recommends \
      autoconf \
      autoconf-archive \
      automake \
      bison \
      build-essential \
      ca-certificates \
      clang \
      clang-format \
      clang-tidy \
      clang-tools \
      cmake \
      curl \
      flex \
      gdb \
      git \
      gnupg \
      less \
      libtool \
      libtool-bin \
      m4 \
      nasm \
      ninja-build \
      perl \
      pkg-config \
      python3 \
      python3-venv \
      tar \
      unzip \
      zip \
      zlib1g-dev \
  && rm -rf /var/lib/apt/lists/*

# Node.js from NodeSource, then the Claude Code CLI. Own layer so it caches and
# rarely rebuilds.
RUN curl -fsSL "https://deb.nodesource.com/setup_${NODE_MAJOR}.x" | bash - \
  && apt-get install -y --no-install-recommends nodejs \
  && rm -rf /var/lib/apt/lists/* \
  && npm install -g @anthropic-ai/claude-code \
  && npm cache clean --force

# vcpkg in the classic location, owned by the non-root vscode user so it can
# write its downloads and build trees. The binary cache lives under the user
# home so devcontainer.json can persist it with a volume.
ENV VCPKG_ROOT=/opt/vcpkg
ENV VCPKG_DEFAULT_BINARY_CACHE=/home/vscode/.cache/vcpkg
ENV PATH=${VCPKG_ROOT}:${PATH}

RUN git clone https://github.com/microsoft/vcpkg "${VCPKG_ROOT}" \
  && git -C "${VCPKG_ROOT}" checkout "${VCPKG_REF}" \
  && "${VCPKG_ROOT}/bootstrap-vcpkg.sh" -disableMetrics \
  && mkdir -p "${VCPKG_DEFAULT_BINARY_CACHE}" \
  && chown -R vscode:vscode "${VCPKG_ROOT}" /home/vscode/.cache

USER vscode
WORKDIR /workspaces

# Build-time sanity check. Fails the build early if a core tool is missing.
RUN cmake --version && ninja --version && gcc --version && node --version \
  && npm ls -g @anthropic-ai/claude-code
