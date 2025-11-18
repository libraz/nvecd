# 設定ガイド

このガイドでは、Nvecd で利用可能なすべての設定オプションについて説明します。

## 設定ファイル

Nvecd は設定に YAML 形式を使用します。サンプル設定ファイルは `examples/config.yaml` にあります。

## 基本的な使い方

```bash
# 設定ファイルを指定して nvecd を起動
nvecd -c /path/to/config.yaml
```

---

## 設定セクション

### イベントストア設定

イベント追跡と共起インデックスの動作を制御します。

```yaml
events:
  ctx_buffer_size: 50          # コンテキストごとのリングバッファサイズ
  decay_interval_sec: 3600     # 減衰間隔（秒）
  decay_alpha: 0.99            # 減衰係数（0.0 - 1.0）
  dedup_window_sec: 60         # 重複排除の時間窓（秒）
  dedup_cache_size: 10000      # 重複排除キャッシュサイズ（LRU）
```

| オプション | 型 | デフォルト | 説明 |
|--------|------|---------|-------------|
| `ctx_buffer_size` | int | 50 | コンテキストごとに保持するイベント数。古いイベントは上書きされます。 |
| `decay_interval_sec` | int | 3600 | 共起スコアの減衰間隔（0 = 無効）。 |
| `decay_alpha` | float | 0.99 | 各減衰時のスコア乗数（0.0-1.0、高いほど減衰が遅い）。 |
| `dedup_window_sec` | int | 60 | 重複検出の時間窓（秒）。この時間窓内に同じ `(ctx, id, score)` の組み合わせを受信した場合、重複として無視されます。0 に設定すると重複排除が無効になります。 |
| `dedup_cache_size` | int | 10000 | 重複排除で追跡する最近のイベントの最大数（LRUキャッシュ）。満杯になると最も古いエントリが削除されます。 |

**重複排除の動作:**

同じ `(ctx, id, score)` の組み合わせが `dedup_window_sec` 内に受信された場合、重複として検出されます。これにより以下を防ぎます：
- リトライバグによる統計の肥大化
- ネットワーク再送信による重複エントリの作成
- クライアント側のバグによる共起データへの影響

統計の追跡：
- `total_events`: 受信したEVENTコマンドの総数（重複を含む）
- `deduped_events`: 無視された重複イベントの数
- `stored_events`: リングバッファに実際に格納されたイベント数（total_events - deduped_events）

---

### ベクトルストア設定

ベクトルストレージと検索動作を制御します。

```yaml
vectors:
  default_dimension: 768       # デフォルトベクトル次元数
  distance_metric: "cosine"    # 距離メトリック: cosine, dot, l2
```

| オプション | 型 | デフォルト | 説明 |
|--------|------|---------|-------------|
| `default_dimension` | int | 768 | デフォルトベクトル次元数（一般的: 768=BERT, 1536=OpenAI, 384=MiniLM）。 |
| `distance_metric` | string | "cosine" | 距離メトリック: `cosine`, `dot`, `l2`。 |

---

### 類似検索設定

類似検索とフュージョンアルゴリズムのパラメータを制御します。

```yaml
similarity:
  default_top_k: 100           # デフォルト結果数
  max_top_k: 1000              # 最大許容 top_k
  fusion_alpha: 0.6            # ベクトル類似度の重み（フュージョンモード）
  fusion_beta: 0.4             # 共起の重み（フュージョンモード）
```

| オプション | 型 | デフォルト | 説明 |
|--------|------|---------|-------------|
| `default_top_k` | int | 100 | 指定されていない場合のデフォルト結果数。 |
| `max_top_k` | int | 1000 | 最大許容 top_k（メモリ問題を防ぐ）。 |
| `fusion_alpha` | float | 0.6 | フュージョンモードでのベクトル類似度の重み（alpha + beta = 1.0）。 |
| `fusion_beta` | float | 0.4 | フュージョンモードでの共起の重み。 |

**注意**: `fusion_beta` を高くすると、イベントベースのシグナルがより重視されます。

---

### スナップショット永続化設定

スナップショットの保存/読み込み動作を制御します。

```yaml
snapshot:
  dir: "/var/lib/nvecd/snapshots"  # スナップショットディレクトリ
  default_filename: "nvecd.snapshot" # デフォルトファイル名
  interval_sec: 0                   # 自動スナップショット間隔（0 = 無効）
  retain: 3                         # 保持するスナップショット数
```

| オプション | 型 | デフォルト | 説明 |
|--------|------|---------|-------------|
| `dir` | string | "/var/lib/nvecd/snapshots" | スナップショットディレクトリ（存在しない場合は作成）。 |
| `default_filename` | string | "nvecd.snapshot" | 手動保存のデフォルトファイル名。 |
| `interval_sec` | int | 0 | 自動スナップショット間隔（秒、0 = 無効）。 |
| `retain` | int | 3 | 保持する自動スナップショット数（手動スナップショットは影響を受けない）。 |

**自動スナップショットのファイル名**: `auto_YYYYMMDD_HHMMSS.snapshot`

---

### パフォーマンス設定

サーバーパフォーマンスとリソース制限を制御します。

```yaml
performance:
  thread_pool_size: 8          # ワーカースレッドプールサイズ
  max_connections: 1000        # 最大同時接続数
  connection_timeout_sec: 300  # 接続タイムアウト（秒）
```

| オプション | 型 | デフォルト | 説明 |
|--------|------|---------|-------------|
| `thread_pool_size` | int | 8 | ワーカースレッドプールサイズ（推奨: CPU コア数）。 |
| `max_connections` | int | 1000 | 最大同時接続数（システム制限に基づいて設定）。 |
| `connection_timeout_sec` | int | 300 | アイドル接続タイムアウト（秒）。 |

---

### API サーバー設定

TCP および HTTP API サーバーの設定を制御します。

#### TCP API（常に有効）

```yaml
api:
  tcp:
    bind: "127.0.0.1"          # TCP バインドアドレス
    port: 11017                # TCP ポート
```

| オプション | 型 | デフォルト | 説明 |
|--------|------|---------|-------------|
| `bind` | string | "127.0.0.1" | バインドアドレス（"0.0.0.0" = 全インターフェース、**セキュリティリスク**）。 |
| `port` | int | 11017 | TCP リッスンポート。 |

#### HTTP API（オプション）

> ⚠️ **未実装** - HTTP/JSON API は将来のリリースで予定されています。

```yaml
api:
  http:
    enable: false              # HTTP/JSON API を有効化
    bind: "127.0.0.1"          # HTTP バインドアドレス
    port: 8080                 # HTTP ポート
    enable_cors: false         # CORS ヘッダーを有効化
    cors_allow_origin: ""      # 許可されたオリジン
```

#### レート制限（オプション）

> ⚠️ **未実装** - レート制限は将来のリリースで予定されています。

```yaml
api:
  rate_limiting:
    enable: false              # レート制限を有効化
    capacity: 100              # 最大バーストトークン
    refill_rate: 10            # 秒あたりのトークン
    max_clients: 10000         # 追跡する最大クライアント数
```

---

### ネットワークセキュリティ設定

IP アドレスアクセス制御（CIDR ベース）を制御します。

```yaml
network:
  allow_cidrs:
    - "127.0.0.1/32"           # ローカルホストのみ（推奨）
    # - "192.168.1.0/24"       # 例: ローカルネットワーク
    # - "0.0.0.0/0"            # 警告: すべて許可（非推奨）
```

| オプション | 型 | デフォルト | 説明 |
|--------|------|---------|-------------|
| `allow_cidrs` | list | `["127.0.0.1/32"]` | 許可された CIDR 範囲。**空 = すべて拒否（フェイルクローズ）**。 |

**セキュリティ注意**: 空の `allow_cidrs` はデフォルトで**すべての接続を拒否**します。許可する IP 範囲を明示的に設定する必要があります。

---

### ログ設定

ログ出力形式と出力先を制御します。

```yaml
logging:
  level: "info"                # ログレベル
  json: true                   # JSON 形式出力
  file: ""                     # ログファイルパス（空 = stdout）
```

| オプション | 型 | デフォルト | 説明 |
|--------|------|---------|-------------|
| `level` | string | "info" | ログレベル: `trace`, `debug`, `info`, `warn`, `error`。 |
| `json` | bool | true | JSON 構造化ログを有効化。 |
| `file` | string | "" | ログファイルパス（空 = stdout、systemd/Docker 用）。 |

---

### クエリ結果キャッシュ設定（オプション）

> ⚠️ **未実装** - クエリ結果キャッシュは将来のリリースで予定されています。

```yaml
cache:
  enabled: true                # クエリ結果キャッシュを有効化
  max_memory_mb: 32            # 最大キャッシュメモリ（MB）
  min_query_cost_ms: 10.0      # キャッシュする最小クエリコスト（ms）
  ttl_seconds: 3600            # キャッシュエントリ TTL（秒）
  compression_enabled: true    # LZ4 圧縮を有効化
  eviction_batch_size: 10      # 削除バッチサイズ
```

---

## 最小設定例

```yaml
# ローカルテスト用の最小設定
events:
  ctx_buffer_size: 50

vectors:
  default_dimension: 768

api:
  tcp:
    bind: "127.0.0.1"
    port: 11017

network:
  allow_cidrs:
    - "127.0.0.1/32"

logging:
  level: "info"
  json: true
```

---

## 本番環境設定例

```yaml
# セキュリティ強化された本番設定
events:
  ctx_buffer_size: 100
  decay_interval_sec: 7200     # 2時間
  decay_alpha: 0.95

vectors:
  default_dimension: 768

similarity:
  max_top_k: 500
  fusion_alpha: 0.7
  fusion_beta: 0.3

snapshot:
  dir: "/var/lib/nvecd/snapshots"
  interval_sec: 14400          # 4時間
  retain: 5

performance:
  thread_pool_size: 16         # 16コアサーバー
  max_connections: 5000
  connection_timeout_sec: 600

api:
  tcp:
    bind: "0.0.0.0"            # 全インターフェース（allow_cidrs でセキュリティ確保）
    port: 11017

network:
  allow_cidrs:
    - "10.0.0.0/8"             # プライベートネットワークのみ
    - "172.16.0.0/12"

logging:
  level: "warn"
  json: true
  file: "/var/log/nvecd/nvecd.log"
```

---

## 設定の検証

`CONFIG VERIFY` コマンドで設定ファイルの構文をチェックできます：

```bash
# サーバーに接続
nc localhost 11017

# 設定を検証
CONFIG VERIFY
```

または `CONFIG SHOW` で現在の設定を表示：

```bash
CONFIG SHOW
```

---

## 次のステップ

- 利用可能なコマンドについては [プロトコルリファレンス](protocol.md) を参照
- 永続化の詳細については [スナップショット管理](snapshot.md) を参照
- デプロイ手順については [インストールガイド](installation.md) を参照
