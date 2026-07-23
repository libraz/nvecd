# libnvecdclient - Nvecd Client Library

## Overview

libnvecdclient is a C/C++ client library for connecting to and querying nvecd servers. It provides both a modern C++ API and a C API suitable for language bindings.

## Features

- Full support for all nvecd protocol commands
- Modern C++17 API with RAII and type safety
- C API for easy integration with other languages (Python, Node.js, etc.)
- Thread-safe connection management
- Shared library build with a CMake package target (`nvecd::client`)

## Building

The library is built automatically with nvecd:

```bash
make
```

This creates `build/lib/libnvecdclient.dylib` (macOS) or `libnvecdclient.so` (Linux).

## Installation

```bash
sudo make install
```

This installs:
- Headers to `/usr/local/include/nvecd/`
- Libraries to `/usr/local/lib/`

## C++ API

### Basic Usage

```cpp
#include <nvecdclient.h>
#include <iostream>

using namespace nvecd::client;

int main() {
    // Configure client
    ClientConfig config;
    config.host = "localhost";
    config.port = 11017;
    config.timeout_ms = 5000;

    // Create client
    NvecdClient client(config);

    // Connect (Expected is truthy on success; check with !)
    if (auto result = client.Connect(); !result) {
        std::cerr << "Connection failed: " << result.error().message() << std::endl;
        return 1;
    }

    // Register vectors
    std::vector<float> vec1 = {0.1f, 0.2f, 0.3f, 0.4f};
    if (auto result = client.Vecset("item123", vec1); !result) {
        std::cerr << "Vecset failed: " << result.error().message() << std::endl;
        return 1;
    }

    // Search similar items
    auto result = client.Sim("item123", 10, "vectors");
    if (result) {
        std::cout << "Found " << result->results.size() << " similar items:\n";
        for (const auto& item : result->results) {
            std::cout << "  " << item.id << ": " << item.score << "\n";
        }
    }

    return 0;
}
```

### Compiling with libnvecdclient

```bash
cmake -S . -B build && cmake --build build
```

Use `find_package(nvecd CONFIG REQUIRED)` and link your target with `nvecd::client`.

### Event Tracking

The `Event` signature is `Event(ctx, type, id, score)`, where `type` is
`"ADD"`, `"SET"`, or `"DEL"` (score is ignored for `DEL`).

```cpp
// Track user behavior (ADD = stream event such as a click/view)
client.Event("user_alice", "ADD", "product123", 100);  // score: 0-100
client.Event("user_alice", "ADD", "product456", 80);
client.Event("user_bob", "ADD", "product123", 100);

// State event (SET) and deletion (DEL)
client.Event("user_alice", "SET", "like:product123", 100);
client.Event("user_alice", "DEL", "like:product123");

// Get recommendations based on co-occurrence
auto result = client.Sim("product123", 10, "events");
// → Returns products that users often interact with together
```

### Hybrid Recommendations (Fusion)

```cpp
// Register vectors for content similarity
std::vector<float> product1_embedding = {...};  // From ML model
client.Vecset("product123", product1_embedding);

// Track user behavior
client.Event("user_alice", "ADD", "product123", 100);
client.Event("user_alice", "ADD", "product456", 80);

// Fusion search: content + behavior
auto result = client.Sim("product123", 10, "fusion");
// → Returns products that are BOTH similar AND co-occur with product123
```

### Vector Query Search

```cpp
// Search by raw vector (e.g., query embedding)
std::vector<float> query_vector = {0.5f, 0.3f, 0.2f, 0.1f};
auto result = client.Simv(query_vector, 10, "vectors");

if (result) {
    for (const auto& item : result->results) {
        std::cout << item.id << ": " << item.score << "\n";
    }
}
```

### Server Info

```cpp
auto info = client.Info();
if (info) {
    std::cout << "Version: " << info->version << "\n";
    std::cout << "Uptime: " << info->uptime_seconds << "s\n";
    std::cout << "Commands processed: " << info->total_commands_processed << "\n";
}
```

### Snapshot Management

```cpp
// Save snapshot
if (auto result = client.Save("snapshot.dmp"); !result) {
    std::cerr << "Save failed: " << result.error().message() << std::endl;
}

// Load snapshot
if (auto result = client.Load("snapshot.dmp"); !result) {
    std::cerr << "Load failed: " << result.error().message() << std::endl;
}
```

### Debug Mode

```cpp
// Enable debug mode (shows query timing)
client.EnableDebug();

auto result = client.Sim("item123", 10, "vectors");
// Server logs will show detailed timing information

// Disable debug mode
client.DisableDebug();
```

## C API

### Basic Usage

```c
#include <nvecdclient_c.h>
#include <stdio.h>

int main() {
    // Configure client
    NvecdClientConfig_C config = {
        .host = "localhost",
        .port = 11017,
        .timeout_ms = 5000,
        .recv_buffer_size = 65536
    };

    // Create client
    NvecdClient_C* client = nvecdclient_create(&config);
    if (!client) {
        fprintf(stderr, "Failed to create client\n");
        return 1;
    }

    // Connect
    if (nvecdclient_connect(client) != 0) {
        fprintf(stderr, "Connection failed: %s\n",
                nvecdclient_get_last_error(client));
        nvecdclient_destroy(client);
        return 1;
    }

    // Register vector
    float vector[] = {0.1f, 0.2f, 0.3f, 0.4f};
    if (nvecdclient_vecset(client, "item123", vector, 4) != 0) {
        fprintf(stderr, "Vecset failed: %s\n",
                nvecdclient_get_last_error(client));
    }

    // Search
    NvecdSimResponse_C* result = NULL;
    if (nvecdclient_sim(client, "item123", 10, "vectors", &result) == 0) {
        printf("Found %zu results:\n", result->count);
        for (size_t i = 0; i < result->count; i++) {
            printf("  %s: %f\n", result->results[i].id, result->results[i].score);
        }
        nvecdclient_free_sim_response(result);
    }

    // Cleanup
    nvecdclient_disconnect(client);
    nvecdclient_destroy(client);

    return 0;
}
```

### Compiling C Programs

```bash
gcc -o myapp myapp.c -lnvecdclient
```

### Event Tracking (C API)

```c
// Track events: nvecdclient_event(client, ctx, type, id, score)
nvecdclient_event(client, "user_alice", "ADD", "product123", 100);
nvecdclient_event(client, "user_alice", "ADD", "product456", 80);

// Search by co-occurrence
NvecdSimResponse_C* result = NULL;
nvecdclient_sim(client, "product123", 10, "events", &result);
// Process results...
nvecdclient_free_sim_response(result);
```

### Vector Query (C API)

```c
float query_vector[] = {0.5f, 0.3f, 0.2f, 0.1f};
NvecdSimResponse_C* result = NULL;

if (nvecdclient_simv(client, query_vector, 4, 10, "vectors", &result) == 0) {
    for (size_t i = 0; i < result->count; i++) {
        printf("%s: %f\n", result->results[i].id, result->results[i].score);
    }
    nvecdclient_free_sim_response(result);
}
```

## Python Bindings Example

Using ctypes with the C API:

```python
import ctypes
import os

# Load library
lib = ctypes.CDLL('/usr/local/lib/libnvecdclient.dylib')  # macOS
# lib = ctypes.CDLL('/usr/local/lib/libnvecdclient.so')   # Linux

# Define config structure
class NvecdClientConfig(ctypes.Structure):
    _fields_ = [
        ("host", ctypes.c_char_p),
        ("port", ctypes.c_uint16),
        ("timeout_ms", ctypes.c_uint32),
        ("recv_buffer_size", ctypes.c_uint32)
    ]

# Create client
config = NvecdClientConfig(
    host=b"localhost",
    port=11017,
    timeout_ms=5000,
    recv_buffer_size=65536
)

lib.nvecdclient_create.restype = ctypes.c_void_p
lib.nvecdclient_create.argtypes = [ctypes.POINTER(NvecdClientConfig)]
lib.nvecdclient_connect.argtypes = [ctypes.c_void_p]
lib.nvecdclient_vecset.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
                                    ctypes.POINTER(ctypes.c_float), ctypes.c_size_t]
lib.nvecdclient_disconnect.argtypes = [ctypes.c_void_p]
lib.nvecdclient_destroy.argtypes = [ctypes.c_void_p]
lib.nvecdclient_get_last_error.argtypes = [ctypes.c_void_p]
client = lib.nvecdclient_create(ctypes.byref(config))

# Connect
if lib.nvecdclient_connect(client) != 0:
    lib.nvecdclient_get_last_error.restype = ctypes.c_char_p
    error = lib.nvecdclient_get_last_error()
    print(f"Connection failed: {error.decode()}")
    exit(1)

# Register vector
vector = (ctypes.c_float * 4)(0.1, 0.2, 0.3, 0.4)
lib.nvecdclient_vecset(client, b"item123", vector, 4)

# Cleanup
lib.nvecdclient_disconnect(client)
lib.nvecdclient_destroy(client)
```

For production use, consider creating a proper Python wrapper class.

## Node.js Bindings Example

Using node-gyp with the C API:

```javascript
// binding.gyp
{
  "targets": [{
    "target_name": "nvecd",
    "sources": [ "src/nvecd_node.cpp" ],
    "include_dirs": [
      "/usr/local/include",
      "<!(node -p \"require('node-addon-api').include_dir\")"
    ],
    "libraries": [
      "-L/usr/local/lib",
      "-lnvecdclient"
    ],
    "cflags!": [ "-fno-exceptions" ],
    "cflags_cc!": [ "-fno-exceptions" ]
  }]
}
```

```cpp
// src/nvecd_node.cpp (simplified example)
#include <napi.h>
#include <nvecdclient_c.h>

Napi::Value Search(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    // Get parameters
    std::string id = info[0].As<Napi::String>();
    int top_k = info[1].As<Napi::Number>().Int32Value();

    // Create and connect client
    NvecdClientConfig_C config = {
        .host = "localhost",
        .port = 11017,
        .timeout_ms = 5000,
        .recv_buffer_size = 65536
    };

    NvecdClient_C* client = nvecdclient_create(&config);
    nvecdclient_connect(client);

    // Search
    NvecdSimResponse_C* result = NULL;
    nvecdclient_sim(client, id.c_str(), top_k, "vectors", &result);

    // Convert to JavaScript array
    Napi::Array jsResults = Napi::Array::New(env, result->count);
    for (size_t i = 0; i < result->count; i++) {
        Napi::Object obj = Napi::Object::New(env);
        obj.Set("id", Napi::String::New(env, result->results[i].id));
        obj.Set("score", Napi::Number::New(env, result->results[i].score));
        jsResults[i] = obj;
    }

    // Cleanup
    nvecdclient_free_sim_response(result);
    nvecdclient_disconnect(client);
    nvecdclient_destroy(client);

    return jsResults;
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
    exports.Set("search", Napi::Function::New(env, Search));
    return exports;
}

NODE_API_MODULE(nvecd, Init)
```

Usage:
```javascript
const nvecd = require('./build/Release/nvecd');

const results = nvecd.search('item123', 10);
console.log(results);
// → [{ id: 'item456', score: 0.95 }, ...]
```

## API Reference

### C++ API Classes

#### ClientConfig
- `host` - Server hostname (default: "127.0.0.1")
- `port` - Server port (default: 11017)
- `timeout_ms` - Connection timeout (default: 5000)
- `recv_buffer_size` - Receive buffer size (default: 65536)

#### SimResponse
- `results` - Vector of SimResultItem (`id`, `score`)
- `mode` - Search mode used

#### SearchOptions
Optional parameters for `Sim`/`Simv`:
- `filter` - Metadata filter expression (e.g. `"category:books"`)
- `min_score` - `std::optional<float>` minimum score threshold
- `adaptive` - `std::optional<bool>` adaptive fusion toggle (SIM fusion only)

#### ServerInfo
Field names mirror the keys emitted by the server's `INFO` command:
- `version` - Server version string
- `uptime_seconds` - Server uptime in seconds
- `total_commands_processed` - Total commands processed
- `failed_commands` - Commands that returned an error
- `total_connections` - Connections received
- `active_connections` - Currently open connections
- `event_count` - Total events stored
- `vector_count` - Total vectors stored
- `id_count` - Distinct co-occurrence IDs
- `ctx_count` - Distinct event contexts

#### Error
- `message()` - Error message string

### C++ API Methods

#### Connection Management
- `Expected<void, Error> Connect()` - Connect to server
- `void Disconnect()` - Disconnect from server
- `bool IsConnected()` - Check connection status

#### nvecd Commands
- `Expected<void, Error> Event(ctx, type, id, score)` - Track event (`type`: "ADD"/"SET"/"DEL")
- `Expected<void, Error> Vecset(id, vector)` - Register vector
- `Expected<void, Error> Metaset(id, metadata)` - Attach metadata for filtered search
- `Expected<SimResponse, Error> Sim(id, top_k, mode, options)` - Search by ID
- `Expected<SimResponse, Error> Simv(vector, top_k, mode, options)` - Search by vector

#### Admin Commands
- `Expected<void, Error> Auth(password)` - Authenticate the connection
- `Expected<ServerInfo, Error> Info()` - Get server info
- `Expected<std::string, Error> GetConfig()` - Get configuration
- `Expected<std::string, Error> Save(filepath)` - Save snapshot
- `Expected<std::string, Error> Load(filepath)` - Load snapshot
- `Expected<std::string, Error> Verify(filepath)` - Verify snapshot integrity
- `Expected<std::string, Error> DumpInfo(filepath)` - Snapshot metadata
- `Expected<std::string, Error> DumpStatus()` - Background snapshot status
- `Expected<std::string, Error> CacheStats()` - Cache statistics
- `Expected<void, Error> CacheClear()` - Clear cache
- `Expected<void, Error> CacheEnable()` - Enable cache
- `Expected<void, Error> CacheDisable()` - Disable cache
- `Expected<void, Error> EnableDebug()` - Enable debug mode
- `Expected<void, Error> DisableDebug()` - Disable debug mode

### C API Functions

See `nvecdclient_c.h` for full function documentation.

Key functions:
- `nvecdclient_create()` - Create client
- `nvecdclient_connect()` - Connect to server
- `nvecdclient_auth()` - Authenticate the connection
- `nvecdclient_event()` - Track event (`ctx, type, id, score`)
- `nvecdclient_vecset()` - Register vector
- `nvecdclient_metaset()` - Attach metadata
- `nvecdclient_sim()` / `nvecdclient_sim_ex()` - Search by ID (the `_ex` form takes `NvecdSearchOptions_C`)
- `nvecdclient_simv()` / `nvecdclient_simv_ex()` - Search by vector
- `nvecdclient_info()` - Get server info
- `nvecdclient_cache_stats()` / `_clear()` / `_enable()` / `_disable()` - Cache management
- `nvecdclient_dump_status()` - Background snapshot status
- `nvecdclient_free_*()` - Free result structures

**C API memory ownership:** functions returning a string via a `char**` out-parameter
(e.g. `nvecdclient_get_config`, `nvecdclient_dump_status`, `nvecdclient_cache_stats`)
transfer ownership to the caller, who must free it with `nvecdclient_free_string()`.
`NvecdSimResponse_C` is freed with `nvecdclient_free_sim_response()` and
`NvecdServerInfo_C` with `nvecdclient_free_server_info()`. The string returned by
`nvecdclient_get_last_error()` is owned by the client and must **not** be freed.

## Thread Safety

The NvecdClient class manages a single TCP connection and is **not thread-safe**. For multi-threaded applications, create one client instance per thread or use proper synchronization.

## Error Handling

### C++ API

Functions return `Expected<T, Error>` where:
- Success: Contains the result value
- Failure: Contains an Error with a message

Use the `operator bool()` to check for errors:

```cpp
auto result = client.Sim("item123", 10, "vectors");
if (!result) {
    // Error: result.error().message()
    std::cerr << "Error: " << result.error().message() << "\n";
} else {
    // Success: result.value()
    for (const auto& item : result->results) {
        std::cout << item.id << ": " << item.score << "\n";
    }
}
```

### C API

Functions return 0 on success, -1 on error. Use `nvecdclient_get_last_error()` to retrieve the error message.

```c
if (nvecdclient_sim(client, "item123", 10, "vectors", &result) != 0) {
    fprintf(stderr, "Error: %s\n", nvecdclient_get_last_error());
}
```

## Best Practices

### Connection Reuse

✅ **Good:** Reuse connection for multiple queries
```cpp
NvecdClient client(config);
client.Connect();

for (const auto& query : queries) {
    auto result = client.Sim(query.id, 10, "vectors");
    // Process result...
}
```

❌ **Bad:** Reconnect for each query
```cpp
for (const auto& query : queries) {
    NvecdClient client(config);
    client.Connect();          // Expensive!
    auto result = client.Sim(query.id, 10, "vectors");
    client.Disconnect();
}
```

### Error Handling

✅ **Good:** Check all errors
```cpp
if (auto result = client.Connect(); !result) {
    std::cerr << "Connection failed: " << result.error().message() << "\n";
    return 1;
}

auto result = client.Sim("item123", 10, "vectors");
if (!result) {
    std::cerr << "Search failed: " << result.error().message() << "\n";
    return 1;
}
```

❌ **Bad:** Ignore errors
```cpp
client.Connect();  // Might fail!
auto result = client.Sim("item123", 10, "vectors");
// Use result without checking...
```

### Batch Operations

✅ **Good:** Batch vector uploads
```cpp
client.Connect();
for (const auto& [id, vector] : vectors) {
    client.Vecset(id, vector);  // Reuses connection
}
```

❌ **Bad:** One connection per vector
```cpp
for (const auto& [id, vector] : vectors) {
    NvecdClient client(config);
    client.Connect();
    client.Vecset(id, vector);
    client.Disconnect();  // Expensive!
}
```

## Complete Example

```cpp
#include <nvecdclient.h>
#include <iostream>
#include <vector>

using namespace nvecd::client;

int main() {
    // Configure client
    ClientConfig config;
    config.host = "localhost";
    config.port = 11017;
    config.timeout_ms = 5000;

    // Create client
    NvecdClient client(config);

    // Connect
    if (auto result = client.Connect(); !result) {
        std::cerr << "Connection failed: " << result.error().message() << "\n";
        return 1;
    }

    std::cout << "Connected to nvecd server\n";

    // Register product vectors (from ML model)
    std::vector<std::pair<std::string, std::vector<float>>> products = {
        {"laptop_001", {0.1f, 0.2f, 0.3f, 0.4f}},
        {"laptop_002", {0.15f, 0.25f, 0.28f, 0.38f}},
        {"phone_001", {0.8f, 0.7f, 0.6f, 0.5f}}
    };

    for (const auto& [id, vector] : products) {
        if (auto result = client.Vecset(id, vector); !result) {
            std::cerr << "Vecset failed for " << id << ": "
                      << result.error().message() << "\n";
        } else {
            std::cout << "Registered: " << id << "\n";
        }
    }

    // Track user behavior: Event(ctx, type, id, score)
    client.Event("user_alice", "ADD", "laptop_001", 100);  // Purchased
    client.Event("user_alice", "ADD", "laptop_002", 80);   // Viewed
    client.Event("user_bob", "ADD", "laptop_001", 100);    // Purchased
    client.Event("user_bob", "ADD", "phone_001", 90);      // Viewed

    // Get content-based recommendations
    std::cout << "\nContent-based recommendations for laptop_001:\n";
    auto content_result = client.Sim("laptop_001", 5, "vectors");
    if (content_result) {
        for (const auto& item : content_result->results) {
            std::cout << "  " << item.id << ": " << item.score << "\n";
        }
    }

    // Get behavior-based recommendations
    std::cout << "\nBehavior-based recommendations for laptop_001:\n";
    auto behavior_result = client.Sim("laptop_001", 5, "events");
    if (behavior_result) {
        for (const auto& item : behavior_result->results) {
            std::cout << "  " << item.id << ": " << item.score << "\n";
        }
    }

    // Get hybrid recommendations (fusion)
    std::cout << "\nHybrid recommendations for laptop_001:\n";
    auto fusion_result = client.Sim("laptop_001", 5, "fusion");
    if (fusion_result) {
        for (const auto& item : fusion_result->results) {
            std::cout << "  " << item.id << ": " << item.score << "\n";
        }
    }

    // Get server info
    auto info = client.Info();
    if (info) {
        std::cout << "\nServer Info:\n";
        std::cout << "  Version: " << info->version << "\n";
        std::cout << "  Uptime: " << info->uptime_seconds << "s\n";
        std::cout << "  Commands processed: " << info->total_commands_processed << "\n";
    }

    client.Disconnect();
    return 0;
}
```

Compile and run:
```bash
g++ -std=c++17 -o example example.cpp -lnvecdclient
./example
```

## License

MIT License (see LICENSE file)
