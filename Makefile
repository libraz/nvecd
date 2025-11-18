# nvecd Makefile
# Convenience wrapper for CMake build system
# Reference: ../mygram-db/Makefile

.PHONY: help build test test-full test-sequential test-verbose clean rebuild install uninstall format format-check lint configure run

# Build directory
BUILD_DIR := build

# Install prefix (can be overridden: make PREFIX=/opt/nvecd install)
PREFIX ?= /usr/local

# clang-format command (can be overridden: make CLANG_FORMAT=clang-format-18 format)
CLANG_FORMAT ?= clang-format

# Test options (can be overridden)
TEST_JOBS ?= 4          # Parallel jobs for tests (make test TEST_JOBS=2)
TEST_VERBOSE ?= 0       # Verbose output (make test TEST_VERBOSE=1)
TEST_DEBUG ?= 0         # Debug output (make test TEST_DEBUG=1)

# Detect OS for CPU count
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
	NPROC := $(shell sysctl -n hw.ncpu)
else
	NPROC := $(shell nproc)
endif

# Default target
.DEFAULT_GOAL := build

help:
	@echo "nvecd Build System"
	@echo ""
	@echo "Available targets:"
	@echo "  make build          - Build the project (default)"
	@echo "  make test           - Run all tests (configurable with TEST_JOBS, TEST_VERBOSE, TEST_DEBUG)"
	@echo "  make test-full      - Run tests with full parallelism (j=\$$(nproc))"
	@echo "  make test-sequential - Run tests sequentially (j=1)"
	@echo "  make test-verbose   - Run tests with verbose output"
	@echo "  make clean          - Clean build directory"
	@echo "  make rebuild        - Clean and rebuild"
	@echo "  make install        - Install binaries and files"
	@echo "  make uninstall      - Uninstall binaries and files"
	@echo "  make format         - Format code with clang-format"
	@echo "  make format-check   - Check code formatting (CI)"
	@echo "  make lint           - Check code with clang-tidy"
	@echo "  make configure      - Configure CMake (for changing options)"
	@echo "  make run            - Build and run nvecd"
	@echo "  make help           - Show this help message"
	@echo ""
	@echo "Test options (override with environment variables):"
	@echo "  TEST_JOBS=N        - Number of parallel test jobs (default: 4, use 1 for sequential)"
	@echo "  TEST_VERBOSE=1     - Enable verbose test output"
	@echo "  TEST_DEBUG=1       - Enable debug test output"
	@echo ""
	@echo "Examples:"
	@echo "  make                                  # Build the project"
	@echo "  make test                             # Run tests (j=4, default)"
	@echo "  make test-full                        # Run tests with full parallelism"
	@echo "  make test-sequential                  # Run tests sequentially"
	@echo "  make test-verbose                     # Run tests with verbose output"
	@echo "  make test TEST_JOBS=2                 # Run tests with 2 parallel jobs (custom)"
	@echo "  make test TEST_JOBS=1 TEST_VERBOSE=1  # Sequential verbose tests (combined)"
	@echo "  make install                          # Install to $(PREFIX) (default: /usr/local)"
	@echo "  make PREFIX=/opt/nvecd install        # Install to custom location"
	@echo "  make CMAKE_OPTIONS=\"-DENABLE_ASAN=ON\" configure  # Enable AddressSanitizer"
	@echo "  make CMAKE_OPTIONS=\"-DBUILD_TESTS=OFF\" configure # Disable tests"

# Configure CMake
configure:
	@mkdir -p $(BUILD_DIR)
	cd $(BUILD_DIR) && cmake -DCMAKE_INSTALL_PREFIX=$(PREFIX) -DBUILD_TESTS=ON $(CMAKE_OPTIONS) ..

# Build the project
build: configure
	@echo "Building nvecd..."
	$(MAKE) -C $(BUILD_DIR) -j$(NPROC)
	@echo "Build complete!"

# Run tests with configurable options
test: build
	@echo "Running tests (jobs=$(TEST_JOBS), verbose=$(TEST_VERBOSE), debug=$(TEST_DEBUG))..."
	@if [ "$(TEST_JOBS)" = "1" ]; then \
		echo "Running tests sequentially..."; \
	fi
	@cd $(BUILD_DIR) && \
		CTEST_FLAGS="--output-on-failure --parallel $(TEST_JOBS)"; \
		if [ "$(TEST_VERBOSE)" = "1" ]; then CTEST_FLAGS="$$CTEST_FLAGS --verbose"; fi; \
		if [ "$(TEST_DEBUG)" = "1" ]; then CTEST_FLAGS="$$CTEST_FLAGS --debug"; fi; \
		ctest $$CTEST_FLAGS
	@echo "Tests complete!"

# Convenience aliases for common test scenarios
test-full:
	@$(MAKE) test TEST_JOBS=$(NPROC)

test-sequential:
	@$(MAKE) test TEST_JOBS=1

test-verbose:
	@$(MAKE) test TEST_VERBOSE=1

# Clean build directory
clean:
	@echo "Cleaning build directory..."
	rm -rf $(BUILD_DIR)
	@echo "Clean complete!"

# Rebuild from scratch
rebuild: clean build

# Install binaries and files
install: build
	@echo "Installing nvecd to $(PREFIX)..."
	$(MAKE) -C $(BUILD_DIR) install
	@echo ""
	@echo "Installation complete!"
	@echo "  Binary:        $(PREFIX)/bin/nvecd"
	@echo "  Config example: $(PREFIX)/etc/nvecd/config.yaml"
	@echo "  Documentation: $(PREFIX)/share/doc/nvecd/"
	@echo ""
	@echo "To run: $(PREFIX)/bin/nvecd -c config.yaml"

# Uninstall
uninstall:
	@echo "Uninstalling nvecd from $(PREFIX)..."
	rm -f $(PREFIX)/bin/nvecd
	rm -f $(PREFIX)/bin/nvecd-cli
	rm -rf $(PREFIX)/etc/nvecd
	rm -rf $(PREFIX)/share/doc/nvecd
	@echo "Uninstall complete!"

# Format code with clang-format
format:
	@echo "Formatting code..."
	@find src tests -type f \( -name "*.cpp" -o -name "*.h" \) ! -path "*/build/*" | xargs $(CLANG_FORMAT) -i
	@echo "Format complete!"

# Check code formatting (CI mode - fails on formatting issues)
format-check:
	@echo "Checking code formatting..."
	@find src tests -type f \( -name "*.cpp" -o -name "*.h" \) ! -path "*/build/*" | xargs $(CLANG_FORMAT) --dry-run --Werror
	@echo "Format check passed!"

# Check code with clang-tidy
lint:
	@echo "Running clang-tidy..."
	@echo "Note: Project must be built first (run 'make build')"
	@if [ ! -d "$(BUILD_DIR)" ]; then \
		echo "Error: Build directory not found. Run 'make build' first."; \
		exit 1; \
	fi
	@find src -type f -name "*.cpp" ! -path "*/build/*" | while read file; do \
		echo "Checking $$file..."; \
		clang-tidy "$$file" -p=$(BUILD_DIR) || exit 1; \
	done
	@echo "Lint complete!"

# Build and run nvecd
run: build
	@echo "Running nvecd..."
	@if [ -f "examples/config.yaml" ]; then \
		$(BUILD_DIR)/bin/nvecd -c examples/config.yaml; \
	else \
		echo "Error: examples/config.yaml not found"; \
		exit 1; \
	fi

# Quick test (useful during development)
quick-test: build
	@echo "Running quick test..."
	cd $(BUILD_DIR) && ctest --output-on-failure --parallel $(NPROC) -R "EventStore|VectorStore"
