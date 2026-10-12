# Shortcuts for the CMake build; `make help` lists them.
BUILD ?= build
TESTS = nsgt_test editor_test notesync_test model_test contour_test estimate_test isolate_test

.PHONY: all build deploy test asan tsan lint format format-check clean help

all: build

$(BUILD)/CMakeCache.txt:
	cmake -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=Release $(CMAKE_ARGS)

build: $(BUILD)/CMakeCache.txt ## build the extensions and self-checks
	cmake --build $(BUILD) --parallel

deploy: $(BUILD)/CMakeCache.txt ## build and copy to REAPER's UserPlugins (restart REAPER)
	cmake --build $(BUILD) --parallel --target deploy

test: build ## run the self-checks
	@set -e; for t in $(TESTS); do echo "== $$t"; ./$(BUILD)/$$t; done

asan: ## self-checks with address and undefined-behaviour sanitizers
	$(MAKE) test BUILD=build-asan CMAKE_ARGS=-DREATRACE_SANITIZE=address,undefined

tsan: ## self-checks with the thread sanitizer
	$(MAKE) test BUILD=build-tsan CMAKE_ARGS=-DREATRACE_SANITIZE=thread

lint: $(BUILD)/CMakeCache.txt ## clang-tidy (Homebrew's llvm)
	cmake --build $(BUILD) --target lint

format: $(BUILD)/CMakeCache.txt ## clang-format the sources
	cmake --build $(BUILD) --target format

format-check: $(BUILD)/CMakeCache.txt ## report formatting differences only
	cmake --build $(BUILD) --target format-check

clean: ## remove the build directories
	rm -rf build build-asan build-tsan

help:
	@grep -E '^[a-z-]+:.*## ' $(MAKEFILE_LIST) | sed 's/:.*## /\t/' | expand -t 14
