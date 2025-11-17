# Getting Started with Nvecd

This guide helps you get started using Nvecd in your applications.

## Quick Start

### 1. Start the Server

```bash
# Start nvecd with configuration
nvecd -c config.yaml
```

### 2. Connect to the Server

```bash
# Using telnet
telnet localhost 11017

# Or using nc
nc localhost 11017
```

### 3. Register Vectors

```bash
# Register a vector for item "item1"
VECSET item1 3 0.1 0.5 0.8
# Response: OK

# Register more vectors
VECSET item2 3 0.2 0.4 0.9
VECSET item3 3 0.1 0.6 0.7
```

### 4. Record Events

```bash
# Record event: user "user123" interacted with "item1" with score 95
EVENT user123 item1 95
# Response: OK

EVENT user123 item2 80
EVENT user456 item1 90
EVENT user456 item3 85
```

### 5. Search for Similar Items

```bash
# Find similar items to "item1" using fusion mode
SIM item1 10 fusion
# Response:
# OK RESULTS 2
# item2 0.8523
# item3 0.7891
```

---

## Common Use Cases

### Use Case 1: Content Recommendation

Build a recommendation system combining content similarity and user behavior.

```bash
# 1. Register content vectors (from embeddings)
VECSET article123 768 0.123 0.456 ... (768 dimensions)
VECSET article456 768 0.789 0.234 ...

# 2. Track user engagement
EVENT user_alice article123 95  # Alice highly engaged with article123
EVENT user_alice article456 80
EVENT user_bob article123 90

# 3. Find similar articles (fusion mode combines vectors + engagement)
SIM article123 10 fusion
# Returns articles similar in content AND popular with similar users
```

### Use Case 2: Semantic Search

Pure vector-based similarity search.

```bash
# Register document vectors
VECSET doc1 384 0.1 0.2 ...
VECSET doc2 384 0.3 0.4 ...

# Search with query vector (from user's search)
SIMV 384 0.15 0.25 ... 10 cosine
# Returns documents semantically similar to the query
```

### Use Case 3: Collaborative Filtering

Recommend based on user behavior patterns.

```bash
# Track user interactions
EVENT user1 movie123 95
EVENT user1 movie456 80
EVENT user2 movie123 90
EVENT user2 movie789 85

# Register movie vectors (metadata embeddings)
VECSET movie123 128 ...
VECSET movie456 128 ...
VECSET movie789 128 ...

# Find movies similar to movie123 (fusion mode)
SIM movie123 10 fusion
# Recommends movies liked by users with similar taste
```

---

## Similarity Modes

### Dot Product (`dot`)

Raw dot product between vectors. Use when vectors are already normalized or when magnitude matters.

```bash
SIM item1 10 dot
SIMV 768 0.1 0.2 ... 10 dot
```

### Cosine Similarity (`cosine`)

Normalized similarity (range: -1.0 to 1.0). Use for general semantic similarity regardless of vector magnitude.

```bash
SIM item1 10 cosine
SIMV 768 0.1 0.2 ... 10 cosine
```

### Fusion Search (`fusion`) - Recommended

Combines vector similarity + co-occurrence from events. Use for hybrid recommendation systems.

```bash
SIM item1 10 fusion
# Note: Only available for SIM, not SIMV
```

**Fusion formula**: `score = alpha * vector_sim + beta * co_occurrence`

Configure weights in `config.yaml`:
```yaml
similarity:
  fusion_alpha: 0.6  # Vector similarity weight
  fusion_beta: 0.4   # Co-occurrence weight
```

---

## Best Practices

### 1. Vector Dimensions

Choose appropriate dimensions for your use case:
- **384**: MiniLM embeddings (lightweight)
- **768**: BERT embeddings (balanced)
- **1536**: OpenAI embeddings (high quality)

Set default dimension in config:
```yaml
vectors:
  default_dimension: 768
```

### 2. Event Scoring

Use consistent scoring (0-100):
- **90-100**: High engagement (purchase, long view time, high rating)
- **70-89**: Medium engagement (click, short view, moderate rating)
- **50-69**: Low engagement (impression, quick bounce)
- **Below 50**: Negative signals (if applicable)

### 3. Context Design

Design contexts based on your use case:
- **User-based**: `user_{id}` (track per-user behavior)
- **Session-based**: `session_{id}` (track per-session behavior)
- **Time-based**: `hourly_{timestamp}` (track time-windowed patterns)

### 4. Decay Configuration

Configure decay to handle evolving preferences:

```yaml
events:
  decay_interval_sec: 7200  # Decay every 2 hours
  decay_alpha: 0.95         # 5% decay per interval
```

- **Short decay**: Capture recent trends (news, trending topics)
- **Long decay**: Stable preferences (product recommendations)
- **No decay**: Historical analysis (`decay_interval_sec: 0`)

---

## Integration Examples

### Python Integration

> ⚠️ **Not Implemented Yet** - Python client library is planned for future releases.

Future usage:
```python
from nvecdclient import NvecdClient

client = NvecdClient(host='localhost', port=11017)
client.connect()

# Register vector
client.vecset('item1', [0.1, 0.5, 0.8])

# Record event
client.event('user123', 'item1', 95)

# Search
results = client.sim('item1', top_k=10, mode='fusion')
for item_id, score in results:
    print(f"{item_id}: {score}")
```

### Node.js Integration

> ⚠️ **Not Implemented Yet** - Node.js client library is planned for future releases.

Future usage:
```javascript
const NvecdClient = require('nvecdclient');

const client = new NvecdClient({ host: 'localhost', port: 11017 });
await client.connect();

// Register vector
await client.vecset('item1', [0.1, 0.5, 0.8]);

// Record event
await client.event('user123', 'item1', 95);

// Search
const results = await client.sim('item1', { topK: 10, mode: 'fusion' });
results.forEach(({ id, score }) => console.log(`${id}: ${score}`));
```

### Raw TCP Integration

Current method (any language with TCP sockets):

```python
import socket

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.connect(('localhost', 11017))

# Send command
sock.sendall(b'VECSET item1 3 0.1 0.5 0.8\n')
response = sock.recv(1024).decode().strip()
print(response)  # "OK"

# Record event
sock.sendall(b'EVENT user123 item1 95\n')
response = sock.recv(1024).decode().strip()
print(response)  # "OK"

# Search
sock.sendall(b'SIM item1 10 fusion\n')
response = sock.recv(4096).decode().strip()
print(response)  # "OK RESULTS 2\nitem2 0.8523\nitem3 0.7891"

sock.close()
```

---

## Monitoring and Debugging

### Server Status

Check server statistics:
```bash
INFO
```

Response includes:
- Uptime
- Total commands processed
- Memory usage
- Store sizes (contexts, vectors, co-occurrence pairs)

### Debug Mode

Enable debug logging for a connection:
```bash
DEBUG ON
# Now all commands will show additional debug information

DEBUG OFF
# Disable debug logging
```

### Configuration Verification

Verify configuration is valid:
```bash
CONFIG VERIFY
```

Show current configuration:
```bash
CONFIG SHOW
```

---

## Production Deployment

### 1. Security

Configure network access in `config.yaml`:
```yaml
network:
  allow_cidrs:
    - "10.0.0.0/8"      # Allow private network
    - "172.16.0.0/12"   # Allow Docker network
```

### 2. Persistence

Enable automatic snapshots:
```yaml
snapshot:
  dir: "/var/lib/nvecd/snapshots"
  interval_sec: 7200  # Snapshot every 2 hours
  retain: 5           # Keep 5 snapshots
```

Manual snapshot before maintenance:
```bash
DUMP SAVE /backup/before_maintenance.nvec
```

### 3. Performance Tuning

Configure thread pool:
```yaml
performance:
  thread_pool_size: 16        # Match CPU cores
  max_connections: 5000       # Based on system limits
  connection_timeout_sec: 600
```

---

## Next Steps

- See [Protocol Reference](protocol.md) for all available commands
- See [Configuration Guide](configuration.md) for tuning options
- See [Performance Guide](performance.md) for optimization tips
- See [HTTP API Reference](http-api.md) for REST API usage (when available)
