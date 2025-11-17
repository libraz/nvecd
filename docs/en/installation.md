# Installation Guide

This guide provides detailed instructions for building and installing Nvecd.

## Prerequisites

- C++17 compatible compiler (GCC 9+, Clang 10+)
- CMake 3.15+
- yaml-cpp (bundled in third_party/)

### Installing Dependencies

#### Ubuntu/Debian

```bash
sudo apt-get update
sudo apt-get install -y cmake g++
```

#### macOS

```bash
brew install cmake
```

## Building from Source

### Using Makefile (Recommended)

```bash
# Clone repository
git clone https://github.com/yourusername/nvecd.git
cd nvecd

# Build
make

# Run tests
make test

# Clean build
make clean

# Other useful commands
make help      # Show all available commands
make rebuild   # Clean and rebuild
make format    # Format code with clang-format
```

### Using CMake Directly

```bash
# Create build directory
mkdir build && cd build

# Configure and build
cmake ..
cmake --build . --parallel

# Run tests
ctest --output-on-failure
```

## Installing Binaries

### System-wide Installation

Install to `/usr/local` (default location):

```bash
sudo make install
```

This will install:
- Binary: `/usr/local/bin/nvecd`
- Config sample: `/usr/local/etc/nvecd/config.yaml`
- Documentation: `/usr/local/share/doc/nvecd/`

### Custom Installation Location

Install to a custom directory:

```bash
make PREFIX=/opt/nvecd install
```

### Uninstalling

To remove installed files:

```bash
sudo make uninstall
```

## Running Tests

Using Makefile:

```bash
make test
```

Or using CTest directly:

```bash
cd build
ctest --output-on-failure
```

Current test coverage: **173 tests, 100% passing**

## Build Options

You can configure CMake options when using Makefile:

```bash
# Enable AddressSanitizer
make CMAKE_OPTIONS="-DENABLE_ASAN=ON" configure

# Enable ThreadSanitizer
make CMAKE_OPTIONS="-DENABLE_TSAN=ON" configure

# Disable tests
make CMAKE_OPTIONS="-DBUILD_TESTS=OFF" configure
```

## Verifying Installation

After installation, verify the binary is accessible:

```bash
# Check server binary
nvecd --help
```

## Running the Server

### Manual Execution

```bash
# Run with configuration file
nvecd -c examples/config.yaml

# Or with custom config
nvecd -c /etc/nvecd/config.yaml
```

### Running as a Service (systemd)

> ⚠️ **Not Implemented Yet** - systemd service configuration is planned for future releases.

## Security Notes

- Configuration files should be readable only by the nvecd user (mode 600)
- Run as a non-root user in production environments
- Protect dump directory with appropriate file permissions

## Next Steps

After successful installation:

1. See [Configuration Guide](configuration.md) to set up your config file
2. See [Protocol Specification](protocol.md) to learn the text protocol
3. Run `nvecd -c config.yaml` to start the server
