# Benchmarks

This document presents measured performance benchmarks for nvecd's similarity search pipeline.

## Benchmark Environment

- **Hardware**: Apple M4 Max (arm64), 128GB unified memory
- **SIMD**: NEON (4-wide, 128-bit), 4x multi-accumulator unrolled
- **Build**: Release (`-O2`), Clang 16
- **Configuration**: Default (`distance_metric: cosine`, `sample_size: 10000`)
- **Test Methodology**: Median over 15 iterations, warm state (compact storage populated)
- **Reproducible**: `./bin/similarity_benchmark --gtest_also_run_disabled_tests`

> **Note on hardware**: Apple Silicon uses unified memory with higher bandwidth than typical server DDR4/DDR5 configurations. On x86 servers with AVX2 (8-wide, 256-bit), absolute latency is expected to be **lower** due to wider SIMD registers (2x the throughput per instruction). The relative scaling characteristics remain consistent across platforms.

## Search Latency

### End-to-End Query (SIM, dim=128, top_k=10, cosine)

| Dataset Size | SearchByIdVectors (SIM) | SearchByVector (SIMV) | Cache Hit |
|---|---|---|---|
| 1K vectors | 0.012ms | 0.012ms | 0.25us |
| 10K vectors | 0.12ms | 0.12ms | 0.25us |
| 50K vectors | 0.62ms | 0.64ms | 0.25us |
| 100K vectors | **1.03ms** | **1.05ms** | **0.25us** |

**Key findings:**

- Sub-millisecond latency up to 50K vectors without sampling
- 100K vectors: ~1ms per query (**~970 QPS per thread**)
- Cache hit latency: 250 nanoseconds (**4 million ops/sec**)
- Cache provides **4,000x** speedup over cold queries at 100K scale

## Pipeline Breakdown (100K vectors, dim=128)

| Component | Time | % of E2E |
|---|---|---|
| NEON cosine scan (brute-force) | 0.80ms | 78% |
| Min-heap top-k selection | 0.05ms | 5% |
| Lock acquisition + index lookup | 0.03ms | 3% |
| Result string construction (k=10) | 0.02ms | 2% |
| **Total (SearchByIdVectors)** | **1.03ms** | **100%** |

The scan (SIMD dot product) dominates at 78%. The min-heap approach avoids allocating 100K result strings, reducing overhead from ~0.22ms (partial_sort) to ~0.05ms.

## SIMD Performance

### Distance Operations (dim=128, per-call)

| Operation | Scalar | NEON (4x unrolled) | AVX2 (4x unrolled) |
|---|---|---|---|
| Dot product | ~45ns | **~8ns** | **~4ns** (projected) |
| L2 norm | ~40ns | **~7ns** | **~3.5ns** (projected) |
| L2 distance | ~50ns | **~9ns** | **~4.5ns** (projected) |

NEON: 5-6x over scalar. AVX2 (projected): 10-12x over scalar.

### 4x Multi-Accumulator Optimization

The dot product loop uses 4 independent SIMD accumulators to hide FMA latency:

| Technique | dim=128 | dim=768 |
|---|---|---|
| Single accumulator | ~14ns | ~85ns |
| **4x multi-accumulator** | **~8ns** | **~48ns** |
| Improvement | 1.75x | 1.77x |

## Optimization History

### Cumulative Improvements (100K vectors, dim=128, top_k=10)

| Optimization | E2E Latency | Speedup vs Previous | Cumulative |
|---|---|---|---|
| Baseline (hash map brute-force) | 5.27ms | - | 1.0x |
| Compact contiguous storage | 2.83ms | 1.86x | 1.86x |
| Min-heap top-k (no string alloc) | 1.66ms | 1.71x | 3.17x |
| 4x SIMD multi-accumulator | 1.25ms | 1.33x | 4.22x |
| Unified storage (no dual map) | **1.03ms** | 1.21x | **5.12x** |

**Total: 5.12x faster** from baseline to current.

## Compact Storage vs Dual Storage

| Metric | Dual (vectors_ + compact) | Unified (compact only) |
|---|---|---|
| Memory (100K, dim=128) | ~98MB | **~49MB** (50% reduction) |
| Memory (1M, dim=128) | ~1.1GB | **~550MB** (50% reduction) |
| SetVector latency | O(1) hash insert + invalidate | O(dim) memcpy (**~0.5us**) |
| Write-then-read block | **160ms** (Compact() rebuild) | **0ms** (always ready) |
| DeleteVector | O(1) erase | O(1) tombstone |

Unified storage eliminates the 160ms Compact() blocking that occurred after every write.

## Cache Performance

### Selective Invalidation vs Full Clear

| Metric | Full Clear (old) | Selective (new) |
|---|---|---|
| Invalidation scope | All entries | Only affected entries |
| Invalidation time | O(n) clear | O(k) per item |
| Cache survival after write | 0% | **~95-99%** |
| Effective hit rate under writes | Low | **High** |

Selective invalidation uses a reverse index (`item_id -> set<CacheKey>`) to invalidate only cache entries that reference the mutated item.

### Cache Lookup Latency

| Operation | Latency | Ops/sec |
|---|---|---|
| Cache miss (key not found) | 42-83ns | 12-24M |
| Cache hit (decompress + return) | 250ns | 4M |

Cache overhead is negligible compared to search latency.

## Approximate Search (Sampling)

### Accuracy vs Speed Tradeoff

| sample_size | Coverage (1M vectors) | Latency | Recall@10 (typical) |
|---|---|---|---|
| 0 (exact) | 100% | ~11ms | 100% |
| 50,000 | 5% | ~0.6ms | ~95% |
| **10,000 (default)** | **1%** | **~0.12ms** | **~80-90%** |
| 5,000 | 0.5% | ~0.06ms | ~70-80% |

Reservoir sampling (Algorithm R) provides uniform random coverage. For recommendation workloads where approximate results are acceptable, `sample_size=10000` offers a good speed/quality tradeoff.

### When to Use Exact Search

Set `sample_size: 0` in configuration when:
- Dataset < 20K vectors (full scan is already fast)
- Exact top-k results are required (e.g., deduplication)
- Latency budget allows > 10ms per query

## Defragment Performance

After `DeleteVector()`, tombstoned slots accumulate. `Defragment()` runs automatically when fragmentation exceeds 25%.

| Dataset Size | Defragment Time (25% tombstones) |
|---|---|
| 1K vectors | 0.04ms |
| 10K vectors | 0.33ms |
| 50K vectors | 1.69ms |
| 100K vectors | 3.39ms |

Defragment acquires an exclusive lock briefly for the pointer swap. The rebuild phase itself is fast due to contiguous memory access patterns.

## Memory Sizing

### Vector Storage

```
Memory = num_vectors x dimension x 4 bytes (float32)
       + num_vectors x 4 bytes (norms)
       + num_vectors x ~64 bytes (ID strings + index overhead)
```

| Vectors | dim=128 | dim=384 | dim=768 |
|---|---|---|---|
| 10K | 6MB | 16MB | 31MB |
| 100K | 55MB | 155MB | 308MB |
| 1M | **550MB** | **1.5GB** | **3.1GB** |

### Total System Memory (with events + cache)

| Scale | Vectors (dim=128) | Events (50K contexts) | Cache (32MB) | Total |
|---|---|---|---|---|
| 100K items | 55MB | 250MB | 32MB | **~340MB** |
| 1M items | 550MB | 250MB | 128MB | **~930MB** |

## Running Benchmarks

```bash
# Build in Release mode
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel

# Run all benchmarks
./build/bin/similarity_benchmark --gtest_also_run_disabled_tests

# Run specific benchmark
./build/bin/similarity_benchmark --gtest_also_run_disabled_tests \
  --gtest_filter="*SearchByIdVectors*"
```

Benchmark tests are labeled `benchmark` and excluded from CI. They are `DISABLED_` prefixed for safety.
