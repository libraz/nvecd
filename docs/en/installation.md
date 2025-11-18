# Installation Guide

This guide provides detailed instructions for building and installing nvecd from source.

## Prerequisites

### System Requirements

- **Operating System**: Linux (Ubuntu 20.04+, Debian 11+) or macOS (10.15+)
- **Compiler**: C++17 compatible compiler
  - GCC 9+ (Linux)
  - Clang 10+ (macOS/Linux)
- **CMake**: Version 3.15 or later
- **Memory**: At least 1GB RAM for building, 512MB+ for running

### Required Dependencies

All dependencies are bundled in the `third_party/` directory and will be automatically fetched during build:

- **yaml-cpp**: YAML configuration parser (bundled)
- **GoogleTest**: Testing framework (bundled, only for tests)
- **spdlog**: Fast logging library (bundled)

### System Dependencies

#### Ubuntu/Debian

```bash
# Update package list
sudo apt-get update

# Install build essentials
sudo apt-get install -y \
  build-essential \
  cmake \
  git \
  pkg-config \
  libz-dev
```

#### macOS

```bash
# Install Homebrew (if not already installed)
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# Install dependencies
brew install cmake
```

---

## Building from Source

### Quick Start (Using Makefile)

The easiest way to build nvecd is using the provided Makefile:

```bash
# Clone repository
git clone https://github.com/yourusername/nvecd.git
cd nvecd

# Build (automatically configures CMake and builds in parallel)
make

# Run tests (218 tests)
make test

# View all available commands
make help
```

**Build output**: Binaries will be in `build/bin/nvecd`

### Manual Build (Using CMake Directly)

If you prefer to use CMake directly:

```bash
# Configure build (Release mode)
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build with parallel jobs
cmake --build build --parallel

# Run tests
cd build
ctest --output-on-failure
```

**Build output**: Binaries will be in `build/bin/nvecd`

---

## Build Options

### Build Types

```bash
# Debug build (with symbols, no optimization)
cmake -B build -DCMAKE_BUILD_TYPE=Debug

# Release build (optimized, no debug symbols)
cmake -B build -DCMAKE_BUILD_TYPE=Release

# RelWithDebInfo (optimized with debug symbols)
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

### Optional Features

#### Disable Tests

To skip building tests:

```bash
cmake -B build -DBUILD_TESTS=OFF
```

Or with Makefile:

```bash
make CMAKE_OPTIONS="-DBUILD_TESTS=OFF" configure
make
```

#### Enable AddressSanitizer (Memory Error Detection)

```bash
cmake -B build -DENABLE_ASAN=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
```

Or with Makefile:

```bash
make CMAKE_OPTIONS="-DENABLE_ASAN=ON" configure
make
```

#### Enable ThreadSanitizer (Race Condition Detection)

```bash
cmake -B build -DENABLE_TSAN=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
```

Or with Makefile:

```bash
make CMAKE_OPTIONS="-DENABLE_TSAN=ON" configure
make
```

**Note**: Do not enable both ASAN and TSAN simultaneously.

#### Enable Code Coverage

```bash
cmake -B build -DENABLE_COVERAGE=ON
cmake --build build --parallel

# Run tests with coverage
make coverage

# View coverage report
open build/coverage/html/index.html
```

---

## Installing Binaries

### System-wide Installation (Recommended)

Install to `/usr/local` (default location):

```bash
# Build first
make

# Install (requires sudo)
sudo make install
```

This will install:

- **Binary**: `/usr/local/bin/nvecd`
- **Config sample**: `/usr/local/etc/nvecd/config.yaml.example`
- **Documentation**: `/usr/local/share/doc/nvecd/`

### Custom Installation Location

Install to a custom directory (e.g., `/opt/nvecd`):

```bash
# Configure with custom prefix
cmake -B build -DCMAKE_INSTALL_PREFIX=/opt/nvecd

# Build and install
cmake --build build --parallel
sudo cmake --install build
```

Or with Makefile:

```bash
make PREFIX=/opt/nvecd install
```

### Uninstalling

To remove installed files:

```bash
sudo make uninstall
```

Or with CMake:

```bash
sudo cmake --build build --target uninstall
```

---

## Running Tests

nvecd includes comprehensive test coverage (218 tests).

### Using Makefile

```bash
make test
```

### Using CTest Directly

```bash
cd build
ctest --output-on-failure
```

### Running Specific Tests

```bash
# Run only event store tests
cd build
./bin/event_store_test

# Run with verbose output
./bin/event_store_test --gtest_verbose

# Run specific test case
./bin/event_store_test --gtest_filter="EventStoreTest.BasicIngest"
```

### Test Coverage

- **Event Store**: 35 tests
- **Vector Store**: 28 tests
- **Co-occurrence Index**: 22 tests
- **Similarity Search**: 31 tests
- **Configuration Parser**: 18 tests
- **Server & Handlers**: 45 tests
- **Cache System**: 24 tests
- **Client Library**: 15 tests

**Total**: 218 tests, 100% passing ✅

---

## Verifying Installation

After installation, verify the binary is accessible:

```bash
# Check server binary
nvecd --help

# Expected output:
# Usage: nvecd [OPTIONS]
#
# Options:
#   -c, --config <file>    Configuration file path
#   -h, --help             Show this help message
#   -v, --version          Show version information
```

Check version:

```bash
nvecd --version

# Expected output:
# nvecd version 0.1.0
```

---

## Running the Server

### Basic Usage

```bash
# Run with example configuration
./build/bin/nvecd -c examples/config.yaml

# Server will start and listen on 127.0.0.1:11017
```

Expected output:

```
[2025-11-18 14:30:00.123] [info] nvecd version 0.1.0 starting...
[2025-11-18 14:30:00.125] [info] Loading configuration from examples/config.yaml
[2025-11-18 14:30:00.127] [info] TCP server listening on 127.0.0.1:11017
[2025-11-18 14:30:00.128] [info] nvecd ready to accept connections
```

### Testing the Server

Connect using `nc` (netcat):

```bash
# Connect to server
nc localhost 11017

# Try commands
EVENT user1 item1 100
VECSET item1 3 0.1 0.5 0.8
SIM item1 10 fusion

# Expected responses
OK
OK
OK RESULTS 0
```

### Running as System Service (systemd)

**Note**: systemd service configuration is planned for future releases.

For now, you can run nvecd manually or use a process manager like `supervisord`.

---

## Configuration

Before running nvecd in production, create a configuration file:

```bash
# Copy example config
sudo mkdir -p /etc/nvecd
sudo cp examples/config.yaml /etc/nvecd/config.yaml

# Edit configuration
sudo nano /etc/nvecd/config.yaml
```

See [Configuration Guide](configuration.md) for detailed configuration options.

---

## Development Build

For development work with debugging enabled:

```bash
# Debug build with sanitizers
cmake -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DENABLE_ASAN=ON

cmake --build build --parallel

# Run with debug logging
./build/bin/nvecd -c examples/config.yaml --log-level debug
```

### Code Formatting

```bash
# Format all source files
make format

# Or manually
find src tests -name "*.cpp" -o -name "*.h" | xargs clang-format -i
```

### Static Analysis

```bash
# Run clang-tidy
make lint

# Or manually
clang-tidy src/**/*.cpp -- -std=c++17
```

---

## Troubleshooting

### Build Failures

**Issue**: CMake cannot find yaml-cpp

**Solution**: yaml-cpp is bundled. Make sure you have internet access during the first build (CMake will fetch dependencies).

```bash
# Clean and rebuild
make clean
make
```

**Issue**: Compiler version too old

**Solution**: Update to GCC 9+ or Clang 10+:

```bash
# Ubuntu/Debian
sudo apt-get install -y gcc-9 g++-9
export CC=gcc-9
export CXX=g++-9

# Or use Clang
sudo apt-get install -y clang-10
export CC=clang-10
export CXX=clang++-10
```

### Test Failures

**Issue**: Tests fail with sanitizer errors

**Solution**: This may indicate actual bugs. Run tests without sanitizers first:

```bash
# Build without sanitizers
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
cd build && ctest --output-on-failure
```

### Runtime Issues

**Issue**: Server fails to bind to port

**Solution**: Check if port 11017 is already in use:

```bash
# Check for process using port
lsof -i :11017

# Or use a different port in config.yaml
api:
  tcp:
    port: 12017
```

**Issue**: Permission denied for snapshot directory

**Solution**: Create directory with proper permissions:

```bash
sudo mkdir -p /var/lib/nvecd/snapshots
sudo chown $(whoami) /var/lib/nvecd/snapshots
chmod 755 /var/lib/nvecd/snapshots
```

---

## Security Notes

### Production Deployment

1. **Run as non-root user**:

```bash
# Create dedicated user
sudo useradd -r -s /bin/false nvecd

# Change ownership
sudo chown -R nvecd:nvecd /var/lib/nvecd
```

2. **Protect configuration files**:

```bash
# Secure config file
sudo chmod 600 /etc/nvecd/config.yaml
sudo chown nvecd:nvecd /etc/nvecd/config.yaml
```

3. **Use CIDR filtering**:

Configure `network.allow_cidrs` in config.yaml to restrict access:

```yaml
network:
  allow_cidrs:
    - "10.0.0.0/8"      # Private network only
    - "172.16.0.0/12"
```

4. **Monitor logs**:

```bash
# Follow logs
tail -f /var/log/nvecd/nvecd.log
```

---

## Next Steps

After successful installation:

1. **Configure nvecd**: See [Configuration Guide](configuration.md)
2. **Learn the protocol**: See [Protocol Reference](protocol.md)
3. **Use client libraries**: See [Client Library Guide](libnvecdclient.md)
4. **Set up persistence**: See [Snapshot Management](snapshot.md)
5. **Optimize performance**: See [Performance Guide](performance.md)

---

## Getting Help

- **Documentation**: [docs/en/](.)
- **Issues**: GitHub Issues
- **Examples**: [examples/](../../examples/)
