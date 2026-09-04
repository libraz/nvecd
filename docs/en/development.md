# Development

This page covers working on nvecd itself: where code lives, the build-and-iterate loop, the test tiers, the sanitizer and coverage builds, formatting and linting, what continuous integration does, and the conventions a change has to follow.

Building the project for the first time is covered in [Installation](./installation.md); everything below assumes a configured `build/` directory.

## Repository layout

Source is split by concern under `src/`, and each subdirectory becomes one static library.

| Directory | Contents |
|---|---|
| `src/utils/` | `Expected`, `Error`, structured logging, string, memory, path and network helpers, RAII guards for file descriptors and flags |
| `src/config/` | The YAML parser, the JSON schema it validates against, the embedded copy of that schema generated at build time, `CONFIG SHOW` rendering, and the runtime variable manager |
| `src/events/` | The event ring buffers, the co-occurrence index, the deduplication cache and the state cache |
| `src/vectors/` | The vector store, the HNSW and IVF indexes, the metadata store and filter, CPU feature detection and the scalar, AVX2 and NEON distance kernels |
| `src/similarity/` | The search engine that owns the ANN index and combines the vector and co-occurrence signals |
| `src/cache/` | The query cache, its key generation and MD5, LZ4 result compression, and the controller that enables and disables it at runtime |
| `src/storage/` | The snapshot format, the fork-based and lock-based snapshot sessions, and the write-ahead log with its checkpoint sidecar |
| `src/server/` | The connection acceptor, the I/O reactor and its multiplexer backends, the thread pool, the command parser and dispatcher, the per-command handlers, the HTTP server, rate limiting and the background schedulers |
| `src/client/` | The C++ and C client libraries, built as one shared library |
| `src/cli/` | `nvecd-cli` |

`src/main.cpp` is the server entry point and does nothing but parse arguments, apply logging configuration and drive start and stop.

Outside `src/`: `tests/` holds the CTest tree, `e2e/` the Python end-to-end suite, `examples/` the annotated configuration file, `support/` the developer scripts and the configuration documentation generator, `third_party/` the `FetchContent` declarations, and `cmake/` the package configuration and uninstall templates.

A new module gets its own subdirectory, its own static library in `src/CMakeLists.txt`, and an entry in the `-Werror=switch` target list at the bottom of that file. Dependencies point inward: `nvecd_utils` depends on nothing of nvecd's, and `nvecd_server` may depend on everything.

## Building and iterating

```bash
make            # configure if needed, then build with one job per core
make test       # build, then run CTest
make rebuild    # delete build/ and build from scratch
make run        # build, then start the server on examples/config.yaml
```

`make` reuses the existing CMake cache. Changing a CMake option means re-running the configure step with the new value:

```bash
make CMAKE_OPTIONS="-DENABLE_ASAN=ON" configure
make
```

Test runs are tuned with three variables:

```bash
make test TEST_JOBS=1                  # sequential
make test TEST_JOBS=2 TEST_VERBOSE=1   # two jobs, verbose
make test TEST_DEBUG=1                 # pass --debug to CTest
```

`TEST_JOBS` defaults to 4. `TEST_VERBOSE` and `TEST_DEBUG` default to 0 and add `--verbose` and `--debug` respectively.

## Makefile targets

| Target | What it does |
|---|---|
| `help` | Prints the target list and the test variables |
| `configure` | Runs CMake into `build/` with `CMAKE_INSTALL_PREFIX` from `PREFIX` and any `CMAKE_OPTIONS` |
| `build` | The default target; configures, then builds in parallel |
| `test` | Builds, then runs CTest with `TEST_JOBS`, `TEST_VERBOSE` and `TEST_DEBUG` applied |
| `test-full` | `test` with one job per core |
| `test-sequential` | `test` with a single job, for diagnosing a test that only fails under contention |
| `test-verbose` | `test` with CTest's verbose output |
| `quick-test` | Builds, then runs only the tests whose names match the event store or vector store, as a fast inner loop |
| `clean` | Removes `build/` |
| `rebuild` | `clean` followed by `build` |
| `install` | Builds, then installs into `PREFIX` |
| `uninstall` | Removes the files listed in the build's install manifest |
| `format` | Rewrites every `.cpp` and `.h` under `src/` and `tests/` with `clang-format` |
| `format-check` | The same file set in check-only mode; fails on the first file that would change |
| `lint` | Formats, builds, then runs `clang-tidy` over every source file |
| `lint-diff` | The same, restricted to the `.cpp` files under `src/` that the working tree has changed |
| `lint-diff-main` | The same, restricted to the files changed relative to `main` |
| `run` | Builds, then runs the server against `examples/config.yaml`, failing if that file is absent |
| `e2e-test` | Builds, then runs the whole Python suite |
| `e2e-test-smoke` | Builds, then runs the Python smoke tests only |
| `e2e-test-commands` | Builds, then runs the Python per-command tests only |

Three variables override defaults: `PREFIX` for the install prefix, `CLANG_FORMAT` for the formatter binary, and `CMAKE_OPTIONS` for extra configure arguments.

## Test tiers

Three tiers exist, and only the first runs by default.

### CTest unit tests

The bulk of the suite is GoogleTest binaries registered with `gtest_discover_tests`, one executable per area, mirroring the `src/` layout under `tests/`. Run them all through CTest:

```bash
ctest --test-dir build --output-on-failure
```

or run one binary directly, which is faster and gives GoogleTest's own filtering:

```bash
./build/bin/event_store_test
./build/bin/event_store_test --gtest_filter="EventStoreTest.*"
```

Two tests in this tier are not GoogleTest binaries. `docs_contract_test`, carrying the label `docs`, checks the documentation against the source: that the generated configuration reference and example file are current, that every schema key is parsed and readable and actually consumed outside `src/config/`, that every field name and status word the protocol pages promise exists verbatim in the source, that the English and Japanese pages state the same set of facts, and that every error code has a construction site. A documentation change that invents a response field fails here. `install_consumer_smoke`, carrying the label `install`, installs the project into a scratch prefix and builds a small consumer against the exported CMake package.

Tests carry labels beyond those two: `integration`, `benchmark`, `http`, `client`, `e2e`, `sanitizer`, `concurrency` and `crash`. Select or exclude with `ctest -L` and `ctest --label-exclude`.

### Integration-labelled tests

`tests/integration/` holds tests that start a real server on a real socket, or fork for a snapshot, and take seconds rather than milliseconds: end-to-end command coverage, reactor behaviour under load, concurrency, adversarial input, cache behaviour, metrics, scale scenarios, and write-ahead log recovery including a crash case. They register with the `integration` label and are part of a default `ctest` run locally.

```bash
ctest --test-dir build -L integration --output-on-failure
ctest --test-dir build --label-exclude integration --output-on-failure
```

The second form is what continuous integration runs, so these tests are verified only by whoever runs them locally.

### The Python end-to-end suite

`e2e/` is a separate tier that CTest does not know about. It drives the built server over its real TCP and HTTP surfaces from `pytest`, and it is reachable only through the `e2e-test` targets:

```bash
make e2e-test
make e2e-test-smoke
make e2e-test-commands
```

The suite needs a Python 3 interpreter with `pytest` and `pytest-timeout` installed; `e2e/pyproject.toml` records the interpreter version it requires. Its tests are grouped into `smoke`, `commands`, `edge_cases` and `workflows`, registered as pytest markers, so a subset can also be selected with `-m`. The fixtures start one server per session on ports chosen at run time, with its own temporary snapshot directory and a password of their own, and they expect the binary at `build/bin/nvecd`. Set `NVECD_E2E_BINARY` to point at a binary from a different build tree — a sanitizer build, for instance.

### Benchmarks

`tests/benchmark/` holds two measurement binaries. Their tests are prefixed `DISABLED_`, so a normal run skips them and they cost nothing. [Benchmarks](./benchmarks.md) covers what they measure and how to run them.

## Sanitizer builds

Sanitizers are configure-time options, so each needs its own build directory rather than a reconfigure of the one in use:

```bash
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure

cmake -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON
cmake --build build-tsan --parallel
ctest --test-dir build-tsan -L concurrency --output-on-failure
```

The two cannot be combined in one build. The tests most worth running under ThreadSanitizer carry the `sanitizer` and `concurrency` labels: the ANN index tests, the shared index contract tests, the tiered store tests and the similarity concurrency stress test.

## Coverage

```bash
cmake -B build-coverage -DENABLE_COVERAGE=ON
cmake --build build-coverage --parallel
cmake --build build-coverage --target coverage
```

The `coverage` target zeroes the counters, runs the whole suite in parallel, captures the data, filters it down to `src/` and renders HTML into `build-coverage/coverage/html/`. It exists only when `lcov` and `genhtml` are both on the path; without them CMake warns at configure time and the target is not defined. `coverage-clean` zeroes the counters and removes the output directory.

## Formatting

Formatting is `clang-format` driven by a repository-root `.clang-format`: Google style, C++17, 120-column lines, two-space indent, pointers bound to the type, includes sorted and regrouped.

```bash
make format
make format-check
```

`format` rewrites files in place. `format-check` is its check-only counterpart, running the same file set with `--dry-run --Werror`, and is what a pre-merge check would use — continuous integration does not currently run it.

Both cover every `.cpp` and `.h` under `src/` and `tests/`. Use `CLANG_FORMAT` when the binary is not called `clang-format` on the machine:

```bash
make CLANG_FORMAT=clang-format-18 format
```

## Linting

```bash
make lint
make lint-diff
make lint-diff-main
```

Each target delegates to `support/dev/run-clang-tidy.sh`. Two things follow from how that script works.

It needs a compiled build: it reads `build/compile_commands.json` for the exact flags each translation unit was compiled with, and stops with an error if the build directory or that file is missing. Running `clang-tidy` over the sources directly, without a compilation database, does not work — the sources include generated and fetched headers whose locations only the build knows.

It applies one configuration to every file, the `.clang-tidy` at the repository root, passed explicitly with `--config-file`. That configuration enables the `cppcoreguidelines`, `modernize`, `performance` and `readability` families along with the compiler and analyzer diagnostics, and restricts header diagnostics to `src/`. Warnings are advisory: the script prints them, reports a count, and exits successfully either way, so lint output has to be read rather than relied on to fail.

`lint-diff` selects the `.cpp` files under `src/` that differ from `HEAD`, staged or not; `lint-diff-main` selects those that differ from `main`. Both fall back to serial execution, while a full `make lint` uses `run-clang-tidy` in parallel when it is available. All three depend on `format` and `build`, so they rewrite files and compile before checking.

## Generated documentation

Part of the documentation is rendered rather than written. `src/config/config-schema.json` is the single authority for every configuration option — its type, default, accepted range, and its prose description in both languages — and `support/generate_config_docs.py` renders that authority into three artefacts:

- `examples/config.yaml`, rendered in full, header comment included.
- The option tables in `docs/en/configuration.md`, one per schema section.
- The option tables in `docs/ja/configuration.md`, the same set.

In the two configuration guides the generator does not own the whole file, only the regions delimited by `<!-- BEGIN GENERATED: options <section> -->` and `<!-- END GENERATED: options <section> -->`, one pair per schema section that owns leaf keys. Everything outside those markers is hand-written prose and the generator leaves it alone; everything between them is overwritten on every run.

Adding or changing a configuration option therefore means editing the schema and re-running the generator:

```bash
python3 support/generate_config_docs.py
```

It prints each artefact it rewrote, and rewrites nothing that is already current. Never edit a rendered table or `examples/config.yaml` by hand — the next run discards the edit. This includes the Japanese wording: the Japanese table takes its text from each key's `description_ja` in the schema, falling back to `description` when that field is absent, so translating an option means adding `description_ja` to the schema rather than editing the Japanese table.

The markers are part of the contract in both directions. A region naming a section the schema does not define stops the generator with an error, so renaming a schema section means renaming its region; and a section whose markers are missing from a guide is silently not rendered there, which leaves that language's table to drift by hand.

Check mode writes nothing and reports instead:

```bash
python3 support/generate_config_docs.py --check
```

It exits non-zero and names every artefact that has fallen out of date with the schema. `--source-root` points it at a checkout other than the one the script lives in.

Nothing in the `Makefile` and no step in the workflow file runs the generator. Its only automatic invocation is from `docs_contract_test`, which runs it in check mode as the first of its checks. That test carries the `docs` label rather than `integration`, so it is one of the tests continuous integration does run — a schema change committed without regenerating fails there.

## Continuous integration

The workflow in `.github/workflows/ci.yml` runs on pushes to `main` and on pull requests against it, skipping runs whose changes are confined to Markdown, `docs/` or the licence. What it does, stated plainly so nobody assumes wider coverage than exists:

- It runs on Ubuntu only. macOS is a supported platform with no automated coverage.
- It configures a `Debug` build with `NVECD_PORTABLE_BUILD=ON`, so the binary it tests is built for the baseline architecture rather than the runner's own.
- It enables coverage by adding `--coverage` to `CMAKE_CXX_FLAGS` and `CMAKE_C_FLAGS` by hand, not through the `ENABLE_COVERAGE` option, so the `coverage` target and its `lcov` filtering are not involved; the raw `gcov` data is uploaded instead.
- It runs `ctest --label-exclude integration`, so no `integration`-labelled test runs there. The snapshot, fork and write-ahead log recovery tests are verified locally or not at all.
- It does not run `lint`, `format-check`, either sanitizer, or the Python end-to-end suite.

Anything in those last two points is the contributor's responsibility before opening a change.

## Conventions

Commit subjects use Conventional Commits with one of six scopes, matching the areas above: `cache`, `server`, `events`, `vectors`, `similarity`, `client`.

Error codes are partitioned by module, and a new module takes an unused band rather than extending someone else's:

| Range | Module |
|---|---|
| 0-999 | General |
| 1000-1999 | Configuration |
| 2000-2999 | Event processing |
| 3000-3999 | Command parsing |
| 4000-4999 | Vector and similarity |
| 5000-5999 | Storage and snapshot |
| 6000-6999 | Network and server |
| 7000-7999 | Client |
| 8000-8999 | Cache |

Every new code in the enumeration must have at least one construction site. `docs_contract_test` fails on a code that nothing can produce, apart from a fixed list of codes it carries that may only shrink, so a code added ahead of the code path that raises it breaks the suite.

Comments and identifiers are English. Failures propagate as `Expected<T, Error>` rather than exceptions, so a new function that can fail returns one rather than signalling out of band.
