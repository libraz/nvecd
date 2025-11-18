# Performance Guide

This guide provides performance optimization tips and best practices for nvecd.

## Performance Features

Nvecd is designed for high-performance vector similarity search with the following optimizations:

### 1. SIMD Acceleration

**Automatic CPU Detection**

Nvecd automatically detects and uses the best SIMD instructions available on your CPU:

- **ARM (Apple Silicon, ARM servers)**: NEON SIMD (4-wide operations)
- **x86_64 (Intel, AMD)**: AVX2 SIMD (8-wide operations)
- **Fallback**: Scalar implementation for older CPUs

**Performance Impact:**

| Operation | Scalar | NEON (ARM64) | AVX2 (x86_64) |
|-----------|--------|--------------|---------------|
| Dot product (768-dim) | Baseline | **3-4x faster** | **6-8x faster** |
| Cosine similarity | Baseline | **2.5-3x faster** | **5-6x faster** |

**Verification:**

Check server logs on startup:
```
[info] Vector SIMD: NEON      # ARM64
[info] Vector SIMD: AVX2      # x86_64
[info] Vector SIMD: Scalar    # Fallback
```

### 2. Smart Query Caching

**LRU Cache with Compression**

Nvecd uses an intelligent cache system that:
- Caches expensive query results (configurable threshold)
- Compresses results with LZ4 (75% memory reduction)
- Automatically invalidates stale entries

**Configuration:**

```yaml
cache:
  enabled: true
  max_memory_mb: 32          # Maximum cache size
  min_query_cost_ms: 10.0    # Only cache queries ≥10ms
  compression_enabled: true
```

**Cache Statistics:**

```bash
echo "CACHE STATS" | nc localhost 11017
```

Output:
```
total_queries: 10000
cache_hits: 8500
cache_misses: 1500
hit_rate: 0.8500
current_entries: 2450
current_memory_mb: 12.45
evictions: 320
avg_hit_latency_ms: 0.05
time_saved_ms: 15420.50
```

**Key Metrics:**
- `hit_rate`: 0.85 = 85% of queries served from cache
- `time_saved_ms`: Total latency saved by cache hits
- `avg_hit_latency_ms`: Cache lookup time (~0.05ms)

### 3. Automatic Cache Invalidation

**Smart Invalidation:**

Nvecd automatically invalidates affected cache entries when data changes:

| Event | Invalidated Cache |
|-------|-------------------|
| `VECSET` (update vector) | SIM queries for that vector ID |
| `EVENT` (add co-occurrence) | Fusion queries (events mode affected) |

**Manual Cache Management:**

```bash
# Clear all cache
echo "CACHE CLEAR" | nc localhost 11017

# Check cache status
echo "CACHE STATS" | nc localhost 11017
```

## Optimization Tips

### 1. Choose the Right Search Mode

```yaml
# SIM <id> <top_k> <mode>
```

**Modes:**

| Mode | Use Case | Performance |
|------|----------|-------------|
| `vectors` | Pure content similarity | Fastest (SIMD only) |
| `events` | Pure behavior patterns | Fast (bitmap lookup) |
| `fusion` | Hybrid recommendations | Medium (SIMD + bitmap) |

**Recommendation:**
- Start with `vectors` for content-based search
- Use `events` for collaborative filtering ("users who liked X also liked Y")
- Use `fusion` when you need both signals

### 2. Cache Tuning

**Adjust min_query_cost_ms:**

```yaml
cache:
  min_query_cost_ms: 10.0    # Only cache queries ≥10ms
```

**Guidelines:**
- **Small datasets (<10K vectors)**: Set to `5.0` (most queries are fast)
- **Medium datasets (10K-100K)**: Set to `10.0` (default)
- **Large datasets (>100K)**: Set to `20.0` (cache only slow queries)

**Memory vs Hit Rate Trade-off:**

```yaml
cache:
  max_memory_mb: 32    # Low memory: fewer cached queries
  max_memory_mb: 128   # High memory: more cached queries
```

Monitor with `CACHE STATS` and adjust based on hit rate.

### 3. Vector Dimension Selection

**Smaller dimensions = Faster queries**

| Dimension | Typical Use | Query Time* |
|-----------|-------------|-------------|
| 128 | Fast lookups, moderate quality | ~0.5ms |
| 384 | Balanced (sentence transformers) | ~1.5ms |
| 768 | High quality (BERT embeddings) | ~3ms |
| 1536 | Very high quality (OpenAI) | ~6ms |

\* Approximate query time for 10K vectors with AVX2

**Recommendation:**
- Start with 384-dimensional embeddings (good quality/speed trade-off)
- Use 768 if quality is critical and dataset is <100K vectors
- Use 128 if you need sub-millisecond latency

### 4. Batch Operations

**Loading Many Vectors:**

Use a client library to batch VECSET commands:

```cpp
#include <nvecdclient.h>

nvecdclient::NvecdClient client(config);
client.Connect();

// Batch 1000 vectors
for (const auto& [id, vector] : vectors) {
    client.Vecset(id, vector);  // Reuses TCP connection
}
```

**Avoid:** Opening/closing connection for each vector (TCP overhead)

### 5. Event Buffer Sizing

```yaml
events:
  ctx_buffer_size: 1000    # Events per context
```

**Guidelines:**
- **User sessions**: 100-500 (typical session length)
- **Product views**: 500-1000 (more activity per user)
- **Long-term tracking**: 1000-5000 (multi-day sessions)

Larger buffers = more memory, better recommendations

## Production Deployment

### 1. Memory Sizing

**Rule of thumb:**

```
Memory = (num_vectors × dimension × 4 bytes) + event_memory + overhead
```

**Example:**
- 100K vectors × 768 dimensions × 4 bytes = 307MB (vectors)
- 10K contexts × 1000 events × 50 bytes = 500MB (events)
- **Total: ~1GB minimum, 2GB recommended**

### 2. Monitoring

**Health Checks:**

```bash
# Liveness probe (Kubernetes)
curl http://localhost:8080/health/live
# → 200 OK

# Readiness probe (load balancer)
curl http://localhost:8080/health/ready
# → 200 OK (ready) or 503 (loading snapshot)

# Detailed metrics
curl http://localhost:8080/health/detail
```

**Key Metrics to Monitor:**

```json
{
  "uptime_seconds": 3600,
  "total_requests": 15000,
  "memory": {
    "vector_store_bytes": 307200000,
    "event_store_bytes": 524288000,
    "cache_bytes": 13107200
  },
  "cache": {
    "hit_rate": 0.85,
    "time_saved_ms": 15420.50
  }
}
```

### 3. Backup Strategy

**Regular Snapshots:**

```bash
# Manual snapshot
echo "DUMP SAVE /backup/nvecd-$(date +%Y%m%d-%H%M%S).dmp" | nc localhost 11017

# Automated daily snapshot (cron)
0 2 * * * echo "DUMP SAVE" | nc localhost 11017
```

**Snapshot Performance:**
- 100K vectors (768-dim) + 1M events: ~500ms save time, ~300MB file size
- Snapshots are atomic (no service interruption)

### 4. High Availability

**Load Balancer Setup:**

```
┌─────────────┐
│ Load        │
│ Balancer    │
└─────┬───────┘
      │
  ┌───┴───┬───────┬───────┐
  │       │       │       │
┌─▼──┐  ┌─▼──┐  ┌─▼──┐  ┌─▼──┐
│nvecd│  │nvecd│  │nvecd│  │nvecd│
│ #1  │  │ #2  │  │ #3  │  │ #4  │
└─────┘  └─────┘  └─────┘  └─────┘
```

**Configuration:**
- Each instance loads the same snapshot on startup
- Use `/health/ready` for load balancer health checks
- Updates (VECSET/EVENT) must go through a write coordinator (not shown)

**Note:** nvecd currently does not support multi-master replication. For write operations, use a single primary instance or implement application-level write coordination.

## Performance Benchmarks

### Typical Query Performance

**Environment:**
- Apple M1 (NEON SIMD)
- 10K vectors (768-dim)
- 50K events across 5K contexts

**Results:**

| Query Type | Mode | Cold (no cache) | Warm (cached) |
|------------|------|-----------------|---------------|
| SIM (ID search) | vectors | 2.5ms | 0.05ms |
| SIM (ID search) | events | 0.8ms | 0.05ms |
| SIM (ID search) | fusion | 3.2ms | 0.05ms |
| SIMV (vector query) | vectors | 2.8ms | 0.05ms |

**Key Findings:**
- SIMD acceleration: 3-6x faster than scalar
- Cache hit latency: ~50 microseconds (50× faster than cold)
- Fusion queries: Only 25% slower than pure vector search

### Scaling Characteristics

**Query latency vs dataset size (vectors mode, 768-dim, AVX2):**

| Dataset Size | Query Time |
|--------------|------------|
| 1K vectors | 0.3ms |
| 10K vectors | 2.5ms |
| 100K vectors | 25ms |
| 1M vectors | 250ms |

**Linear scaling:** O(n) with dataset size (brute-force search)

**Cache effectiveness:**

| Hit Rate | Latency Reduction |
|----------|-------------------|
| 50% | 2x faster |
| 80% | 5x faster |
| 90% | 10x faster |

## Troubleshooting

### Query is Slower Than Expected

**1. Check SIMD is enabled:**

```bash
# Look for SIMD mode in logs
grep "Vector SIMD" /path/to/nvecd.log
# → Vector SIMD: NEON  (good)
# → Vector SIMD: Scalar  (slow, check CPU)
```

**2. Enable debug mode:**

```bash
echo "DEBUG ON" | nc localhost 11017
echo "SIM vec123 10 vectors" | nc localhost 11017
# → Shows query timing breakdown
```

**3. Check cache hit rate:**

```bash
echo "CACHE STATS" | nc localhost 11017
# If hit_rate < 0.5, consider:
# - Increasing max_memory_mb
# - Lowering min_query_cost_ms
```

### High Memory Usage

**1. Check memory breakdown:**

```bash
curl http://localhost:8080/health/detail | jq '.memory'
```

**2. Reduce cache size:**

```yaml
cache:
  max_memory_mb: 16    # Reduce from 32MB
```

**3. Reduce event buffer:**

```yaml
events:
  ctx_buffer_size: 500  # Reduce from 1000
```

**4. Use snapshot + restart:**

```bash
echo "DUMP SAVE /backup/snapshot.dmp" | nc localhost 11017
# Restart server with clean state
```

### Cache Not Working

**1. Verify cache is enabled:**

```yaml
cache:
  enabled: true    # Must be true
```

**2. Check min_query_cost_ms threshold:**

```yaml
cache:
  min_query_cost_ms: 10.0
```

Fast queries (<10ms) won't be cached by default.

**3. Monitor cache statistics:**

```bash
echo "CACHE STATS" | nc localhost 11017
```

If `total_queries > 0` but `cache_hits = 0`, queries may be too fast to cache.

## Best Practices

### Do's

✅ **Use SIMD-compatible CPUs** (ARM64 with NEON, x86_64 with AVX2)
✅ **Enable caching** for production workloads
✅ **Monitor cache hit rate** and adjust settings
✅ **Use smaller dimensions** (384 instead of 1536) if latency matters
✅ **Batch vector uploads** using client library
✅ **Take regular snapshots** for disaster recovery

### Don'ts

❌ **Don't disable cache** in production
❌ **Don't use 4096-dimensional vectors** (slow, memory-intensive)
❌ **Don't open/close connections** for each query
❌ **Don't run without monitoring** (`/health/detail`)
❌ **Don't use SIMV** when SIM is sufficient (SIMV computes vector hash)

## Conclusion

Nvecd provides high-performance vector search through:

1. **SIMD acceleration** (3-8× faster vector operations)
2. **Smart caching** (50-100× faster for repeated queries)
3. **Efficient data structures** (compressed events, optimized layouts)

For typical workloads with 10K-100K vectors:
- **Cold queries**: 2-25ms
- **Cached queries**: 0.05ms (50 microseconds)
- **Cache hit rate**: 70-90% with proper tuning

Follow the optimization tips in this guide to achieve sub-millisecond query latency in production.
