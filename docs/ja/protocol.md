# プロトコルリファレンス

Nvecd はシンプルなテキストベースのプロトコルを TCP 経由で使用します（memcached/Redis と同様）。

## 接続

TCP 経由で Nvecd に接続：

```bash
telnet localhost 11017
```

または `nc` を使用：

```bash
nc localhost 11017
```

## コマンドフォーマット

コマンドはテキストベースで、1 行に 1 コマンドです。レスポンスは改行で終了します。

---

## EVENT コマンド

コンテキストと ID を関連付けるイベントを記録します。

### 構文

```
EVENT <ctx> <id> <score>
```

- `<ctx>`: コンテキスト識別子（文字列）
- `<id>`: アイテム識別子（文字列）
- `<score>`: イベントスコア（整数、0-100）

### 例

```
EVENT user123 item456 95
```

### レスポンス

```
OK
```

### エラーレスポンス

- `(error) Invalid score: must be 0-100`
- `(error) Context buffer overflow`

---

## VECSET コマンド

アイテムのベクトルを登録または更新します。

### 構文

```
VECSET <id> <dimension> <v1> <v2> ... <vN>
```

- `<id>`: アイテム識別子（文字列）
- `<dimension>`: ベクトル次元数（整数）
- `<v1> <v2> ... <vN>`: ベクトル成分（浮動小数点数）

### 例

```
VECSET item456 3 0.1 0.5 0.8
```

### レスポンス

```
OK
```

### エラーレスポンス

- `(error) Dimension mismatch: expected 768, got 512`
- `(error) Invalid vector format`

---

## SIM コマンド

既存アイテムのベクトルと共起データに基づいて類似アイテムを検索します。

### 構文

```
SIM <id> <top_k> <mode>
```

- `<id>`: アイテム識別子（文字列）
- `<top_k>`: 返す結果数（整数）
- `<mode>`: 類似度モード（`dot`, `cosine`, または `fusion`）

### 例

```
SIM item456 10 fusion
```

### レスポンス

```
OK RESULTS <count>
<id1> <score1>
<id2> <score2>
...
```

例：
```
OK RESULTS 3
item789 0.9245
item101 0.8932
item202 0.8567
```

### エラーレスポンス

- `(error) Item not found: item456`
- `(error) Invalid mode: must be dot, cosine, or fusion`

---

## SIMV コマンド

クエリベクトルに基づいて類似アイテムを検索します。

### 構文

```
SIMV <dimension> <v1> <v2> ... <vN> <top_k> <mode>
```

- `<dimension>`: ベクトル次元数（整数）
- `<v1> <v2> ... <vN>`: クエリベクトル成分（浮動小数点数）
- `<top_k>`: 返す結果数（整数）
- `<mode>`: 類似度モード（`dot` または `cosine`）

### 例

```
SIMV 3 0.1 0.5 0.8 10 cosine
```

### レスポンス

```
OK RESULTS <count>
<id1> <score1>
<id2> <score2>
...
```

### エラーレスポンス

- `(error) Dimension mismatch`
- `(error) Invalid mode: must be dot or cosine (fusion not supported for SIMV)`

---

## INFO コマンド

サーバー情報と統計を取得します（Redis スタイル形式）。

### 構文

```
INFO
```

### レスポンス

Redis スタイルのキーバリュー形式でサーバー情報を返します：

```
OK INFO

# Server
version: 0.1.0
uptime_seconds: 3600

# Stats
total_commands_processed: 10000
total_connections_received: 150

# Commandstats
cmd_event: 5000
cmd_vecset: 2000
cmd_sim: 2500
cmd_simv: 500

# Memory
event_store_contexts: 100
event_store_events: 5000
vector_store_vectors: 2000
co_occurrence_pairs: 1500
```

---

## CONFIG コマンド

設定管理コマンドです。

### CONFIG HELP

利用可能な設定コマンドを表示します。

```
CONFIG HELP
```

### CONFIG SHOW

現在の設定を表示します。

```
CONFIG SHOW
```

レスポンス：
```
OK CONFIG
server.host: 127.0.0.1
server.port: 11017
server.thread_pool_size: 4
event_store.ctx_buffer_size: 100
event_store.decay_factor: 0.95
vector_store.default_dimension: 768
...
```

### CONFIG VERIFY

設定ファイルの構文を検証します。

```
CONFIG VERIFY
```

レスポンス：
```
OK Configuration is valid
```

---

## DUMP コマンド

スナップショット永続化コマンドです。

### DUMP SAVE

スナップショットをディスクに保存します。

```
DUMP SAVE [filepath]
```

`filepath` を省略すると、タイムスタンプ付きのファイル名が自動生成されます。

レスポンス：
```
OK Snapshot saved: /path/to/dump_20250118_120000.nvec
```

### DUMP LOAD

スナップショットをディスクから読み込みます。

```
DUMP LOAD <filepath>
```

レスポンス：
```
OK Snapshot loaded: 5000 events, 2000 vectors
```

### DUMP VERIFY

スナップショットの整合性を検証します。

```
DUMP VERIFY <filepath>
```

レスポンス：
```
OK Snapshot is valid (CRC32: 0x12345678)
```

### DUMP INFO

スナップショットのメタデータを表示します。

```
DUMP INFO <filepath>
```

レスポンス：
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

## DEBUG コマンド

現在の接続のデバッグ出力を有効化・無効化します。

### DEBUG ON

デバッグログを有効化します。

```
DEBUG ON
```

レスポンス：
```
OK Debug mode enabled
```

### DEBUG OFF

デバッグログを無効化します。

```
DEBUG OFF
```

レスポンス：
```
OK Debug mode disabled
```

---

## エラーレスポンス

すべてのエラーは以下の形式に従います：

```
(error) <error_message>
```

例：
- `(error) Unknown command: FOO`
- `(error) Invalid argument count`
- `(error) Item not found: item123`
- `(error) Dimension mismatch: expected 768, got 512`

---

## 類似度モード

### `dot` - 内積

ベクトル間の生の内積です。値が大きいほど類似度が高いことを示します。

### `cosine` - コサイン類似度

正規化された内積（範囲：-1.0 ～ 1.0）。ベクトル間の角度を測定します。

### `fusion` - フュージョン検索（SIM のみ）

ベクトル類似度とイベントデータからの共起スコアを組み合わせます。ハイブリッド推薦システムに最適です。

**注意**: `fusion` モードは `SIM` コマンドでのみ利用可能です（`SIMV` では不可）。

---

## 次のステップ

- 設定オプションについては [設定ガイド](configuration.md) を参照
- 永続化の詳細については [スナップショット管理](snapshot.md) を参照
