# Installation

This page covers building nvecd from source, installing it, running the server and diagnosing a build or startup that fails.

## Supported platforms

nvecd builds and runs on Linux and macOS, on x86_64 and on 64-bit ARM. The build assumes a POSIX system with `epoll` or `kqueue` available to the I/O reactor; there is no Windows build.

Continuous integration builds and tests on Ubuntu only. macOS is supported and the build carries macOS-specific configuration, but no automated job exercises it, so a macOS regression is found by whoever builds there.

## Compiler and CMake requirements

- CMake 3.15 or later.
- A C++17 compiler: GCC 9 or later, or Clang 10 or later. The build sets flags that only GCC and Clang accept; no other compiler is configured for.
- zlib development headers, found with `find_package(ZLIB REQUIRED)`.
- A pthreads implementation, found with `find_package(Threads REQUIRED)`.
- A Python 3 interpreter, required at configure time whenever tests are built, which is the default. Configuration fails without it.
- Optionally `readline`. When it is found, `nvecd-cli` gets tab completion and history; when it is not, the CLI builds without them.

These versions are the compatibility floor for the whole project and are not restated on other pages.

## Bundled dependencies

Seven dependencies are fetched by CMake's `FetchContent` at configure time and built as part of the project. Nothing needs to be installed for them, but the first configure of a clean tree needs network access.

| Dependency | Used for |
|---|---|
| yaml-cpp | Parsing the YAML configuration file |
| GoogleTest | The test suite; fetched only when `BUILD_TESTS` is on |
| spdlog | Server logging |
| lz4 | Compressing cached query results |
| nlohmann/json | JSON request and response bodies on the HTTP API |
| cpp-httplib | The HTTP server |
| json-schema-validator | Validating the configuration against the embedded schema |

Each is pinned to an immutable tag or commit, so a clean build resolves to the same sources every time. On macOS, Homebrew prefixes are added to `CMAKE_PREFIX_PATH` for `find_package` and `find_library` only, and deliberately not to the include path, so a Homebrew copy of one of these libraries cannot shadow the pinned one.

## System packages

### Debian and Ubuntu

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake git python3 zlib1g-dev
```

Optional, for the CLI's line editing and for the formatting and lint targets:

```bash
sudo apt-get install -y libreadline-dev clang-format clang-tidy
```

### macOS

Install the Xcode command line tools, then the build tools:

```bash
xcode-select --install
brew install cmake
```

Optional, for the same reasons as above:

```bash
brew install readline llvm
```

`clang-format` and `clang-tidy` ship inside the `llvm` formula rather than with the command line tools, and the formula is not linked into `PATH` by default.

## Building with make

The `Makefile` wraps CMake. It configures into `build/`, then builds with one job per detected core.

```bash
git clone https://github.com/libraz/nvecd.git
cd nvecd
make
```

The wrapper passes no build type, so a plain `make` produces a build with warnings enabled and no optimisation level selected. For a build to measure or deploy, pass the build type through to the configure step:

```bash
make CMAKE_OPTIONS="-DCMAKE_BUILD_TYPE=Release" configure
make
```

`CMAKE_OPTIONS` is read by the `configure` target only. Once `build/` holds a CMake cache, later `make` invocations reuse it, so changing an option means re-running `configure`.

## Building with CMake directly

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Binaries land in `build/bin/` and libraries in `build/lib/`, regardless of which of the two paths configured the tree.

## Build types

| Build type | Flags added |
|---|---|
| `Debug` | `-g -O0 -fno-omit-frame-pointer` |
| `Release` | `-O3 -DNDEBUG` |
| `RelWithDebInfo` | CMake's defaults for the type |
| (unset) | Neither set; only the common warning and architecture flags apply |

Every build also gets `-Wall -Wextra -Wpedantic` and an architecture flag chosen by `NVECD_PORTABLE_BUILD`, described below. nvecd's own targets additionally promote `-Wswitch` to an error, so adding an enumerator breaks the build at every switch that decides something about it.

## CMake options

| Option | Default | Effect |
|---|---|---|
| `BUILD_TESTS` | `ON` | Builds the test executables, fetches GoogleTest and requires a Python 3 interpreter |
| `ENABLE_ASAN` | `OFF` | Adds `-fsanitize=address -fno-omit-frame-pointer` |
| `ENABLE_TSAN` | `OFF` | Adds `-fsanitize=thread` |
| `ENABLE_COVERAGE` | `OFF` | Adds `--coverage -O0 -g` and defines the `coverage` and `coverage-clean` targets |
| `NVECD_PORTABLE_BUILD` | off | Selects a baseline architecture instead of tuning for the build host |

`NVECD_PORTABLE_BUILD` decides the architecture flag. Left off, the build gets `-march=native`, which tunes for the machine doing the compiling and produces a binary that may fault with an illegal instruction on an older CPU. Turned on, the build gets `-march=x86-64` on x86_64 or `-march=armv8-a` on 64-bit ARM, which runs anywhere in that architecture family. Turn it on whenever the build host and the run host are not the same machine; continuous integration builds with it on.

Unlike the four options above, `NVECD_PORTABLE_BUILD` is not declared with CMake's `option()` command. It has no cache entry, no help text and does not appear in `cmake -LH` output, so nothing advertises it. Set it explicitly:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DNVECD_PORTABLE_BUILD=ON
```

Sanitizers are mutually exclusive: a build with both `ENABLE_ASAN` and `ENABLE_TSAN` on does not link.

The version compiled into the binary comes from the `NVECD_VERSION` environment variable when it is set, and from `git describe --tags --abbrev=0` otherwise.

## Artifacts

A completed build produces three things:

- `build/bin/nvecd` — the server.
- `build/bin/nvecd-cli` — an interactive and one-shot client for the TCP protocol.
- `build/lib/libnvecdclient.so` on Linux, `build/lib/libnvecdclient.dylib` on macOS — the shared client library, described in [Client Library](./client-library.md).

## Installing

### To the default prefix

```bash
make
sudo make install
```

The default prefix is `/usr/local`. Installed files:

| Path | Contents |
|---|---|
| `bin/nvecd`, `bin/nvecd-cli` | The server and the CLI |
| `lib/libnvecdclient.*` | The shared client library |
| `include/nvecd/` | `nvecdclient.h`, `nvecdclient_c.h`, and `utils/error.h`, `utils/expected.h` |
| `lib/cmake/nvecd/` | The CMake package, exporting the `nvecd::client` target |
| `etc/nvecd/config.yaml` | The annotated example configuration |
| `etc/nvecd/config-schema.json` | The schema the server validates configuration against |
| `share/doc/nvecd/` | `README.md` and both documentation trees |

A CMake project consumes the installed client library with:

```cmake
find_package(nvecd CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE nvecd::client)
```

### To another prefix

```bash
make PREFIX=/opt/nvecd install
```

Or, driving CMake directly:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/nvecd
cmake --build build --parallel
sudo cmake --install build
```

The `Makefile` passes `PREFIX` to the configure step as `CMAKE_INSTALL_PREFIX`, so setting it on the `install` line reconfigures the tree.

### Uninstalling

```bash
sudo make uninstall
```

which drives the CMake target directly:

```bash
sudo cmake --build build --target nvecd-uninstall
```

Removal reads `install_manifest.txt` from the build directory, so it removes exactly what that build installed. A build tree that has been cleaned cannot uninstall a previous installation.

## Verifying the installation

```bash
nvecd --version
nvecd --help
```

The first prints the binary's version followed by a one-line description; the second prints the accepted options.

Validate a configuration file without starting the server:

```bash
nvecd --config-test -c /etc/nvecd/config.yaml
```

The exit status is zero when the file parses and validates. On success it prints a summary of the event, vector, similarity, API and performance sections, which is the fastest way to confirm that an edit took effect.

## Running the server

The server takes the configuration file as `-c`/`--config`, or as a bare positional argument:

```bash
nvecd -c /etc/nvecd/config.yaml
```

| Option | Meaning |
|---|---|
| `-c`, `--config` | Path to the configuration file |
| `-t`, `--config-test` | Validate the file, print a summary and exit |
| `-h`, `--help` | Print usage and exit |
| `-v`, `--version` | Print version information and exit |

Started with no configuration file, the server runs on the compiled-in defaults and restricts `network.allow_cidrs` to `127.0.0.1/32`, so a server started by accident is not reachable from the network. Every other setting keeps its default. See [Configuration](./configuration.md) for the full set.

The server reads no environment variables for its own options; everything is set in the configuration file. `SIGINT` and `SIGTERM` request a graceful shutdown. There is no reload signal — a configuration change that is not one of the runtime-settable variables takes a restart.

### Authenticating the CLI

When `security.requirepass` is set, `nvecd-cli` needs the password, and both ways of supplying it keep it out of the process argument list, where any user on the machine could read it.

```bash
chmod 600 /path/to/nvecd-password
nvecd-cli --password-file /path/to/nvecd-password INFO
```

The password file must be a regular file owned by the user running the CLI, with no group or other permissions; the CLI refuses it otherwise, and refuses to follow a symlink to it.

The second form is the only place nvecd looks at the environment for an option. `--password-env` names the variable to read; it does not read a fixed variable name.

```bash
export NVECD_PASSWORD='replace-me'
nvecd-cli --password-env NVECD_PASSWORD INFO
```

The two are mutually exclusive. A password containing a carriage return or newline is rejected, since neither can survive the line-oriented protocol.

## Running under systemd

The server runs in the foreground and stops on `SIGTERM`, so a plain service unit is enough.

```text
[Unit]
Description=nvecd vector search engine
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=nvecd
Group=nvecd
ExecStart=/usr/local/bin/nvecd -c /etc/nvecd/config.yaml
Restart=on-failure
RestartSec=5

NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=/var/lib/nvecd

[Install]
WantedBy=multi-user.target
```

`ReadWritePaths` has to cover `snapshot.dir` and, when the write-ahead log is enabled, `wal.dir`. With `ProtectSystem=strict` and those paths missing, the server starts and then fails its first write.

Set `logging.file` to an empty string to keep logs on stdout, where the journal collects them, and `logging.json` to `true` for a machine-readable format.

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now nvecd
journalctl -u nvecd -f
```

## File permissions and a dedicated user

Run the server as an unprivileged account that owns only its data:

```bash
sudo useradd --system --shell /usr/sbin/nologin --home-dir /var/lib/nvecd nvecd
sudo mkdir -p /var/lib/nvecd/snapshots /var/lib/nvecd/wal
sudo chown -R nvecd:nvecd /var/lib/nvecd
sudo chmod 700 /var/lib/nvecd/snapshots /var/lib/nvecd/wal
```

The configuration file holds `security.requirepass` in cleartext, so it should be readable by the service account and nobody else:

```bash
sudo chown root:nvecd /etc/nvecd/config.yaml
sudo chmod 640 /etc/nvecd/config.yaml
```

The server writes snapshots and write-ahead log segments under its own account, and a snapshot taken in `fork` mode is written by a forked child, so both directories must stay writable for the account for the lifetime of the process, not only at startup.

## Troubleshooting

**The first configure fails while fetching a dependency.** `FetchContent` clones each pinned dependency at configure time. Behind a proxy or without outbound network access the configure step fails before any compilation starts. Configure once on a machine with access, or make the Git remotes reachable.

**Configure fails looking for a Python interpreter.** The test tree requires one, and tests are on by default. Install Python 3, or configure with `-DBUILD_TESTS=OFF` if the tests are not wanted.

**The binary dies with an illegal instruction on another machine.** A build without `NVECD_PORTABLE_BUILD` is compiled with `-march=native` for the build host. Rebuild with `-DNVECD_PORTABLE_BUILD=ON`.

**`nvecd-cli` has no tab completion.** `readline` was not found when the CLI was configured. Install it and re-run the configure step; the search result is cached, so a rebuild alone does not pick it up.

**The server exits reporting that it cannot bind.** Another process holds the port. Check with `lsof -i :11017`, then either stop it or change `api.tcp.port`.

**The server starts but refuses every connection.** `network.allow_cidrs` defaults to empty, which denies everything, and a server started without a configuration file allows only `127.0.0.1/32`. List the client networks explicitly.

**Snapshot or write-ahead log writes fail with a permission error.** The directories named by `snapshot.dir` and `wal.dir` must exist and be writable by the account the server runs as; the server does not create parent directories it lacks permission for.
