# プロトコルリファレンス

Nvecd はシンプルなテキストベースのプロトコルを TCP 経由で使用します（Redis/Memcached と同様）。MygramDB 互換の管理コマンドも備えています。

**プロトコル形式**: テキストベース、行区切り、UTF-8 エンコーディング

## 接続

TCP 経由で nvecd に接続：

```bash
# netcat を使用
nc localhost 11017

# telnet を使用
telnet localhost 11017
```

---

## プロトコル形式

- **トランスポート**: テキストベースの行プロトコル（UTF-8）
- **リクエスト**: `COMMAND args...\r\n`（`\r\n` と `\n` の両方を受け付けます）
- **レスポンス**: `OK data...\r\n` または `ERROR message\r\n`
- **最大リクエストサイズ**: 16MB（設定変更可能）

### レスポンス形式

**成功**:
```
OK [data]\r\n
```
または Redis スタイル: `+OK [data]\r\n`

**エラー**:
```
ERROR <message>\r\n
```
または Redis スタイル: `-ERR <message>\r\n` または `(error) <message>\r\n`

---

## コマンドカテゴリ

- **コアコマンド**: EVENT, VECSET, SIM, SIMV（nvecd 固有）
- **管理コマンド**: INFO, CONFIG, DUMP, DEBUG（MygramDB 互換）
- **キャッシュコマンド**: CACHE（クエリ結果キャッシュ管理）

---

## コアコマンド

### EVENT — 共起イベントの取り込み

コンテキストと ID を関連付けるイベントを記録します。3 種類のイベントタイプをサポート：
- **ADD**: ストリーム型イベント（クリック、閲覧）- 時間窓ベースの重複排除
- **SET**: 状態型イベント（いいね、ブックマーク、評価）- 冪等な更新
- **DEL**: 削除型イベント（いいね解除、ブックマーク削除）- 冪等な削除

**構文**:
```
EVENT <ctx> ADD <id> <score>
EVENT <ctx> SET <id> <score>
EVENT <ctx> DEL <id>
```

**パラメータ**:
- `<ctx>`: コンテキスト識別子（文字列、例: ユーザーID、セッションID）
- `<type>`: イベントタイプ: `ADD`, `SET`, `DEL`
- `<id>`: アイテム識別子（文字列、例: アイテムID、アクションID）
- `<score>`: イベントスコア（整数、0-100）— ADD/SET では必須、DEL では無視

**例**:

```bash
# ストリーム型イベント（クリック追跡）
EVENT user123 ADD view:item456 95
→ OK

# 状態型イベント（いいね ON）
EVENT user123 SET like:item456 100
→ OK

# 状態型イベント（いいね OFF）
EVENT user123 SET like:item456 0
→ OK

# 重み付きブックマーク（重要度: 高）
EVENT user123 SET bookmark:item789 100
→ OK

# ブックマーク重要度変更（中）
EVENT user123 SET bookmark:item789 50
→ OK

# ブックマーク削除
EVENT user123 DEL bookmark:item789
→ OK
```

**イベントタイプの動作**:

| タイプ | 用途 | 重複排除 | 例 |
|--------|------|---------|-----|
| **ADD** | ストリーム型イベント（クリック、閲覧、再生） | 時間窓ベース（デフォルト: 60秒） | `EVENT user1 ADD view:item1 100` |
| **SET** | 状態型イベント（いいね、ブックマーク、評価） | 同じ値 = 重複（冪等） | `EVENT user1 SET like:item1 100` |
| **DEL** | 削除型イベント | 既に削除済み = 重複（冪等） | `EVENT user1 DEL like:item1` |

**冪等性の保証**:

```bash
# SET は同じ値に対して冪等
EVENT user1 SET like:item1 100
EVENT user1 SET like:item1 100  # 重複、無視される
→ OK（両方成功、2回目は重複排除）

# SET は状態遷移を許可
EVENT user1 SET bookmark:item1 100  # 重要度: 高
EVENT user1 SET bookmark:item1 50   # 重要度: 中（格納）
EVENT user1 SET bookmark:item1 50   # 重複（無視）
→ OK

# DEL は冪等
EVENT user1 DEL like:item1
EVENT user1 DEL like:item1  # 既に削除済み、無視される
→ OK
```

**エラーレスポンス**:
- `(error) Invalid EVENT type: <type> (must be ADD, SET, or DEL)`
- `(error) EVENT ADD requires 4 arguments: <ctx> ADD <id> <score>`
- `(error) EVENT SET requires 4 arguments: <ctx> SET <id> <score>`
- `(error) EVENT DEL requires 3 arguments: <ctx> DEL <id>`
- `(error) Invalid score: must be integer`
- `(error) Context cannot be empty`
- `(error) ID cannot be empty`

**注意事項**:
- イベントはコンテキストごとのリングバッファに格納されます（サイズ: `events.ctx_buffer_size`）
- 重複排除キャッシュサイズ: `events.dedup_cache_size`（デフォルト: 10,000 エントリ）
- ADD タイプの時間窓: `events.dedup_window_sec`（デフォルト: 60秒）
- SET/DEL は最後の値を追跡して冪等性を保証（時間窓なし）
- 同じコンテキスト内の ID 間で共起スコアが自動的に追跡されます
- スコアは `events.decay_interval_sec` と `events.decay_alpha` に基づいて減衰します

---

### VECSET — ベクトルの登録

アイテムのベクトルを登録または更新します。

**構文**:
```
VECSET <id> <f1> <f2> ... <fN>
```

**パラメータ**:
- `<id>`: アイテム識別子（文字列）
- `<f1> <f2> ... <fN>`: ベクトル成分（浮動小数点数）

**例**:
```
VECSET item456 0.1 0.5 0.8
→ OK
```

**768 次元ベクトルの例**:
```
VECSET item789 0.11 0.98 -0.22 0.44 ... (768 個の値)
→ OK
```

**エラーレスポンス**:
- `(error) Dimension mismatch: expected 768, got 512`
- `(error) Invalid vector format`
- `(error) Invalid argument count`

**注意事項**:
- 次元数は値の数から自動検出されます
- すべてのベクトルは同じ次元数である必要があります（デフォルト: 768、`vectors.default_dimension` で設定変更可能）
- ベクトルは `vectors.distance_metric` の設定に基づいて自動的に正規化されます

---

### SIM — ID による類似度検索

既存アイテムのベクトルと共起データに基づいて類似アイテムを検索します。

**構文**:
```
SIM <id> <top_k> [using=events|vectors|fusion]
```

**パラメータ**:
- `<id>`: アイテム識別子（文字列）
- `<top_k>`: 返す結果数（整数、最大: `similarity.max_top_k`）
- `using=`（省略可能）: 検索モード
  - `events`: 共起ベース（イベントデータのみ）
  - `vectors`: ベクトル距離ベース（ベクトルデータのみ）
  - `fusion`（デフォルト）: 共起 x ベクトルのハイブリッド

**レスポンス形式**:
```
OK RESULTS <count>
<id1> <score1>
<id2> <score2>
...
```

**例（fusion モード）**:
```
SIM item456 10 using=fusion
→ OK RESULTS 3
item789 0.9245
item101 0.8932
item202 0.8567
```

**例（events のみ）**:
```
SIM item456 10 using=events
→ OK RESULTS 2
item101 0.95
item789 0.87
```

**例（vectors のみ）**:
```
SIM item456 10 using=vectors
→ OK RESULTS 3
item789 0.9245
item202 0.8932
item555 0.8567
```

**エラーレスポンス**:
- `(error) Item not found: item456`
- `(error) Invalid mode: must be events, vectors, or fusion`
- `(error) Invalid top_k: must be > 0 and <= 1000`

**注意事項**:
- fusion モードはベクトル類似度（重み: `similarity.fusion_alpha`）と共起スコア（重み: `similarity.fusion_beta`）を組み合わせます
- クエリコストが `cache.min_query_cost_ms` を超えた場合、結果がキャッシュされます（キャッシュが有効な場合）
- キャッシュは VECSET（vectors モード向け）または EVENT（fusion モード向け）で無効化されます

---

### SIMV — ベクトルによる類似度検索

クエリベクトルに基づいて類似アイテムを検索します。

**構文**:
```
SIMV <top_k> <f1> <f2> ... <fN>
```

**パラメータ**:
- `<top_k>`: 返す結果数（整数）
- `<f1> <f2> ... <fN>`: クエリベクトル成分（浮動小数点数）

**レスポンス形式**:
```
OK RESULTS <count>
<id1> <score1>
<id2> <score2>
...
```

**例**:
```
SIMV 5 0.1 0.9 -0.2 0.5
→ OK RESULTS 2
item789 0.98
item101 0.82
```

**エラーレスポンス**:
- `(error) Dimension mismatch: expected 768, got 512`
- `(error) Invalid vector format`
- `(error) Invalid top_k`

**注意事項**:
- 次元数は値の数から自動検出されます
- ベクトル類似度のみが使用されます（クエリベクトルでは fusion モードは非対応）
- クエリコストが `cache.min_query_cost_ms` を超えた場合、結果がキャッシュされます

---

## 管理コマンド（MygramDB 互換）

### INFO — サーバー統計

包括的なサーバー情報と統計を取得します（Redis スタイル形式）。

**構文**:
```
INFO
```

**レスポンス**:
```
OK INFO

# Server
version: 0.1.0
uptime_seconds: 3600

# Stats
total_commands_processed: 100000
total_connections_received: 150

# Memory
used_memory_bytes: 536870912
used_memory_human: 512.00 MB
memory_health: HEALTHY

# Data
id_count: 12345
ctx_count: 6789
vector_count: 12000
event_count: 987654

# Commandstats
cmd_event: 50000
cmd_vecset: 20000
cmd_sim: 25000
cmd_simv: 5000
```

**Memory Health**:
- `HEALTHY`: システムメモリの空き容量が 20% 以上
- `WARNING`: 空き容量が 10-20%
- `CRITICAL`: 空き容量が 10% 未満

---

### CONFIG — 設定管理

**コマンド**:
```
CONFIG HELP [path]
CONFIG SHOW [path]
CONFIG VERIFY
```

#### CONFIG HELP

設定ドキュメントを表示します。

**例**:
```
CONFIG HELP events
→ +OK
events:
  ctx_buffer_size: Ring buffer size per context (default: 50)
  decay_interval_sec: Decay interval in seconds (default: 3600)
  decay_alpha: Decay factor 0.0-1.0 (default: 0.99)
```

#### CONFIG SHOW

現在の設定を表示します（パスワードはマスクされます）。

**例**:
```
CONFIG SHOW events.ctx_buffer_size
→ +OK
events:
  ctx_buffer_size: 50
```

#### CONFIG VERIFY

設定ファイルを検証します（サーバー起動前にも使用可能）。

**レスポンス**:
```
+OK Configuration is valid
```

---

### DUMP — スナップショット管理

**コマンド**:
```
DUMP SAVE [<filepath>]
DUMP LOAD [<filepath>]
DUMP VERIFY [<filepath>]
DUMP INFO [<filepath>]
```

単一バイナリの `.dmp` 形式、MygramDB 互換。

#### DUMP SAVE

完全なスナップショットをディスクに保存します。

**例**:
```
DUMP SAVE /data/nvecd.dmp
→ +OK Snapshot saved: /data/nvecd.dmp (512.3 MB) in 2.35s
```

**ファイルパスを省略した場合（自動生成名）**:
```
DUMP SAVE
→ +OK Snapshot saved: /var/lib/nvecd/snapshots/auto_20251118_143000.dmp (512.3 MB) in 2.35s
```

#### DUMP LOAD

スナップショットをディスクから読み込みます（読み込み中はサーバーが読み取り専用になります）。

**例**:
```
DUMP LOAD /data/nvecd.dmp
→ +OK Snapshot loaded: /data/nvecd.dmp (5000 events, 2000 vectors) in 1.23s
```

**エラーレスポンス**:
- `(error) File not found: /data/nvecd.dmp`
- `(error) CRC mismatch: file may be corrupted`
- `(error) Unsupported snapshot version`

#### DUMP VERIFY

スナップショットの整合性を読み込みなしで検証します。

**例**:
```
DUMP VERIFY /data/nvecd.dmp
→ +OK Snapshot is valid (CRC32: 0x12345678)
```

#### DUMP INFO

スナップショットのメタデータ（バージョン、サイズ、CRC32、レコード数）を表示します。

**例**:
```
DUMP INFO /data/nvecd.dmp
→ +OK INFO
version: 1
flags: 0
timestamp: 2025-11-18T14:30:00Z
event_store_count: 5000
co_occurrence_count: 1500
vector_store_count: 2000
file_size_bytes: 536870912
file_size_human: 512.00 MB
crc32: 0x12345678
```

---

### DEBUG — デバッグモード

接続ごとのデバッグモードです。SIM コマンドの詳細な実行情報を表示します。

**コマンド**:
```
DEBUG ON
DEBUG OFF
```

#### DEBUG ON

この接続のデバッグログを有効化します。

**例**:
```
DEBUG ON
→ OK Debug mode enabled
```

#### DEBUG OFF

この接続のデバッグログを無効化します。

**例**:
```
DEBUG OFF
→ OK Debug mode disabled
```

**デバッグ出力例**（DEBUG ON 時）:
```
SIM item456 10 using=fusion
→ OK RESULTS 3
item789 0.9245
item101 0.8932
item202 0.8567

# DEBUG
query_time_us: 850
event_search_time_us: 320
vector_search_time_us: 410
fusion_time_us: 120
mode: fusion
event_candidates: 15
vector_candidates: 12
```

---

## キャッシュコマンド

### CACHE — キャッシュ管理

クエリ結果キャッシュの管理コマンドです。

**コマンド**:
```
CACHE STATS
CACHE CLEAR
CACHE ENABLE
CACHE DISABLE
```

#### CACHE STATS

詳細なキャッシュ統計を返します。

**レスポンス**:
```
OK CACHE_STATS
total_queries: 1250
cache_hits: 985
cache_misses: 265
cache_misses_invalidated: 45
cache_misses_not_found: 220
hit_rate: 0.7880
current_entries: 342
current_memory_bytes: 12845632
current_memory_mb: 12.25
evictions: 15
avg_hit_latency_ms: 0.125
avg_miss_latency_ms: 2.450
time_saved_ms: 2418.75
```

**統計フィールド**:
- `total_queries`: キャッシュルックアップの総数
- `cache_hits`: キャッシュヒット数
- `cache_misses`: ミスの合計（invalidated + not found）
- `cache_misses_invalidated`: 無効化（VECSET/EVENT）によるミス
- `cache_misses_not_found`: キーがキャッシュにない、TTL 期限切れ、またはデコンプレッション失敗によるミス
- `hit_rate`: キャッシュヒット率（0.0 ~ 1.0）
- `current_entries`: キャッシュされたエントリ数
- `current_memory_mb`: 現在のキャッシュメモリ使用量
- `evictions`: LRU エビクション数
- `avg_hit_latency_ms`: ヒット時の平均キャッシュルックアップレイテンシ
- `avg_miss_latency_ms`: ミス時の平均キャッシュルックアップレイテンシ
- `time_saved_ms`: キャッシュヒットにより節約されたクエリ時間の合計

#### CACHE CLEAR

すべてのキャッシュエントリをクリアします。

**レスポンス**:
```
OK CACHE CLEARED
```

#### CACHE ENABLE

キャッシュを有効化します（既に初期化済みの場合は no-op）。

**レスポンス**:
```
OK CACHE ENABLED
```

**エラー**（起動時にキャッシュが初期化されていない場合）:
```
-ERR Cache was not initialized at startup
```

**注意**: キャッシュは起動時に config.yaml で有効化されている必要があります。ランタイムでの有効化は、設定で `cache.enabled=true` が設定されている場合のみ可能です。

#### CACHE DISABLE

ランタイムでのキャッシュ無効化は**サポートされていません**。

**レスポンス**:
```
-ERR Runtime cache disable not supported. Set cache.enabled=false in config and restart.
```

**キャッシュの動作**:
- SIM/SIMV のクエリ結果は `query_cost_ms >= min_query_cost_ms` の場合にキャッシュされます
- キャッシュエントリは VECSET（SIM クエリ向け）および EVENT（fusion クエリ向け）で無効化されます
- `current_memory_bytes >= max_memory_bytes` に達すると LRU エビクションが発生します
- メモリ使用量を削減するため、結果は LZ4 で圧縮されます

**設定**（`config.yaml`）:
```yaml
cache:
  enabled: true               # キャッシュの有効化/無効化
  max_memory_mb: 32           # キャッシュの最大メモリ
  min_query_cost_ms: 10.0     # キャッシュする最小クエリコスト
  ttl_seconds: 3600           # キャッシュエントリの TTL（0 = TTL なし）
  compression_enabled: true   # LZ4 圧縮の有効化
```

---

## エラーレスポンス

すべてのエラーは一貫した形式に従います：

```
(error) <error_message>
```

または Redis スタイル：

```
-ERR <error_message>
```

**一般的なエラー例**:
- `(error) Unknown command: FOO`
- `(error) Invalid argument count`
- `(error) Item not found: item123`
- `(error) Dimension mismatch: expected 768, got 512`
- `(error) Invalid score: must be 0-100`
- `(error) File not found: /data/dump.dmp`
- `(error) CRC mismatch: file may be corrupted`

---

## 類似度モード

### `events` - イベントベース（共起）

イベントデータの共起スコアのみを使用します。コンテンツ特徴量なしの協調フィルタリングに最適です。

**ユースケース**: 「このアイテムに対してインタラクションしたユーザーは、こちらにもインタラクションしています...」

### `vectors` - ベクトルベース

ベクトル類似度（内積またはコサイン）のみを使用します。コンテンツベースの推薦に最適です。

**ユースケース**: 「類似したコンテンツ/特徴量を持つアイテム...」

### `fusion` - フュージョン検索（SIM のみ）

ベクトル類似度（重み: `similarity.fusion_alpha`）と共起スコア（重み: `similarity.fusion_beta`）を組み合わせます。

**ユースケース**: コンテンツ類似度 + ユーザー行動を組み合わせたハイブリッド推薦。

**計算式**:
```
fusion_score = (alpha x vector_similarity) + (beta x co_occurrence_score)
ここで alpha + beta = 1.0
```

**注意**: `fusion` モードは `SIM` コマンドでのみ利用可能です（`SIMV` では不可）。

---

## ベストプラクティス

### パフォーマンスのヒント

1. **適切な top_k を使用する**: 値が小さいほど高速です
2. **キャッシュを有効化する**: 繰り返しクエリには `cache.enabled=true` を設定してください
3. **fusion の重みを調整する**: ユースケースに応じて `fusion_alpha` と `fusion_beta` を調整してください
4. **コールドアイテムには events モードを使用する**: ベクトルのないアイテムでもイベント経由で推薦可能です
5. **キャッシュヒット率を監視する**: `CACHE STATS` でパフォーマンスを確認してください

### データ管理

1. **定期的なスナップショット**: バックアップには `DUMP SAVE` を使用してください
2. **スナップショットの検証**: 読み込み前に `DUMP VERIFY` を使用してください
3. **メモリの監視**: `INFO` でメモリ使用量を追跡してください
4. **減衰の設定**: データの鮮度要件に応じて `decay_interval_sec` を調整してください

### デバッグ

1. **DEBUG モードを有効化する**: `DEBUG ON` でクエリ実行の詳細を確認できます
2. **INFO の統計を確認する**: コマンド数とパフォーマンスを監視してください
3. **小さなデータセットでテストする**: スケールアップ前に動作を検証してください

---

## 次のステップ

- 設定オプションについては [設定ガイド](configuration.md) を参照
- 永続化の詳細については [スナップショット管理](snapshot.md) を参照
- プログラムからのアクセスについては [クライアントライブラリガイド](libnvecdclient.md) を参照
- 最適化のヒントについては [パフォーマンスガイド](performance.md) を参照
