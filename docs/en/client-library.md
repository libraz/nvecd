# Client library

`libnvecdclient` is a shared library that speaks the [TCP protocol](./protocol.md) over a managed connection. It ships two entry points from one binary: a C++ class returning `Expected<T, Error>`, and a C API with integer return codes suitable for FFI bindings.

## What is installed

`make install` places the library and its headers under the install prefix:

```text
<prefix>/lib/libnvecdclient.so         (libnvecdclient.dylib on macOS)
<prefix>/include/nvecd/nvecdclient.h
<prefix>/include/nvecd/nvecdclient_c.h
<prefix>/include/nvecd/utils/error.h
<prefix>/include/nvecd/utils/expected.h
<prefix>/lib/cmake/nvecd/nvecdConfig.cmake
<prefix>/lib/cmake/nvecd/nvecdTargets.cmake
```

`nvecdclient.h` includes `utils/error.h` relative to itself, so the include directory to add is `<prefix>/include/nvecd`, and the header is included by its bare name.

## Building against it

With CMake, the exported target does both:

```cmake
find_package(nvecd CONFIG REQUIRED)

add_executable(app main.cpp)
target_link_libraries(app PRIVATE nvecd::client)
```

`nvecd::client` is an alias for the `nvecdclient` target, and it carries the include directory, so no `target_include_directories` is needed.

By hand:

```bash
c++ -std=c++17 -I /usr/local/include/nvecd main.cpp -L /usr/local/lib -lnvecdclient -o app
```

For a C program, only the C header is needed and no C++ standard library flag:

```bash
cc -I /usr/local/include/nvecd main.c -L /usr/local/lib -lnvecdclient -o app
```

The library requires C++17 to build; a consumer of the C API links it as an ordinary shared library and does not.

## Connecting

Both APIs open one connection per handle and keep it open until the handle is disconnected or destroyed.

```cpp
nvecd::client::ClientConfig config;
config.host = "127.0.0.1";
config.port = 11017;
config.timeout_ms = 5000;
config.recv_buffer_size = 65536;
```

| Field | Type | Default | Meaning |
|---|---|---|---|
| `host` | `std::string` | `"127.0.0.1"` | server hostname or IPv4 address |
| `port` | `uint16_t` | `11017` | server port |
| `timeout_ms` | `uint32_t` | `5000` | send and receive timeout |
| `recv_buffer_size` | `uint32_t` | `65536` | receive buffer size |
| `unix_socket_path` | `std::string` | `""` | Unix domain socket path |

Setting `unix_socket_path` switches the transport: the client connects to that socket and ignores `host` and `port`.

```cpp
nvecd::client::ClientConfig config;
config.unix_socket_path = "/var/run/nvecd.sock";
```

`NvecdClientConfig_C` has no `unix_socket_path` field, so the C API connects over TCP only. A caller that needs a Unix socket uses the C++ API.

When the server sets `security.requirepass`, call `Auth` (or `nvecdclient_auth`) once after connecting and before the first write or admin command. Authentication belongs to the connection, so a reconnect has to repeat it.

## C++ API

`nvecd::client::NvecdClient` is move-constructible and move-assignable, not copyable. A moved-from instance stays valid and destructible: `IsConnected()` reports false, `Disconnect()` does nothing, and every command returns `ErrorCode::kClientNotConnected` rather than dereferencing the implementation it gave away.

### Lifecycle

| Signature | Returns |
|---|---|
| `explicit NvecdClient(ClientConfig config)` | — |
| `Expected<void, Error> Connect()` | success, or a connection error |
| `void Disconnect()` | — |
| `bool IsConnected() const` | whether a socket is open |

The destructor disconnects.

### Data commands

| Signature | Returns |
|---|---|
| `Expected<void, Error> Event(const std::string& ctx, const std::string& type, const std::string& id, int score = 0) const` | success or error |
| `Expected<void, Error> Vecset(const std::string& id, const std::vector<float>& vector) const` | success or error |
| `Expected<void, Error> Vecdel(const std::string& id) const` | success or error |
| `Expected<void, Error> Metaset(const std::string& id, const std::string& metadata) const` | success or error |

`type` must be exactly `"ADD"`, `"SET"` or `"DEL"`, upper case; the client rejects anything else locally with `kClientInvalidArgument` rather than sending it. For `ADD` and `SET` the score must lie in 0–100, also checked locally. `DEL` ignores `score` and omits it from the wire command.

`Metaset` takes a **string expression**, not a structured map:

```cpp
client.Metaset("item1", "category:books,active:true");
```

This is the shape TCP `METASET` accepts, and it is not the shape the HTTP `/metaset` route accepts — that route takes a JSON object under a `metadata` key. Code ported from the HTTP surface has to convert its map into `key:value,key:value` before calling this method. The expression must contain no whitespace, and an empty expression is rejected locally.

Every string argument is checked for bytes the line protocol cannot carry — NUL, CR and LF — plus whitespace where the command is whitespace-delimited. Nothing else is filtered, so the client accepts exactly the bytes the server does.

### Search

```cpp
struct SearchOptions {
  std::string filter;              // metadata filter expression
  std::optional<float> min_score;  // minimum score threshold
  std::optional<bool> adaptive;    // adaptive fusion toggle; SIM fusion only
};
```

| Signature |
|---|
| `Expected<SimResponse, Error> Sim(const std::string& id, uint32_t top_k = 10, const std::string& mode = "fusion", const SearchOptions& options = {}) const` |
| `Expected<SimResponse, Error> Simv(const std::vector<float>& vector, uint32_t top_k = 10, const std::string& mode = "vectors", const SearchOptions& options = {}) const` |

```cpp
struct SimResultItem { std::string id; float score; };
struct SimResponse { std::vector<SimResultItem> results; std::string mode; };
```

An unset `SearchOptions` field is omitted from the command, which leaves the server's default in force. `filter` uses the grammar documented in [protocol.md](./protocol.md).

`Sim` sends `using=<mode>` when `mode` is non-empty, and `adaptive=on|off` when `options.adaptive` is set. `Simv` sends neither: the wire command has no mode and no adaptive option, so its `mode` parameter is validated as a protocol token and then used only to fill `SimResponse::mode` in the returned value. Passing `"events"` to `Simv` does not produce a co-occurrence search.

An empty query vector is rejected locally with `kClientInvalidArgument`.

### Administration

| Signature | Returns |
|---|---|
| `Expected<void, Error> Auth(const std::string& password) const` | success, or an error on a wrong password |
| `Expected<ServerInfo, Error> Info() const` | parsed `INFO` counters |
| `Expected<std::string, Error> GetConfig() const` | the raw `CONFIG SHOW` block |
| `Expected<SaveResult, Error> Save(const std::string& filepath = "") const` | snapshot path and completion state |
| `Expected<std::string, Error> Load(const std::string& filepath) const` | the loaded path |
| `Expected<std::string, Error> Verify(const std::string& filepath) const` | the verification response |
| `Expected<std::string, Error> DumpInfo(const std::string& filepath) const` | the raw `DUMP INFO` block |
| `Expected<std::string, Error> DumpStatus() const` | the raw `DUMP STATUS` block |
| `Expected<std::string, Error> CacheStats() const` | the raw `CACHE STATS` block |
| `Expected<void, Error> CacheClear() const` | success or error |
| `Expected<void, Error> CacheEnable() const` | success or error |
| `Expected<void, Error> CacheDisable() const` | success or error |
| `Expected<void, Error> EnableDebug() const` | success or error |
| `Expected<void, Error> DisableDebug() const` | success or error |
| `Expected<std::string, Error> SendCommand(const std::string& command) const` | the raw response line or block |

`ServerInfo` mirrors the `INFO` keys:

```cpp
struct ServerInfo {
  std::string version;
  uint64_t uptime_seconds;
  uint64_t total_commands_processed;
  uint64_t failed_commands;
  uint64_t total_connections;      // INFO key: total_connections_received
  uint64_t active_connections;
  uint64_t event_count;
  uint64_t vector_count;
  uint64_t id_count;
  uint64_t ctx_count;
};
```

`SaveResult` distinguishes the two success replies, which are two different durability guarantees:

```cpp
struct SaveResult {
  std::string filepath;   // path the server reported
  bool completed;         // true when the file is already complete and loadable
};
```

Under the default `snapshot.mode: fork`, `completed` is false and `filepath` is not readable yet — it may be missing or still hold the previous snapshot. Poll `DumpStatus()` until the writer finishes before copying or loading it. Under `snapshot.mode: lock`, `completed` is true and the file is ready when `Save` returns.

`SendCommand` is the escape hatch for commands the class does not wrap, such as `SET`, `GET` and `SHOW VARIABLES`. It returns the response with the trailing CRLF stripped and does not classify it, so a caller checks for an `ERROR` prefix itself.

`EnableDebug` and `DisableDebug` toggle the connection's debug mode, which appends a `# DEBUG` block to every subsequent `SIM` and `SIMV` response. The parsers in `Sim` and `Simv` read the leading result set and stop, so the block is discarded rather than mis-parsed.

### Error handling

Every method returns `nvecd::utils::Expected<T, nvecd::utils::Error>`. It is contextually convertible to `bool`, `*result` and `result->` reach the value, and `result.error()` reaches the error.

```cpp
auto result = client.Sim("item1", 10, "fusion");
if (!result) {
  std::cerr << result.error().message() << '\n';        // human-readable text
  std::cerr << static_cast<int>(result.error().code()); // numeric ErrorCode
  return;
}
for (const auto& item : result->results) {
  std::cout << item.id << ' ' << item.score << '\n';
}
```

`Error::to_string()` renders both together, in the form `[Connection failed (7001)] Connection failed: Connection refused`.

Three error sources are distinguishable by code:

| Range | Source |
|---|---|
| `kClientInvalidArgument` (7009) | the client rejected the arguments without sending anything |
| `kClientNotConnected` (7000), `kClientConnectionFailed` (7001), `kClientSendFailed` (7002), `kClientReceiveFailed` (7003), `kClientTimeout` (7005), `kClientConnectionClosed` (7008) | transport |
| `kClientServerError` (7010) | the server answered `ERROR`; the message is the server's text |
| `kClientProtocolError` (7011) | the response did not have the expected shape |

A server-side error keeps the connection usable; a transport error does not, and the client disconnects itself.

### A complete example

```cpp
#include <iostream>
#include <vector>

#include <nvecdclient.h>

int main() {
  nvecd::client::ClientConfig config;
  config.host = "127.0.0.1";
  config.port = 11017;

  nvecd::client::NvecdClient client(config);
  auto connected = client.Connect();
  if (!connected) {
    std::cerr << connected.error().message() << '\n';
    return 1;
  }
  if (auto authed = client.Auth("s3cret"); !authed) {
    std::cerr << authed.error().message() << '\n';
    return 1;
  }

  const std::vector<float> vector = {0.1F, 0.2F, 0.3F, 0.4F};
  if (auto stored = client.Vecset("item1", vector); !stored) {
    std::cerr << stored.error().message() << '\n';
    return 1;
  }
  client.Metaset("item1", "category:books,active:true");
  client.Event("user_alice", "ADD", "item1", 90);

  nvecd::client::SearchOptions options;
  options.filter = "category:books";
  options.min_score = 0.5F;
  auto results = client.Sim("item1", 5, "vectors", options);
  if (!results) {
    std::cerr << results.error().message() << '\n';
    return 1;
  }
  for (const auto& item : results->results) {
    std::cout << item.id << ' ' << item.score << '\n';
  }
  return 0;
}
```

## C API

`nvecdclient_c.h` wraps the same implementation behind an opaque `NvecdClient_C*`. It is the surface FFI bindings use.

Every function returns `0` on success and `-1` on failure, except `nvecdclient_save`, which also returns `1`, and the query and free functions noted below. No function throws across the boundary: an unexpected exception is caught at the wrapper and turned into the same failure code.

### Lifecycle

```c
NvecdClient_C* nvecdclient_create(const NvecdClientConfig_C* config);
void           nvecdclient_destroy(NvecdClient_C* client);
int            nvecdclient_connect(NvecdClient_C* client);
void           nvecdclient_disconnect(NvecdClient_C* client);
int            nvecdclient_is_connected(const NvecdClient_C* client);
```

```c
typedef struct {
  const char* host;
  uint16_t    port;
  uint32_t    timeout_ms;
  uint32_t    recv_buffer_size;
} NvecdClientConfig_C;
```

`nvecdclient_create` returns `NULL` when `config` is `NULL` or allocation fails. Each field falls back to its default when it is `NULL` or zero: `"127.0.0.1"`, `11017`, `5000`, `65536`. The struct is copied, so the caller may free it immediately. `nvecdclient_is_connected` returns `1` or `0`.

The handle owns the connection; `nvecdclient_destroy` disconnects and frees it. Passing `NULL` to any function returns the failure code rather than crashing, and `nvecdclient_destroy(NULL)` is a no-op.

### Data commands

```c
int nvecdclient_event(NvecdClient_C* client, const char* ctx, const char* type,
                      const char* id, int score);
int nvecdclient_vecset(NvecdClient_C* client, const char* id,
                       const float* vector, size_t dimension);
int nvecdclient_vecdel(NvecdClient_C* client, const char* id);
int nvecdclient_metaset(NvecdClient_C* client, const char* id, const char* metadata);
```

`type` is `"ADD"`, `"SET"` or `"DEL"`; `score` is ignored for `"DEL"`. `vector` points at `dimension` floats and is copied before the call returns.

`metadata` is the same string expression the C++ `Metaset` takes — `"category:books,active:true"` — not JSON. A binding that mirrors the HTTP `/metaset` route's object argument has to flatten it first.

### Search

```c
typedef struct {
  const char* filter;      /* NULL or "" = no filter */
  float       min_score;
  int         has_min_score;
  int         adaptive;
  int         has_adaptive;
} NvecdSearchOptions_C;

typedef struct { char* id; float score; } NvecdSimResultItem_C;

typedef struct {
  NvecdSimResultItem_C* results;
  size_t                count;
  char*                 mode;
} NvecdSimResponse_C;

int nvecdclient_sim(NvecdClient_C* client, const char* id, uint32_t top_k,
                    const char* mode, NvecdSimResponse_C** result);
int nvecdclient_sim_ex(NvecdClient_C* client, const char* id, uint32_t top_k,
                       const char* mode, const NvecdSearchOptions_C* options,
                       NvecdSimResponse_C** result);
int nvecdclient_simv(NvecdClient_C* client, const float* vector, size_t dimension,
                     uint32_t top_k, const char* mode, NvecdSimResponse_C** result);
int nvecdclient_simv_ex(NvecdClient_C* client, const float* vector, size_t dimension,
                        uint32_t top_k, const char* mode,
                        const NvecdSearchOptions_C* options,
                        NvecdSimResponse_C** result);
```

A `NULL` `mode` means `"fusion"` for the two `sim` functions and `"vectors"` for the two `simv` functions. The `_ex` variants add filtering; `options` may be `NULL`, and `min_score` and `adaptive` are read only when their `has_` companion is non-zero. `adaptive` reaches the wire only from `sim_ex`, because `SIMV` has no adaptive option.

On success `*result` receives a heap-allocated response the caller owns. Free it with `nvecdclient_free_sim_response`, which frees each `id`, the `results` array, `mode` and the response itself in one call; freeing any member individually is a double free. On failure `*result` is untouched.

### Administration

```c
int nvecdclient_auth(NvecdClient_C* client, const char* password);
int nvecdclient_info(NvecdClient_C* client, NvecdServerInfo_C** info);
int nvecdclient_get_config(NvecdClient_C* client, char** config_str);
int nvecdclient_save(NvecdClient_C* client, const char* filepath, char** saved_path);
int nvecdclient_load(NvecdClient_C* client, const char* filepath, char** loaded_path);
int nvecdclient_verify(NvecdClient_C* client, const char* filepath, char** result_str);
int nvecdclient_dump_info(NvecdClient_C* client, const char* filepath, char** info_str);
int nvecdclient_dump_status(NvecdClient_C* client, char** status_str);
int nvecdclient_cache_stats(NvecdClient_C* client, char** stats_str);
int nvecdclient_cache_clear(NvecdClient_C* client);
int nvecdclient_cache_enable(NvecdClient_C* client);
int nvecdclient_cache_disable(NvecdClient_C* client);
int nvecdclient_debug_on(NvecdClient_C* client);
int nvecdclient_debug_off(NvecdClient_C* client);
```

`nvecdclient_save` is the one function with three outcomes:

| Return | Meaning |
|---|---|
| `0` | the snapshot is complete and loadable |
| `1` | a background writer was started; the file is not ready yet |
| `-1` | the save failed |

`*saved_path` is written for both success codes. A `1` means the caller polls `nvecdclient_dump_status` before touching the file. Treating `!= 0` as failure loses a successful fork-mode save; treating `>= 0` as "file ready" hands a backup script a path that does not exist yet.

`filepath` may be `NULL` for `nvecdclient_save`, which uses the server's configured default name. It is required for `load`, `verify` and `dump_info`.

### Ownership

| Producer | Free with |
|---|---|
| `nvecdclient_create` | `nvecdclient_destroy` |
| `nvecdclient_sim`, `nvecdclient_sim_ex`, `nvecdclient_simv`, `nvecdclient_simv_ex` | `nvecdclient_free_sim_response` |
| `nvecdclient_info` | `nvecdclient_free_server_info` |
| `nvecdclient_get_config`, `nvecdclient_save`, `nvecdclient_load`, `nvecdclient_verify`, `nvecdclient_dump_info`, `nvecdclient_dump_status`, `nvecdclient_cache_stats` | `nvecdclient_free_string` |
| `nvecdclient_get_last_error` | do not free |

```c
void nvecdclient_free_sim_response(NvecdSimResponse_C* result);
void nvecdclient_free_server_info(NvecdServerInfo_C* info);
void nvecdclient_free_string(char* str);
```

All three accept `NULL` and do nothing with it. `nvecdclient_free_server_info` frees the `version` string and the struct.

### Error handling

```c
const char* nvecdclient_get_last_error(const NvecdClient_C* client);
```

The function takes the client handle. Calling it with no argument, or without declaring its signature to an FFI layer, is the most common way to read the wrong memory.

The returned buffer belongs to the calling thread and stays valid until that thread calls the function again; other threads using the same handle never invalidate it. Do not free it, and copy it if it has to outlive the next call. With a `NULL` handle it returns `"Invalid client handle"`. The message is the `Error::to_string()` rendering, so it carries the code:

```text
[Connection failed (7001)] Connection failed: Connection refused
```

A message is recorded only on failure, so read it immediately after a `-1`.

### A complete example

```c
#include <stdio.h>
#include <nvecdclient_c.h>

int main(void) {
  NvecdClientConfig_C config = {0};
  config.host = "127.0.0.1";
  config.port = 11017;

  NvecdClient_C* client = nvecdclient_create(&config);
  if (client == NULL) {
    fprintf(stderr, "client creation failed\n");
    return 1;
  }

  if (nvecdclient_connect(client) != 0) {
    fprintf(stderr, "%s\n", nvecdclient_get_last_error(client));
    nvecdclient_destroy(client);
    return 1;
  }

  const float vector[4] = {0.1f, 0.2f, 0.3f, 0.4f};
  if (nvecdclient_vecset(client, "item1", vector, 4) != 0) {
    fprintf(stderr, "%s\n", nvecdclient_get_last_error(client));
  }
  nvecdclient_metaset(client, "item1", "category:books,active:true");

  NvecdSearchOptions_C options = {0};
  options.filter = "category:books";
  options.min_score = 0.5f;
  options.has_min_score = 1;

  NvecdSimResponse_C* result = NULL;
  if (nvecdclient_sim_ex(client, "item1", 5, "vectors", &options, &result) == 0) {
    for (size_t i = 0; i < result->count; ++i) {
      printf("%s %f\n", result->results[i].id, result->results[i].score);
    }
    nvecdclient_free_sim_response(result);
  } else {
    fprintf(stderr, "%s\n", nvecdclient_get_last_error(client));
  }

  nvecdclient_disconnect(client);
  nvecdclient_destroy(client);
  return 0;
}
```

## Thread safety

One handle may be used from several threads at once, on both APIs. Commands on a handle are serialized internally, and the C handle's error slot is synchronized, so no call observes a torn or freed error message.

Serialized is not parallel: a slow command delays the ones queued behind it. Throughput across threads comes from several handles, each with its own connection, not from sharing one.

Handle creation and destruction are not synchronized. No call may be in flight when `nvecdclient_destroy` runs, and on the C++ side no call may be in flight while the object is moved from or destroyed.

## Connection reuse

Connecting costs a TCP handshake and, when `security.requirepass` is set, an `AUTH` round trip. A handle held open across requests pays neither again, so a long-lived handle — or a small pool of them — is the shape to build on, rather than connect-per-request.

`IsConnected()` reports the local socket state, not reachability: a server that has gone away since the last command is discovered by the next command failing with a transport error. Reconnecting means `Connect()` again on the same handle, followed by `Auth` if the server requires it.

A transport failure disconnects the handle, so a retry loop calls `Connect()` before retrying the command. A server-side `ERROR` leaves the connection usable and needs no reconnect.

## Python binding with ctypes

The C API is callable from `ctypes` directly. Declaring `argtypes` and `restype` for every function used is not optional on 64-bit platforms: without them `ctypes` assumes `int` returns and truncates pointers.

```python
import ctypes

lib = ctypes.CDLL("/usr/local/lib/libnvecdclient.so")  # .dylib on macOS


class NvecdClientConfig(ctypes.Structure):
    _fields_ = [
        ("host", ctypes.c_char_p),
        ("port", ctypes.c_uint16),
        ("timeout_ms", ctypes.c_uint32),
        ("recv_buffer_size", ctypes.c_uint32),
    ]


class NvecdSimResultItem(ctypes.Structure):
    _fields_ = [("id", ctypes.c_char_p), ("score", ctypes.c_float)]


class NvecdSimResponse(ctypes.Structure):
    _fields_ = [
        ("results", ctypes.POINTER(NvecdSimResultItem)),
        ("count", ctypes.c_size_t),
        ("mode", ctypes.c_char_p),
    ]


lib.nvecdclient_create.restype = ctypes.c_void_p
lib.nvecdclient_create.argtypes = [ctypes.POINTER(NvecdClientConfig)]
lib.nvecdclient_destroy.argtypes = [ctypes.c_void_p]
lib.nvecdclient_connect.argtypes = [ctypes.c_void_p]
lib.nvecdclient_disconnect.argtypes = [ctypes.c_void_p]
lib.nvecdclient_auth.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
lib.nvecdclient_vecset.argtypes = [
    ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_float), ctypes.c_size_t
]
lib.nvecdclient_metaset.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
lib.nvecdclient_sim.argtypes = [
    ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint32, ctypes.c_char_p,
    ctypes.POINTER(ctypes.POINTER(NvecdSimResponse)),
]
lib.nvecdclient_free_sim_response.argtypes = [ctypes.POINTER(NvecdSimResponse)]
lib.nvecdclient_get_last_error.argtypes = [ctypes.c_void_p]
lib.nvecdclient_get_last_error.restype = ctypes.c_char_p

config = NvecdClientConfig(
    host=b"127.0.0.1", port=11017, timeout_ms=5000, recv_buffer_size=65536
)
client = lib.nvecdclient_create(ctypes.byref(config))
if not client:
    raise SystemExit("client creation failed")

if lib.nvecdclient_connect(client) != 0:
    raise SystemExit(lib.nvecdclient_get_last_error(client).decode())
if lib.nvecdclient_auth(client, b"s3cret") != 0:
    raise SystemExit(lib.nvecdclient_get_last_error(client).decode())

vector = (ctypes.c_float * 4)(0.1, 0.2, 0.3, 0.4)
if lib.nvecdclient_vecset(client, b"item1", vector, 4) != 0:
    raise SystemExit(lib.nvecdclient_get_last_error(client).decode())
lib.nvecdclient_metaset(client, b"item1", b"category:books,active:true")

response = ctypes.POINTER(NvecdSimResponse)()
if lib.nvecdclient_sim(client, b"item1", 5, b"vectors", ctypes.byref(response)) != 0:
    raise SystemExit(lib.nvecdclient_get_last_error(client).decode())
for i in range(response.contents.count):
    item = response.contents.results[i]
    print(item.id.decode(), item.score)
lib.nvecdclient_free_sim_response(response)

lib.nvecdclient_disconnect(client)
lib.nvecdclient_destroy(client)
```

`nvecdclient_get_last_error` takes the handle, and its `restype` must be `c_char_p`; both are easy to omit and both produce a wrong answer rather than an error. The metadata argument is the flat `key:value,key:value` byte string, not a `dict`.

Other FFI layers follow the same rules: declare every signature, honour the ownership table, and read `nvecdclient_get_last_error(handle)` immediately after a failure. The header is the reference for what a binding has to declare.
