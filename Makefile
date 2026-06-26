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

.PHONY: help configure build test lint format format-check tidy package release clean all

help: ## List the available targets
	@grep -hE '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) \
	  | awk 'BEGIN{FS=":.*?## "}{printf "  \033[36m%-14s\033[0m %s\n", $$1, $$2}'

configure: ## Configure the build (PRESET=dev|release); first run builds deps
	cmake --preset $(PRESET)

build: ## Build the configured preset (PRESET=dev|release)
	cmake --build --preset $(PRESET)

test: ## Run the test suite
	ctest --preset dev

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

package: ## Produce the .deb and .tar.gz from an existing release build
	cd build/release && cpack

release: ## Full release build, then package (.deb + .tar.gz)
	$(MAKE) configure PRESET=release
	$(MAKE) build PRESET=release
	$(MAKE) package

clean: ## Remove the build trees
	rm -rf build

all: configure build test ## Configure, build, and test
