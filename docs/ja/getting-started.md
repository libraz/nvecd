# はじめての操作

空のサーバーからスナップショットの保存まで、1 つのセッションで通します。少数のイベント、3 つの検索モード、クエリベクトル、メタデータによる絞り込み、そしてサーバーの統計です。以下のコマンドはすべて同じサーバーに対して、この順番で実行します。各コマンドの網羅的な構文は [TCP プロトコル](./protocol.md) にあります。このページでは答えの意味を説明します。

## ビルド

```bash
git clone https://github.com/libraz/nvecd.git
cd nvecd
make
```

バイナリは `build/bin/` に生成されます。サーバーの `nvecd` とクライアントの `nvecd-cli` です。システムパッケージ、ビルドオプション、バイナリのインストールは [インストール](./installation.md) を参照してください。

## サーバーの起動

`examples/config.yaml` はすべての項目が既定値です。最初に動かす前に変えておきたいのはスナップショットの保存先で、既定値の `/var/lib/nvecd/snapshots` は一般ユーザーでは書き込めません。

```bash
mkdir -p /tmp/nvecd-data
sed 's|/var/lib/nvecd/snapshots|/tmp/nvecd-data|' examples/config.yaml > /tmp/nvecd.yaml
./build/bin/nvecd -c /tmp/nvecd.yaml
```

ログの最後は `nvecd server started on 127.0.0.1:11017` になります。このまま起動しておき、以下は別の端末で実行してください。設定項目の説明は [設定](./configuration.md) にあります。

## イベントを記録する

イベントは、あるコンテキストの中でアイテムが操作されたことを表します。コンテキストはまとめ方を決めるキーで、ここでは 3 つのセッションを使います。

```bash
./build/bin/nvecd-cli -p 11017 EVENT alice ADD widget 100
./build/bin/nvecd-cli -p 11017 EVENT alice ADD gasket 80
./build/bin/nvecd-cli -p 11017 EVENT bob   ADD widget 100
./build/bin/nvecd-cli -p 11017 EVENT bob   ADD flange 95
./build/bin/nvecd-cli -p 11017 EVENT carol ADD gasket 70
./build/bin/nvecd-cli -p 11017 EVENT carol ADD flange 60
```

いずれも `EVENT` と応答します。スコアは 0 から 100 の整数の重みで、評価点ではなく、その操作の相対的な強さを表します。

## 共起で検索する

```bash
./build/bin/nvecd-cli -p 11017 SIM widget 10 using=events
```

```text
(2 results, showing 2)
1) flange (score: 9500)
2) gasket (score: 8000)
```

`widget` は `flange` と 1 回（`bob`、重み 100 と 95）、`gasket` と 1 回（`alice`、重み 100 と 80）同じコンテキストに現れました。コンテキストを共有するたびに 2 つのイベント重みの積が加算され、合計が 9500 と 8000 になります。共起スコアは上限のない累積和で、[0, 1] の類似度ではありません。異なるクエリアイテム同士でスコアを比べても意味はありません。スコアの計算規則、減衰、枝刈りは [イベントと共起](./events-and-co-occurrence.md) で扱います。

一度もイベントに現れていないアイテムは、索引に項目自体がありません。

```bash
./build/bin/nvecd-cli -p 11017 SIM bolt 10 using=events
```

```text
(0 results)
```

`nvecd-cli` が表示しているのは応答そのものではなく、整形した結果です。サーバーは行区切りのテキストプロトコルを話すので、同じクエリはワイヤ上では次のようになります。

```text
OK RESULTS 2\r\n
flange 9500.0000\r\n
gasket 8000.0000\r\n
```

ワイヤ上のスコアは常に小数点以下 4 桁です。CLI はそれをシェルの既定の浮動小数点精度で表示し直すので、上のブロックでは `9500`、ワイヤ上では `9500.0000` になります。

## ベクトルを登録する

ベクトルは外から与えます。nvecd は保存と比較を行いますが、生成はしません。4 アイテム、各 4 次元です。`bolt` はイベントを 1 件も持たず、`widget`・`gasket`・`flange` はここで両方の信号を持つことになります。

```bash
./build/bin/nvecd-cli -p 11017 VECSET widget 0.10 0.20 0.30 0.40
./build/bin/nvecd-cli -p 11017 VECSET gasket 0.15 0.18 0.32 0.41
./build/bin/nvecd-cli -p 11017 VECSET flange 0.90 0.10 0.05 0.02
./build/bin/nvecd-cli -p 11017 VECSET bolt   0.11 0.21 0.29 0.39
```

いずれも `VECSET` と応答します。最初に保存されたベクトルがストア全体の次元を決め、以降のベクトルは同じ長さでなければなりません。この例では読みやすさのために 4 次元にしていますが、同梱の設定の `vectors.default_dimension` は 768 です。

## ベクトルで検索する

```bash
./build/bin/nvecd-cli -p 11017 SIM widget 10 using=vectors
```

```text
(3 results, showing 3)
1) bolt (score: 0.9994)
2) gasket (score: 0.9954)
3) flange (score: 0.2677)
```

尺度は `vectors.distance_metric` で、既定は cosine です。したがってスコアはコサイン類似度で、クエリアイテム自身は結果から除かれます。`bolt` は誰も操作していないのに 1 位です。埋め込みが `widget` に近く、このモードが見ているのはそれだけだからです。尺度と ANN 索引の種類は [ベクトル検索](./vector-search.md) で扱います。

## 2 つを統合する

```bash
./build/bin/nvecd-cli -p 11017 SIM widget 10 using=fusion
```

```text
(3 results, showing 3)
1) bolt (score: 0.6)
2) gasket (score: 0.5967)
3) flange (score: 0.4)
```

変わった点は 3 つあります。候補集合は両方の検索の和集合なので、ベクトルだけの `bolt` と、イベントに強くベクトルに弱い `flange` の双方が残ります。統合の前に、各ソースのスコアはそのソース自身の候補一覧の中で min-max 正規化されます。コサイン類似度と共起の累積和という比較できない 2 つの尺度を、同じ範囲に揃えるためです。正規化したスコアは設定した配分で足し合わされ、ベクトル側が `similarity.fusion_alpha` の 0.6、イベント側が `similarity.fusion_beta` の 0.4 です。

この正規化は相対的なので、共起の候補が 2 件しかない今回は癖が出ます。`gasket` はイベント側では 2 件のうち弱いほうなので min-max で 0 に落ち、0.5967 はすべてベクトル側から来ています。候補一覧が増えれば影響は薄れます。

イベントをまったく持たないアイテムでは、もう一方の挙動が見えます。

```bash
./build/bin/nvecd-cli -p 11017 SIM bolt 10 using=fusion
```

```text
(3 results, showing 3)
1) widget (score: 1)
2) gasket (score: 0.9942)
3) flange (score: 0)
```

`bolt` には共起の隣接がないため、イベント側は候補を出さず、その配分は外され、ベクトル側の重みが 1 に再正規化されます。欠けた信号のぶんスコアが目減りすることはありません。

`adaptive=on` を付けると、配分は固定の 0.6 / 0.4 ではなくクエリアイテム自身のデータ密度に従います。

```bash
./build/bin/nvecd-cli -p 11017 SIM widget 10 using=fusion adaptive=on
```

```text
(3 results, showing 3)
1) bolt (score: 0.872)
2) gasket (score: 0.8672)
3) flange (score: 0.128)
```

`widget` の共起の隣接は 2 件で、`adaptive_maturity_threshold` の 50 に対して新しいアイテムとみなされるため、ベクトル側の重みが 0.872 まで上がります。隣接数が閾値へ近づくにつれて、この重みは `adaptive_min_alpha` へ下がっていきます。配分と正規化の詳細は [統合検索](./fusion.md) で扱います。

## クエリベクトルで検索する

`SIMV` はアイテム ID ではなくベクトルを受け取るので、クエリが保存済みの何かに対応している必要はありません。

```bash
./build/bin/nvecd-cli -p 11017 SIMV 3 0.50 0.30 0.20 0.10
```

```text
(3 results, showing 3)
1) flange (score: 0.8685)
2) gasket (score: 0.6569)
3) bolt (score: 0.6367)
```

クエリベクトルはストアの次元と一致していなければなりません。候補からは何も除外されません。クエリアイテムがない以上、除くべき自分自身も存在しないからです。したがってここでは `widget` も結果になり得ます。

## 結果を絞り込む

メタデータはアイテムごとに付与し、既にベクトルを持つアイテムにしか設定できません。

```bash
./build/bin/nvecd-cli -p 11017 METASET widget category:tools
./build/bin/nvecd-cli -p 11017 METASET gasket category:seals
./build/bin/nvecd-cli -p 11017 METASET flange category:tools
./build/bin/nvecd-cli -p 11017 METASET bolt   category:tools
```

`filter=` はメタデータが一致しない候補を落とします。使える演算子は `=`、`:`、`!=`、`>`、`<`、`>=`、`<=`、`in(a|b|c)` で、カンマで区切った複数の条件は AND で結ばれます。

```bash
./build/bin/nvecd-cli -p 11017 SIM widget 10 using=vectors filter=category:tools
```

```text
(2 results, showing 2)
1) bolt (score: 0.9994)
2) flange (score: 0.2677)
```

`min_score=` のほうは、スコアを付けたあとで下位を切り落とします。

```bash
./build/bin/nvecd-cli -p 11017 SIM widget 10 using=vectors min_score=0.5
```

```text
(2 results, showing 2)
1) bolt (score: 0.9994)
2) gasket (score: 0.9954)
```

閾値が指す尺度はモードごとに違います。`using=vectors` ではコサイン類似度、`using=fusion` では正規化した統合値、`using=events` では生の共起の累積和です。

## サーバーの状態を見る

```bash
./build/bin/nvecd-cli -p 11017 INFO
```

```text
INFO

# Server
version: 0.1.0
uptime_seconds: 15

# Stats
total_commands_processed: 24
failed_commands: 0
total_connections_received: 24
active_connections: 1
event_commands: 6
sim_commands: 9
vecset_commands: 4
wal_replay_records_skipped: 0

# Memory
used_memory_bytes: 64
used_memory_human: 0.00 MB
memory_health: HEALTHY

# Cache
cache_entries: 0
cache_hits: 0
cache_misses: 8
cache_hit_rate: 0.0000

# Data
id_count: 3
ctx_count: 3
vector_count: 4
event_count: 6
```

`id_count` が 4 ではなく 3 なのは、これが共起索引の知っているアイテムを数えていて、`bolt` はベクトルしか持たないからです。`used_memory_bytes` はベクトル行列だけを対象にした値で、4 次元のベクトル 4 本ぶんです。

キャッシュには何も入っていません。ここまでのクエリはいずれも `cache.min_query_cost_ms` の 10 ミリ秒をはるかに下回っており、それより安いクエリは保存する価値がないと判断されます。ミス 8 件は `SIM` の照会 8 件で、絞り込みのない `SIMV` は既定の方針ではキャッシュ対象になりません。方針と無効化の規則は [キャッシュ](./caching.md) で扱います。

## 状態を保存する

ここまでの状態はすべてメモリ上にあります。`DUMP SAVE` は、イベント・共起・ベクトル・メタデータのスナップショットを `snapshot.dir` に書き出します。起動時の復旧が候補にするのは名前が `.nvec` か `.dmp` で終わるファイルだけなので、その形式の名前を付けてください。同梱の `snapshot.default_filename` は `nvecd.snapshot` で、正しく書き出されはしますが次回の起動では読み込まれません。

```bash
./build/bin/nvecd-cli -p 11017 DUMP SAVE nvecd.dmp
```

```text
DUMP_SAVE_STARTED /tmp/nvecd-data/nvecd.dmp
```

スナップショットの既定モードは `fork` なので、応答は保存の完了ではなくバックグラウンド保存の開始を報告します。パスは `snapshot.dir` を絶対パスに解決したもので、macOS では `/tmp` が `/private/tmp` に解決されます。結果は `DUMP STATUS` で確認します。

```bash
./build/bin/nvecd-cli -p 11017 DUMP STATUS
```

```text
DUMP_STATUS
status: completed
filepath: /tmp/nvecd-data/nvecd.dmp
start_time: 1788427835
end_time: 1788427836
```

同じ設定でサーバーを起動し直すと、これが読み込まれます。ログに `Recovery loaded snapshot: /tmp/nvecd-data/nvecd.dmp` が出て、`SIM widget 10 using=events` は再起動前と同じ答えを返します。自動スナップショット（`snapshot.interval_sec`）、先行書き込みログ、再起動時に何が再生されるかは [永続化](./persistence.md) で扱います。

同じコマンドは `api.http.enable` を有効にすれば HTTP でも使えます（[HTTP API](./http-api.md)）。C と C++ からはクライアントライブラリを使います（[クライアントライブラリ](./client-library.md)）。
