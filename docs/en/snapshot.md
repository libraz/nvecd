# Snapshot Management

Nvecd provides snapshot commands for backing up and restoring your server state. Snapshots capture the entire data including event store, vector store, and co-occurrence index.

## Overview

A snapshot is a single binary file (`.nvec`) that contains:
- Complete event store data (contexts, events, ring buffers)
- Vector store data (all registered vectors)
- Co-occurrence index (relationship scores)
- Integrity checksums (CRC32) for corruption detection

## Commands

### DUMP SAVE - Create Snapshot

Save the current server state to a snapshot file.

**Syntax:**
```
DUMP SAVE [filepath]
```

**Parameters:**
- `[filepath]` (optional): Path where the snapshot file will be saved. If omitted, a timestamped filename is automatically generated.

**Examples:**
```
-- Manual snapshot
DUMP SAVE /var/lib/nvecd/snapshots/backup.nvec

-- Auto-generated filename
DUMP SAVE
```

**Response:**
```
OK Snapshot saved: /var/lib/nvecd/snapshots/backup.nvec
event_store_contexts: 100
vector_store_vectors: 2000
co_occurrence_pairs: 1500
size: 1048576 bytes
```

**Auto-generated filename format**: `dump_YYYYMMDD_HHMMSS.nvec`

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
OK Snapshot loaded: 5000 events, 2000 vectors
event_store_contexts: 100
vector_store_vectors: 2000
co_occurrence_pairs: 1500
```

**Important Notes:**
- Loading a snapshot **replaces** all current data
- The server will be in read-only mode during load
- All existing connections receive an error during load

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
OK Snapshot is valid (CRC32: 0x12345678)
status: valid
crc: verified
size: verified
```

**Response (Failure):**
```
(error) CRC mismatch: file may be corrupted
expected: 0x12345678
actual: 0x87654321
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
- Filenames: `auto_YYYYMMDD_HHMMSS.snapshot`
- Old snapshots are automatically cleaned up (keeps last `retain` snapshots)
- Manual snapshots are **not** affected by cleanup

---

## Best Practices

### Backup Strategy

1. **Regular automated snapshots**
   - Set `interval_sec` to match your RPO (recovery point objective)
   - Typical values: 3600 (1h), 7200 (2h), 14400 (4h)

2. **Manual snapshots before critical operations**
   ```
   DUMP SAVE /backup/before_upgrade.nvec
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
nvecd-test -c config.yaml
> DUMP LOAD /backup/snapshot.nvec
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

**Error**: `(error) Cannot save snapshot: read-only mode`
- **Cause**: Server is in read-only mode (loading in progress)
- **Solution**: Wait for load to complete

**Error**: `(error) Permission denied`
- **Cause**: No write permission for `snapshot.dir`
- **Solution**: Check directory permissions and ownership

### Snapshot Load Fails

**Error**: `(error) CRC mismatch: file may be corrupted`
- **Cause**: File corruption or incomplete write
- **Solution**: Use a different snapshot file

**Error**: `(error) Version mismatch: expected 1, got 2`
- **Cause**: Snapshot created by newer Nvecd version
- **Solution**: Upgrade Nvecd to compatible version

---

## Next Steps

- See [Configuration Guide](configuration.md) for snapshot settings
- See [Protocol Reference](protocol.md) for DUMP command syntax
- See [Installation Guide](installation.md) for deployment instructions
