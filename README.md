# nvecd

An in-memory search engine that keeps two signals about the same items: the vectors you assign to them, and the record of which items were acted on together. A query can be answered from either signal, or from both blended into one ranking.

[![CI](https://img.shields.io/github/actions/workflow/status/libraz/nvecd/ci.yml?branch=main&label=CI)](https://github.com/libraz/nvecd/actions)
[![Version](https://img.shields.io/github/v/tag/libraz/nvecd?label=version)](https://github.com/libraz/nvecd/tags)
[![codecov](https://codecov.io/gh/libraz/nvecd/branch/main/graph/badge.svg)](https://codecov.io/gh/libraz/nvecd)
[![License](https://img.shields.io/badge/license-MIT-blue)](https://github.com/libraz/nvecd/blob/main/LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue?logo=c%2B%2B)](https://en.cppreference.com/w/cpp/17)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS-lightgrey)](https://github.com/libraz/nvecd)

![Events and vectors are maintained separately and fused at query time](docs/images/overview.svg)

"Customers who bought this also bought" and "items whose embeddings are close" are two different questions, and answering both usually means running two systems and reconciling their answers in the application. nvecd holds both indexes in one process and merges them behind a single query.

## What it does

```bash
./build/bin/nvecd -c examples/config.yaml   # listening on 127.0.0.1:11017
```

```bash
# What was acted on together. No vectors involved.
nvecd-cli -p 11017 EVENT alice ADD widget 100
nvecd-cli -p 11017 EVENT alice ADD gasket 80
nvecd-cli -p 11017 EVENT bob   ADD widget 100
nvecd-cli -p 11017 EVENT bob   ADD flange 95

nvecd-cli -p 11017 SIM widget 10 using=events
# (2 results, showing 2)
# 1) flange (score: 9500)
# 2) gasket (score: 8000)

# Give the same items vectors, and content similarity becomes available too.
nvecd-cli -p 11017 VECSET widget 0.10 0.20 0.30 0.40
nvecd-cli -p 11017 VECSET gasket 0.15 0.18 0.32 0.41

nvecd-cli -p 11017 SIM widget 10 using=fusion
# (2 results, showing 2)
# 1) gasket (score: 0.5972)
# 2) flange (score: 0.4)

nvecd-cli -p 11017 SIMV 10 0.5 0.3 0.2 0.1          # nearest to a query vector
# (2 results, showing 2)
# 1) gasket (score: 0.6569)
# 2) widget (score: 0.6139)
```

A co-occurrence score is the accumulated product of the paired event weights — `flange` scores 9500 because one context held it alongside `widget` at 95 and 100. It is a raw sum, not a similarity in `[0, 1]`. Fusion normalizes each side before blending, which is why the third query reorders the first one's answer: `gasket` gains a vector and overtakes the vectorless `flange`.

Co-occurrence works from the first event, with no embedding model and no training step. Vectors are optional and can be added later. When both are present, `using=fusion` blends them, and the blend can follow each item's data density instead of a fixed weight, so a new item is ranked mostly by its vector and a well-observed one mostly by its behaviour.

Events carry a timestamp and decay, so associations age out on their own. Results can be narrowed by item metadata and cut off below a score.

## Use cases

| Use case | What it does | Guide |
|---|---|---|
| A recommender with no vectors | Co-occurrence alone, from a stream of interactions. | [Recommendations](docs/en/use-cases/recommendations.md) |
| A product recommender | Engagement weighting, then content and behaviour fused per query. | [E-commerce](docs/en/use-cases/e-commerce.md) |
| A personalized feed | Continuous engagement events with a short half-life. | [Real-time feed](docs/en/use-cases/real-time-feed.md) |
| Semantic retrieval | Query-vector search over a document corpus. | [Semantic search](docs/en/use-cases/semantic-search.md) |

## Install

A C++17 compiler (GCC 9+ or Clang 10+) and CMake 3.15+. Dependencies are fetched during configuration; nothing needs to be installed system-wide.

```sh
git clone https://github.com/libraz/nvecd.git
cd nvecd
make          # configure, build, and produce bin/nvecd, bin/nvecd-cli, libnvecdclient
make test
```

[Installation](docs/en/installation.md) covers system packages, build options, installing the binaries and running as a service.

## Documentation

Start at [Introduction](docs/en/introduction.md) for the model the engine is built on, then [Getting started](docs/en/getting-started.md) for a first session end to end.

The concept pages explain what the engine is doing: [events and co-occurrence](docs/en/events-and-co-occurrence.md), [vector search](docs/en/vector-search.md), [fusion](docs/en/fusion.md), [caching](docs/en/caching.md) and [persistence](docs/en/persistence.md). The reference pages are the [TCP protocol](docs/en/protocol.md), the [HTTP API](docs/en/http-api.md), the [client library](docs/en/client-library.md) and the [configuration](docs/en/configuration.md) keys. [Architecture](docs/en/architecture.md) describes the internals, and [benchmarks](docs/en/benchmarks.md) records what has been measured.

## What it doesn't do

- **No embeddings.** nvecd stores and searches vectors; it does not produce them. Encode items with whatever model you already use and send the result with `VECSET`.
- **Single node.** The dataset lives in the memory of one process. There is no sharding, no replication and no cross-node query.
- **Memory-resident.** Snapshots and a write-ahead log make a restart recoverable, but nothing is served from disk. A dataset larger than RAM is out of scope.
- **No model serving.** Ranking is a weighted merge of two score lists, not an inference step.

The project is pre-1.0: the wire protocol and the configuration keys can still change between versions. Security issues are best reported through a private GitHub advisory rather than a public issue.

## License

[MIT](LICENSE).
