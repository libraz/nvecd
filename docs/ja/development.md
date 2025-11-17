# Nvecd を使い始める

このガイドは、アプリケーションで Nvecd を使い始めるのに役立ちます。

## クイックスタート

### 1. サーバーを起動

```bash
# 設定ファイルを指定して nvecd を起動
nvecd -c config.yaml
```

### 2. サーバーに接続

```bash
# telnet を使用
telnet localhost 11017

# または nc を使用
nc localhost 11017
```

### 3. ベクトルを登録

```bash
# アイテム "item1" のベクトルを登録
VECSET item1 3 0.1 0.5 0.8
# レスポンス: OK

# さらにベクトルを登録
VECSET item2 3 0.2 0.4 0.9
VECSET item3 3 0.1 0.6 0.7
```

### 4. イベントを記録

```bash
# イベント記録: ユーザー "user123" が "item1" とスコア 95 で対話
EVENT user123 item1 95
# レスポンス: OK

EVENT user123 item2 80
EVENT user456 item1 90
EVENT user456 item3 85
```

### 5. 類似アイテムを検索

```bash
# "item1" に類似するアイテムをフュージョンモードで検索
SIM item1 10 fusion
# レスポンス:
# OK RESULTS 2
# item2 0.8523
# item3 0.7891
```

---

## 一般的な利用例

### 利用例 1: コンテンツ推薦

コンテンツ類似度とユーザー行動を組み合わせた推薦システムを構築します。

```bash
# 1. コンテンツベクトルを登録（埋め込みから）
VECSET article123 768 0.123 0.456 ... (768次元)
VECSET article456 768 0.789 0.234 ...

# 2. ユーザーエンゲージメントを追跡
EVENT user_alice article123 95  # Alice が article123 に高いエンゲージメント
EVENT user_alice article456 80
EVENT user_bob article123 90

# 3. 類似記事を検索（フュージョンモードはベクトル + エンゲージメントを組み合わせ）
SIM article123 10 fusion
# コンテンツが類似し、かつ似たユーザーに人気の記事を返す
```

### 利用例 2: セマンティック検索

純粋なベクトルベースの類似検索です。

```bash
# ドキュメントベクトルを登録
VECSET doc1 384 0.1 0.2 ...
VECSET doc2 384 0.3 0.4 ...

# クエリベクトルで検索（ユーザーの検索から）
SIMV 384 0.15 0.25 ... 10 cosine
# クエリと意味的に類似したドキュメントを返す
```

### 利用例 3: 協調フィルタリング

ユーザー行動パターンに基づいて推薦します。

```bash
# ユーザーインタラクションを追跡
EVENT user1 movie123 95
EVENT user1 movie456 80
EVENT user2 movie123 90
EVENT user2 movie789 85

# 映画ベクトルを登録（メタデータ埋め込み）
VECSET movie123 128 ...
VECSET movie456 128 ...
VECSET movie789 128 ...

# movie123 に類似する映画を検索（フュージョンモード）
SIM movie123 10 fusion
# 似た嗜好のユーザーが好んだ映画を推薦
```

---

## 類似度モード

### 内積 (`dot`)

ベクトル間の生の内積です。ベクトルがすでに正規化されている場合や、大きさが重要な場合に使用します。

```bash
SIM item1 10 dot
SIMV 768 0.1 0.2 ... 10 dot
```

### コサイン類似度 (`cosine`)

正規化された類似度（範囲: -1.0 ～ 1.0）。ベクトルの大きさに関係なく、一般的な意味的類似性に使用します。

```bash
SIM item1 10 cosine
SIMV 768 0.1 0.2 ... 10 cosine
```

### フュージョン検索 (`fusion`) - 推奨

ベクトル類似度 + イベントからの共起を組み合わせます。ハイブリッド推薦システムに使用します。

```bash
SIM item1 10 fusion
# 注意: SIM でのみ利用可能、SIMV では不可
```

**フュージョン式**: `score = alpha * vector_sim + beta * co_occurrence`

`config.yaml` で重みを設定：
```yaml
similarity:
  fusion_alpha: 0.6  # ベクトル類似度の重み
  fusion_beta: 0.4   # 共起の重み
```

---

## ベストプラクティス

### 1. ベクトル次元数

ユースケースに応じて適切な次元数を選択：
- **384**: MiniLM 埋め込み（軽量）
- **768**: BERT 埋め込み（バランス型）
- **1536**: OpenAI 埋め込み（高品質）

デフォルト次元数を設定で指定：
```yaml
vectors:
  default_dimension: 768
```

### 2. イベントスコアリング

一貫したスコアリング（0-100）を使用：
- **90-100**: 高エンゲージメント（購入、長い視聴時間、高評価）
- **70-89**: 中エンゲージメント（クリック、短い視聴、中評価）
- **50-69**: 低エンゲージメント（インプレッション、即離脱）
- **50 未満**: ネガティブシグナル（該当する場合）

### 3. コンテキスト設計

ユースケースに基づいてコンテキストを設計：
- **ユーザーベース**: `user_{id}`（ユーザーごとの行動を追跡）
- **セッションベース**: `session_{id}`（セッションごとの行動を追跡）
- **時間ベース**: `hourly_{timestamp}`（時間窓の付いたパターンを追跡）

### 4. 減衰設定

進化する好みに対応するために減衰を設定：

```yaml
events:
  decay_interval_sec: 7200  # 2時間ごとに減衰
  decay_alpha: 0.95         # インターバルごとに5%減衰
```

- **短い減衰**: 最近のトレンドをキャプチャ（ニュース、トレンドトピック）
- **長い減衰**: 安定した好み（商品推薦）
- **減衰なし**: 履歴分析（`decay_interval_sec: 0`）

---

## 統合例

### Python 統合

> ⚠️ **未実装** - Python クライアントライブラリは将来のリリースで予定されています。

将来の使用方法：
```python
from nvecdclient import NvecdClient

client = NvecdClient(host='localhost', port=11017)
client.connect()

# ベクトルを登録
client.vecset('item1', [0.1, 0.5, 0.8])

# イベントを記録
client.event('user123', 'item1', 95)

# 検索
results = client.sim('item1', top_k=10, mode='fusion')
for item_id, score in results:
    print(f"{item_id}: {score}")
```

### Node.js 統合

> ⚠️ **未実装** - Node.js クライアントライブラリは将来のリリースで予定されています。

将来の使用方法：
```javascript
const NvecdClient = require('nvecdclient');

const client = new NvecdClient({ host: 'localhost', port: 11017 });
await client.connect();

// ベクトルを登録
await client.vecset('item1', [0.1, 0.5, 0.8]);

// イベントを記録
await client.event('user123', 'item1', 95);

// 検索
const results = await client.sim('item1', { topK: 10, mode: 'fusion' });
results.forEach(({ id, score }) => console.log(`${id}: ${score}`));
```

### 生の TCP 統合

現在の方法（TCP ソケットを持つ任意の言語）：

```python
import socket

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.connect(('localhost', 11017))

# コマンドを送信
sock.sendall(b'VECSET item1 3 0.1 0.5 0.8\n')
response = sock.recv(1024).decode().strip()
print(response)  # "OK"

# イベントを記録
sock.sendall(b'EVENT user123 item1 95\n')
response = sock.recv(1024).decode().strip()
print(response)  # "OK"

# 検索
sock.sendall(b'SIM item1 10 fusion\n')
response = sock.recv(4096).decode().strip()
print(response)  # "OK RESULTS 2\nitem2 0.8523\nitem3 0.7891"

sock.close()
```

---

## モニタリングとデバッグ

### サーバーステータス

サーバー統計を確認：
```bash
INFO
```

レスポンスには以下が含まれます：
- 稼働時間
- 処理されたコマンド総数
- メモリ使用量
- ストアサイズ（コンテキスト、ベクトル、共起ペア）

### デバッグモード

接続のデバッグログを有効化：
```bash
DEBUG ON
# すべてのコマンドで追加のデバッグ情報が表示されます

DEBUG OFF
# デバッグログを無効化
```

### 設定の検証

設定が有効かどうかを検証：
```bash
CONFIG VERIFY
```

現在の設定を表示：
```bash
CONFIG SHOW
```

---

## 本番デプロイメント

### 1. セキュリティ

`config.yaml` でネットワークアクセスを設定：
```yaml
network:
  allow_cidrs:
    - "10.0.0.0/8"      # プライベートネットワークを許可
    - "172.16.0.0/12"   # Docker ネットワークを許可
```

### 2. 永続化

自動スナップショットを有効化：
```yaml
snapshot:
  dir: "/var/lib/nvecd/snapshots"
  interval_sec: 7200  # 2時間ごとにスナップショット
  retain: 5           # 5個のスナップショットを保持
```

メンテナンス前の手動スナップショット：
```bash
DUMP SAVE /backup/before_maintenance.nvec
```

### 3. パフォーマンスチューニング

スレッドプールを設定：
```yaml
performance:
  thread_pool_size: 16        # CPU コア数に合わせる
  max_connections: 5000       # システム制限に基づく
  connection_timeout_sec: 600
```

---

## 次のステップ

- すべての利用可能なコマンドについては [プロトコルリファレンス](protocol.md) を参照
- チューニングオプションについては [設定ガイド](configuration.md) を参照
- 最適化のヒントについては [パフォーマンスガイド](performance.md) を参照
- REST API 使用法については [HTTP API リファレンス](http-api.md) を参照（利用可能時）
