# Snapshot Management

Nvecd provides snapshot commands for backing up and restoring your server state. Snapshots capture the entire data including event store, vector store, and co-occurrence index.

## Overview

A snapshot is a single binary file (`.nvec`) that contains:
- Complete event store data (contexts, events, ring buffers)
- Vector store data (all registered vectors)
- Co-occurrence index (relationship scores)
- Integrity checksums (CRC32) for corruption detection

## Snapshot Modes

Nvecd supports two snapshot modes, configured via `snapshot.mode`:

### Fork Mode (Default, Recommended)

Uses `fork()` to create a copy-on-write (COW) child process:

1. Server acquires exclusive write locks on all stores
2. `fork()` creates a child process (COW memory pages)
3. Server immediately releases locks and resumes serving requests
4. Child process serializes data to disk in the background
5. Child calls `_exit()` upon completion

**Advantages**:
- Near-zero downtime — parent resumes serving immediately after fork
- Consistent snapshot — child sees frozen memory state
- No extra memory needed (COW pages shared until modified)

**Trade-offs**:
- Requires OS support for `fork()` (Linux, macOS)
- Write operations after fork cause COW page copies (transient memory spike)

### Lock Mode

Uses a global write lock during the entire snapshot:

1. Server acquires exclusive write locks on all stores
2. Data is serialized directly to disk
3. Locks are released after writing completes

**Advantages**:
- Simpler implementation, no forked processes
- Predictable memory usage

**Trade-offs**:
- Write operations are blocked during the entire save
- Save duration scales with data size

### Configuration

```yaml
snapshot:
  mode: "fork"    # "fork" (COW, recommended) or "lock"
```

---

## Commands

### DUMP SAVE - Create Snapshot

Save the current server state to a snapshot file.

**Syntax:**
```
DUMP SAVE [filepath]
```

**Parameters:**
- `[filepath]` (optional): Where to write the snapshot. If omitted, the name comes from `snapshot.default_filename`.

**Examples:**
```
-- Explicit path
DUMP SAVE /var/lib/nvecd/snapshots/backup.nvec

-- Configured default name
DUMP SAVE
```

**Response:**
```
DUMP SAVE /var/lib/nvecd/snapshots/backup.nvec
→ OK DUMP_SAVED /var/lib/nvecd/snapshots/backup.nvec
```

**Filename resolution:** an argument-less `DUMP SAVE` writes to
`snapshot.default_filename` inside the snapshot directory — a fixed name, so
**repeating the command overwrites the same file**. It does not accumulate
history; that is the auto-snapshot path's job (`auto_*.nvec` kept according to
`snapshot.retain`). Only when `snapshot.default_filename` is empty does the name
fall back to the timestamped form `snapshot_YYYYMMDD_HHMMSS.dmp`.

**Path validation:** the configured name is validated, not trusted. It goes
through the same dump-path check a client-supplied path gets, so a
`default_filename` that is absolute or contains `..` is refused at save time
rather than resolved against the snapshot directory. `DUMP SAVE` then returns an
error instead of writing somewhere unexpected.

**Note:** When `snapshot.mode` is `fork` (default), DUMP SAVE starts a background snapshot via `fork()` and answers `OK DUMP_SAVE_STARTED`. The command returns immediately while the child process writes the snapshot. Use `DUMP STATUS` to check progress. Under `snapshot.mode: lock` the command blocks and `OK DUMP_SAVED` means the snapshot, its sidecar and the WAL checkpoint all completed.

---

### DUMP LOAD - Restore Snapshot

Load a snapshot file and restore the server state.

**Syntax:**
```
DUMP LOAD <filepath>
```

**Parameters:**
- `<filepath>`: Path to the snapshot file to load

**Examples:**
```
DUMP LOAD /var/lib/nvecd/snapshots/backup.nvec
```

**Response:**
```
DUMP LOAD /var/lib/nvecd/snapshots/backup.nvec
→ OK DUMP_LOADED /var/lib/nvecd/snapshots/backup.nvec
```

**Important Notes:**
- Loading a snapshot **replaces** all current data
- The server will be in read-only mode during load
- All existing connections receive an error during load
- A load is **fail-closed on invalid co-occurrence edges**: a snapshot carrying
  a self-edge or a non-finite score is refused whole rather than loaded with the
  bad edge dropped. Skipping it would let the file appear to load while the
  restored index differed from the one that was written, and such an edge
  corrupts the map during pruning and breaks the ordering of every later sort.
- Consequently **a snapshot written by an older build that already contains such
  an edge is no longer loadable.** This is intended. Startup recovery falls back
  to an older snapshot plus the WAL; a manual `DUMP LOAD` returns an error
  naming the offending edge, and the file has to be rebuilt from a source that
  never held one.

---

### DUMP VERIFY - Check Integrity

Verify the integrity of a snapshot file without loading it.

**Syntax:**
```
DUMP VERIFY <filepath>
```

**Parameters:**
- `<filepath>`: Path to the snapshot file to verify

**Examples:**
```
DUMP VERIFY /var/lib/nvecd/snapshots/backup.nvec
```

**Response (Success):**
```
DUMP VERIFY /var/lib/nvecd/snapshots/backup.nvec
→ OK DUMP_VERIFIED /var/lib/nvecd/snapshots/backup.nvec
```

**Response (Failure):**
```
→ ERROR Snapshot verification failed for /var/lib/nvecd/snapshots/backup.nvec: CRC mismatch
```

---

### DUMP INFO - Show Snapshot Metadata

Display metadata about a snapshot file without loading it.

**Syntax:**
```
DUMP INFO <filepath>
```

**Parameters:**
- `<filepath>`: Path to the snapshot file

**Examples:**
```
DUMP INFO /var/lib/nvecd/snapshots/backup.nvec
```

**Response:**
```
OK INFO
version: 1
event_store_count: 5000
vector_store_count: 2000
co_occurrence_count: 1500
file_size: 1048576
created_at: 2025-01-18T12:00:00
```

---

### DUMP STATUS - Check Background Snapshot Status

Check the status of the most recent background snapshot operation.

**Syntax:**
```
DUMP STATUS
```

**Response:**
```
OK DUMP_STATUS
status: idle
END
```

**Status Values:**
| Status | Description |
|--------|-------------|
| `idle` | No snapshot in progress |
| `in_progress` | Fork child is writing snapshot |
| `completed` | Last snapshot saved successfully |
| `failed` | Last snapshot failed (the `error` field carries the reason) |

**In-Progress Response:**
```
OK DUMP_STATUS
status: in_progress
filepath: /var/lib/nvecd/snapshots/dump_20250325_120000.nvec
pid: 12345
start_time: 1711360800
END
```

**Completed Response:**
```
OK DUMP_STATUS
status: completed
filepath: /var/lib/nvecd/snapshots/dump_20250325_120000.nvec
start_time: 1711360800
end_time: 1711360802
END
```

---

## Integrity Protection

### CRC32 Checksums

All snapshot files include CRC32 checksums:
- **File-level CRC**: Detects any corruption in the entire file
- **Section-level CRC**: Validates individual sections (event store, vector store, co-occurrence index)

### File Size Validation

The snapshot header includes the expected file size:
- Detects incomplete writes
- Catches network transfer failures
- Identifies truncated files

---

## Snapshot Format

### Version 1 Format

Current snapshot format (version 1):

```
┌─────────────────────────────────────┐
│ Header (40 bytes)                   │
│  - Magic number (4 bytes)           │
│  - Version (4 bytes)                │
│  - Flags (4 bytes)                  │
│  - Store counts (12 bytes)          │
│  - Timestamps (16 bytes)            │
├─────────────────────────────────────┤
│ Event Store Data                    │
│  - Contexts                         │
│  - Ring buffers                     │
│  - Event data                       │
├─────────────────────────────────────┤
│ Co-Occurrence Index Data            │
│  - ID pairs                         │
│  - Scores                           │
├─────────────────────────────────────┤
│ Vector Store Data                   │
│  - ID mappings                      │
│  - Dimension info                   │
│  - Vector data                      │
├─────────────────────────────────────┤
│ Footer (8 bytes)                    │
│  - File CRC32 (4 bytes)             │
│  - Reserved (4 bytes)               │
└─────────────────────────────────────┘
```

---

## Automated Snapshots

### Configuration

Enable automated snapshots in `config.yaml`:

```yaml
snapshot:
  dir: "/var/lib/nvecd/snapshots"
  interval_sec: 7200           # 2 hours
  retain: 5                    # Keep 5 snapshots
```

### Auto-snapshot Behavior

- Automatic snapshots are created on schedule
- Filenames: `auto_YYYYMMDD_HHMMSS.nvec`
- Cleanup keeps the `retain` newest `auto_*.nvec` files and removes the rest; `retain: 0` disables cleanup and keeps every file
- Manual snapshots are **not** affected by cleanup

---

## Best Practices

### Backup Strategy

1. **Regular automated snapshots**
   - Set `interval_sec` to match your RPO (recovery point objective)
   - Typical values: 3600 (1h), 7200 (2h), 14400 (4h)

2. **Manual snapshots before critical operations**
   ```
   DUMP SAVE before_upgrade.nvec
   ```

3. **Verify snapshots regularly**
   ```
   DUMP VERIFY /var/lib/nvecd/snapshots/auto_20250118_120000.snapshot
   ```

### Storage Recommendations

- Use dedicated backup volume for `snapshot.dir`
- Monitor disk space (snapshots can grow large)
- Consider off-site backup for disaster recovery
- Use file compression for long-term storage

### Testing Recovery

Regularly test snapshot restoration:

```bash
# Stop production server
systemctl stop nvecd

# Test load on a test server
nvecd -c config.yaml
> DUMP LOAD snapshot.nvec
> INFO
> (verify data integrity)
```

---

## Security Considerations

### File Permissions

Snapshot files may contain sensitive data. Protect with appropriate permissions:

```bash
# Recommended permissions
chmod 600 /var/lib/nvecd/snapshots/*.nvec
chown nvecd:nvecd /var/lib/nvecd/snapshots/*.nvec
```

### Path Traversal Protection

Nvecd prevents path traversal attacks:
- Rejects paths containing `..`
- Validates paths are within configured `snapshot.dir`
- Only allows saving/loading from whitelisted directories

---

## Troubleshooting

### Snapshot Save Fails

**Error**: `ERROR Cannot save snapshot: read-only mode`
- **Cause**: Server is in read-only mode (loading in progress)
- **Solution**: Wait for load to complete

**Error**: `ERROR Permission denied`
- **Cause**: No write permission for `snapshot.dir`
- **Solution**: Check directory permissions and ownership

### Snapshot Load Fails

**Error**: `ERROR CRC mismatch: file may be corrupted`
- **Cause**: File corruption or incomplete write
- **Solution**: Use a different snapshot file

**Error**: `ERROR Version mismatch: expected 1, got 2`
- **Cause**: Snapshot created by newer Nvecd version
- **Solution**: Upgrade Nvecd to compatible version

---

## Next Steps

- See [Configuration Guide](configuration.md) for snapshot settings
- See [Protocol Reference](protocol.md) for DUMP command syntax
- See [Installation Guide](installation.md) for deployment instructions
