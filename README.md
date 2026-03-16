# Nvecd

[![CI](https://github.com/libraz/nvecd/actions/workflows/ci.yml/badge.svg)](https://github.com/libraz/nvecd/actions/workflows/ci.yml)
[![codecov](https://codecov.io/gh/libraz/nvecd/branch/main/graph/badge.svg)](https://codecov.io/gh/libraz/nvecd)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](https://opensource.org/licenses/MIT)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue?logo=c%2B%2B)](https://en.cppreference.com/w/cpp/17)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS-lightgrey)](https://github.com/libraz/nvecd)

**In-memory vector search engine with event-based co-occurrence tracking and fusion search.**

## Why Nvecd?

Recommendation engines are complex — they require ML pipelines, model training, and infrastructure. Most teams just need "users who did X also did Y."

**Nvecd** solves this with an in-memory engine that combines user behavior tracking with vector similarity, delivering instant recommendations with zero ML setup.

## Quick Start

### Build & Run

```bash
# Build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel

# Run server
./build/bin/nvecd -c examples/config.yaml
# → Listening on 127.0.0.1:11017
```

### Basic Usage

```bash
# Track user purchases
nvecd-cli -p 11017 EVENT user_alice ADD product123 100
nvecd-cli -p 11017 EVENT user_alice ADD product456 80
nvecd-cli -p 11017 EVENT user_bob ADD product123 100
nvecd-cli -p 11017 EVENT user_bob ADD product789 95

# Get recommendations: "People who bought product123 also bought..."
nvecd-cli -p 11017 SIM product123 10 using=events
# (2 results, showing 2)
# 1) product789 (score: 0.92)
# 2) product456 (score: 0.75)

# Register item vectors (optional, for content-based similarity)
nvecd-cli -p 11017 VECSET product123 0.1 0.2 0.3 0.4
nvecd-cli -p 11017 VECSET product456 0.15 0.18 0.32 0.41

# Hybrid search: behavior + content similarity
nvecd-cli -p 11017 SIM product123 10 using=fusion

# Search by query vector
nvecd-cli -p 11017 SIMV 10 0.5 0.3 0.2 0.1
```

### Interactive Mode

```bash
nvecd-cli -p 11017
# nvecd> EVENT user1 ADD item1 100
# OK
# nvecd> SIM item1 10 using=fusion
# (3 results, showing 3)
# 1) item3 (score: 0.85)
# 2) item2 (score: 0.72)
# 3) item4 (score: 0.61)
# nvecd> help
```

### Monitor Cache Performance

```bash
nvecd-cli -p 11017 CACHE STATS
# hit_rate: 0.8500
# current_memory_mb: 12.45
# time_saved_ms: 15420.50
```

### Test

```bash
make test
```

## Beta Development Status

This project is currently in **beta development**.
Not recommended for production use.

## Features

- **Behavior-Based Recommendations** - Track user actions, get instant recommendations
- **Vector Similarity Search** - Find similar items using embeddings
- **Hybrid Fusion** - Combine user behavior + content similarity
- **Real-time Updates** - Recommendations adapt as users interact
- **Smart Caching** - LRU cache with LZ4 compression for fast repeated queries
- **SIMD Optimization** - AVX2/NEON acceleration for vector operations
- **Persistent Storage** - Snapshot support (DUMP commands)
- **Simple Protocol** - Text-based commands over TCP (Redis/Memcached style)
- **CLI Tool** - `nvecd-cli` with tab completion and interactive mode

## When to Use Nvecd

**Good fit:**
- Recommendation systems ("customers who bought X also bought Y")
- Content-based similarity search with embeddings
- Hybrid recommendations combining behavior + content
- Real-time personalization without ML pipeline
- Simple deployment requirements

**Not recommended:**
- Dataset doesn't fit in RAM
- Need distributed search across nodes
- Complex ML model serving

## Documentation

- [**Protocol Reference**](docs/en/protocol.md) - All available commands
- [**Configuration Guide**](docs/en/configuration.md) - Configuration options
- [**Use Cases**](docs/en/use-cases.md) - Real-world examples
- [**Snapshot Management**](docs/en/snapshot.md) - Persistence and backups
- [**Performance Tuning**](docs/en/performance.md) - Cache tuning and SIMD optimization
- [**Installation Guide**](docs/en/installation.md) - Build and install instructions
- [**Development Guide**](docs/en/development.md) - Contributing guidelines
- [**Client Library**](docs/en/libnvecdclient.md) - C/C++ client library

## Requirements

- C++17 or later (GCC 9+, Clang 10+)
- CMake 3.15+
- yaml-cpp (bundled in third_party/)

## Building from Source

```bash
# Using Makefile (recommended)
make

# Or using CMake directly
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel

# Run tests
make test
```

## License

[MIT License](LICENSE)

## Contributing

Contributions are welcome! Please feel free to submit issues or pull requests.

## Links

- **Documentation**: [docs/en/](docs/en/)
- **Examples**: [examples/](examples/)
- **Tests**: [tests/](tests/)
