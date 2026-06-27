# OpaqueDB developer tasks.
#
# This is the single source of truth for the common build, test, lint, and
# packaging commands. CI, the release workflow, and the docs all invoke these
# targets, so a command only ever changes in one place.
#
# Run `make help` to list targets. Override the preset with PRESET=release,
# e.g. `make build PRESET=release`.

# The dev container provides vcpkg at /opt/vcpkg. ?= so an existing environment
# value (CI sets it explicitly) wins.
export VCPKG_ROOT ?= /opt/vcpkg

# Which CMake preset to configure and build. dev is Debug + -Werror; release is
# the optimized build used for packaging.
PRESET ?= dev

# Docker image coordinates. Override IMAGE to publish under your own namespace,
# e.g. `make docker-push IMAGE=youruser/opaquedb`. The tag comes from vcpkg.json
# (the version the build already checks against CMake project(VERSION)), so the
# image tag never drifts from the binary it carries.
IMAGE ?= opaquedb/opaquedb
VERSION := $(shell sed -n 's/.*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' vcpkg.json | head -1)
# Architectures for the published multi-arch image. amd64 + arm64 covers Intel
# servers and Apple Silicon.
PLATFORMS ?= linux/amd64,linux/arm64

.PHONY: help configure build test test-fast coverage hooks lint format format-check tidy package release docker-build docker-push clean all

help: ## List the available targets
	@grep -hE '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) \
	  | awk 'BEGIN{FS=":.*?## "}{printf "  \033[36m%-14s\033[0m %s\n", $$1, $$2}'

configure: ## Configure the build (PRESET=dev|release); first run builds deps
	cmake --preset $(PRESET)

build: ## Build the configured preset (PRESET=dev|release)
	cmake --build --preset $(PRESET)

test: ## Run the test suite (PRESET=dev|release; release is far faster)
	ctest --preset $(PRESET)

test-fast: ## Configure, build, and test the release preset (fast SEAL)
	$(MAKE) configure PRESET=release
	$(MAKE) build PRESET=release
	ctest --preset release

# Coverage uses the optimized dependency build (SEAL is fast) but compiles our
# own code at -O0 with gcov instrumentation, so a full run finishes in minutes,
# not the tens-of-minutes a Debug run costs. gcovr renders the report.
coverage: ## Build instrumented, run tests, and write build/coverage/coverage.html
	cmake --preset coverage
	cmake --build --preset coverage
	ctest --preset coverage
	gcovr --root . --filter 'src/' \
	  --exclude '.*_test\.cpp' \
	  --print-summary \
	  --html-details build/coverage/coverage.html \
	  --txt build/coverage/coverage.txt \
	  build/coverage
	@echo "Coverage report: build/coverage/coverage.html"

hooks: ## Enable the tracked git hooks (clang-format on commit)
	git config core.hooksPath .githooks
	@echo "git hooks enabled: .githooks (pre-commit formats staged C++)"

format: ## Reformat all C++ sources in place
	find src tests -type f \( -name '*.cpp' -o -name '*.h' \) -print0 \
	  | xargs -0 clang-format -i

format-check: ## Check formatting without modifying files
	find src tests -type f \( -name '*.cpp' -o -name '*.h' \) -print0 \
	  | xargs -0 clang-format --dry-run --Werror

tidy: ## Run clang-tidy against the dev build
	find src -type f -name '*.cpp' -print0 \
	  | xargs -0 clang-tidy -p build/dev

lint: format-check tidy ## Run all style and static-analysis checks

package: ## Produce the .deb (full) and binary-only .tar.gz from a release build
	cd build/release && cpack
	cmake --build build/release --target tarball

release: ## Full release build, then package (.deb + .tar.gz)
	$(MAKE) configure PRESET=release
	$(MAKE) build PRESET=release
	$(MAKE) package

# Docker build the runtime image for the host architecture and load it into the
# local daemon. Tags both the version and latest so docker-compose's
# `image: opaquedb/opaquedb:latest` resolves. Use this to test before pushing.
docker-build: ## Build the runtime image for the host arch (tags :$(VERSION) and :latest)
	docker build -f docker/Dockerfile \
	  -t $(IMAGE):$(VERSION) -t $(IMAGE):latest .

# Build the multi-arch image and push in one step. A multi-arch manifest cannot
# be loaded into the local daemon, so build+push is a single buildx invocation
# (run `docker login` first). Needs a buildx builder: `docker buildx create --use`.
docker-push: ## Build multi-arch and push to the registry (IMAGE=ns/name, PLATFORMS=...)
	docker buildx build -f docker/Dockerfile \
	  --platform $(PLATFORMS) \
	  -t $(IMAGE):$(VERSION) -t $(IMAGE):latest \
	  --push .

clean: ## Remove the build trees
	rm -rf build

all: configure build test ## Configure, build, and test
