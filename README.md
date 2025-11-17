# Nvecd

**In-memory vector search engine with event-based co-occurrence tracking**

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](https://opensource.org/licenses/MIT)

## What is Nvecd?

Nvecd is a recommendation engine that learns from user behavior. Track what users do, and get instant recommendations based on patterns.

### Simple Example: "Customers who bought this also bought..."

```javascript
const net = require('net');

// Connect to nvecd
const client = net.createConnection({ port: 11017 }, () => {
  // Track user purchases
  client.write('EVENT user_alice item1 100\n');
  client.write('EVENT user_alice item2 100\n');
  client.write('EVENT user_bob item1 100\n');
  client.write('EVENT user_bob item3 100\n');

  // Get recommendations: "People who bought item1 also bought..."
  client.write('SIM item1 10 fusion\n');
});

client.on('data', (data) => {
  console.log(data.toString());
  // → item3 0.85
  // → item2 0.72
});
```

**That's it!** No ML models, no complex setup. Just track what users do and get recommendations.

## ⚠️ Alpha Development Status

This project is currently in **alpha development**.
Not recommended for production use.

## Key Features

- **Behavior-Based Recommendations** - Track user actions, get instant recommendations
- **Vector Similarity Search** - Find similar items using embeddings (optional)
- **Hybrid Fusion** - Combine user behavior + content similarity
- **Real-time Updates** - Recommendations adapt as users interact
- **Persistent Storage** - Snapshot support (DUMP commands)
- **Simple Protocol** - Text-based commands over TCP

## Quick Start

### 1. Build & Run

```bash
# Build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel

# Run server
./build/bin/nvecd -c examples/config.yaml
# → Listening on 127.0.0.1:11017
```

### 2. Try It Out

```bash
# Connect
nc localhost 11017

# Track user interactions
EVENT user1 product123 100
EVENT user1 product456 80
EVENT user2 product123 100
EVENT user2 product789 95

# Get recommendations
SIM product123 10 fusion
# → product789 0.92
# → product456 0.75
```

### 3. Test

```bash
make test
# → All 173 tests passing ✅
```

## Use Cases

- 🛒 **E-commerce** - Product recommendations
- 📰 **News/Content** - Article recommendations
- 🎵 **Music/Video** - Personalized playlists
- 📱 **Social Media** - Content feeds (TikTok-style)
- 🔍 **Search** - Semantic search with embeddings

See [**Use Cases Guide**](docs/en/use-cases.md) for detailed examples.

## Documentation

### Getting Started
- [**Installation Guide**](docs/en/installation.md) - Build and install instructions
- [**Quick Start Guide**](docs/en/development.md) - Get started in 5 minutes
- [**Use Cases**](docs/en/use-cases.md) - Real-world examples with code

### Reference
- [**Protocol Reference**](docs/en/protocol.md) - All available commands
- [**Configuration Guide**](docs/en/configuration.md) - Configuration options
- [**Snapshot Management**](docs/en/snapshot.md) - Persistence and backups

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

MIT License

## Contributing

Contributions are welcome! Please feel free to submit issues or pull requests.

## Links

- **Documentation**: [docs/en/](docs/en/)
- **Examples**: [examples/](examples/)
- **Tests**: [tests/](tests/)
