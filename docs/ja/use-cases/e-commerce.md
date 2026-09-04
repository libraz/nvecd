# EC サイト

2 つの手がかりから同時に答える商品レコメンドです。カタログが語る商品の中身と、買い物客がその商品に対して行ったことの両方を使います。このガイドでは、関与度で重みづけしたイベント、商品の埋め込みの保存、アイテムごとのデータ量に追随する配分での `using=fusion`、そして実際のカタログが結果に必要とするメタデータフィルタを扱います。

共起の仕組みは[レコメンドのガイド](./recommendations.md)を前提にします。

## 関与はひとつの手がかりではない

閲覧もカート追加も購入もどれも操作ですが、1 つのイベント型にまとめてしまうとその差が消えます。`EVENT` は `[0, 100]` の整数スコアを取り、組の共起への寄与は 2 つのスコアの積なので、段の差は均されるどころか増幅されます。

```bash
nvecd-cli -p 11017 EVENT alice ADD lamp 60    # 商品ページの閲覧
nvecd-cli -p 11017 EVENT alice ADD lamp 85    # カートに追加
nvecd-cli -p 11017 EVENT alice ADD bulb 100   # 購入
```

この 3 件で alice のコンテキストには 3 つのイベントが残り、`lamp`–`bulb` の辺は閲覧から `60 × 100` を、カート追加から `85 × 100` を受け取ります。購入どうしの組なら `100 × 100`、閲覧どうしなら `60 × 60` です。この段は設定のどこにも書かれていません。スコアそのものが段です。

この種の流れてくるイベントには `ADD` を使います。ほしい物リストのように買い物客が切り替える状態には、最後の値に対して冪等で再送しても安全な `SET` を使います。

## 商品ベクトルの保存

nvecd は埋め込みを作りません。カタログがすでに使っているモデル、たとえば商品写真に対する画像エンコーダ、タイトルと説明文に対するテキストエンコーダ、あるいはその連結で各商品をエンコードし、その結果を送ります。

```bash
nvecd-cli -p 11017 VECSET lamp 0.82 0.31 0.11 0.05
```

ストアは最初に受理したベクトルで次元を確定し、それ以降の `VECSET` はその幅に一致していなければなりません。`vectors.default_dimension` は幅を強制するのではなく ANN 索引を事前に確保するための値なので、最初のベクトルが 768 次元ならキーの値にかかわらず 768 次元の検索を扱います。このガイドの例は 1 行に収まるように 4 次元にしてあります。

`vectors.distance_metric` は近さの測り方を選びます。`cosine`（既定値）、`dot`、`l2` があります。エンコーダが学習した前提に合わせてください。

HTTP なら同じ書き込みにメタデータを載せられるので、商品ごとの往復が 1 回で済みます。HTTP サーバーは既定で無効なので、`api.http.enable: true` にして有効化します。

```bash
curl -s -X POST http://127.0.0.1:8080/vecset \
  -H 'Content-Type: application/json' \
  -d '{"id":"lamp","vector":[0.82,0.31,0.11,0.05],"metadata":{"category":"lighting","in_stock":true,"price":39.9}}'
```

```json
{"dimension":4,"status":"ok"}
```

TCP では `METASET` が同じ役割で、カンマ区切りの組を取り、先にベクトルが存在している必要があります。

```bash
nvecd-cli -p 11017 METASET lamp category:lighting,in_stock:true
```

## 統合検索

`using=fusion` は 2 つの検索を走らせて統合します。それぞれの結果セットの中で最小最大正規化により `[0, 1]` に収めてから、`alpha × ベクトルのスコア + beta × イベントのスコア` として混ぜます。2 つの重みは `similarity.fusion_alpha` と `similarity.fusion_beta` です。

和集合をとる点が重要です。ベクトル検索はイベントの隣接ではなくストア全体を対象に走るので、一度も一緒に買われていない内容の近い商品も浮上できますし、ベクトルを持たないが一緒に買われた商品も同じように浮上できます。

## 適応的な統合

固定の配分は成熟したカタログには合いますが、新規出品には合いません。昨日登録された商品には購買履歴がないので、イベント側は何も返さず、固定の `beta` はスコアの予算を空の手がかりに割り当てることになります。

`similarity.adaptive_fusion` を有効にすると、配分は検索対象のアイテムが実際に持つイベントの量に追随します。重みはアイテムの共起隣接数から補間されます。

```text
ratio = min(1, 隣接数 / adaptive_maturity_threshold)
alpha = adaptive_max_alpha - ratio × (adaptive_max_alpha - adaptive_min_alpha)
beta  = 1 - alpha
```

新しいアイテムは `adaptive_max_alpha` の近くに位置して主にベクトルで順位づけられ、十分に観測されたアイテムは `adaptive_min_alpha` に近づいて主に行動で順位づけられます。隣接数は切り詰められた結果リストではなく共起索引から直接読むので、`top_k` が小さいせいで成熟したアイテムが新しく見えることはありません。

検索ごとに `adaptive=on` と `adaptive=off` で設定値を上書きできます。

## ひととおり動かす

```yaml
events:
  ctx_buffer_size: 100
  decay_interval_sec: 86400
  decay_alpha: 0.98

vectors:
  default_dimension: 768
  distance_metric: "cosine"

similarity:
  fusion_alpha: 0.6
  fusion_beta: 0.4
  adaptive_fusion: true
  adaptive_min_alpha: 0.2
  adaptive_max_alpha: 0.9
  adaptive_maturity_threshold: 50
```

4 商品のカタログを投入します。`shade` が新規出品で、ベクトルとメタデータはありますが、まだ誰も触っていません。

```bash
nvecd-cli -p 11017 VECSET lamp  0.82 0.31 0.11 0.05
nvecd-cli -p 11017 VECSET bulb  0.79 0.35 0.14 0.04
nvecd-cli -p 11017 VECSET shade 0.71 0.44 0.20 0.09
nvecd-cli -p 11017 VECSET desk  0.12 0.20 0.88 0.40

nvecd-cli -p 11017 METASET lamp  category:lighting,in_stock:true
nvecd-cli -p 11017 METASET bulb  category:lighting,in_stock:true
nvecd-cli -p 11017 METASET shade category:lighting,in_stock:false
nvecd-cli -p 11017 METASET desk  category:furniture,in_stock:true

nvecd-cli -p 11017 EVENT alice ADD lamp 60
nvecd-cli -p 11017 EVENT alice ADD lamp 85
nvecd-cli -p 11017 EVENT alice ADD bulb 100
nvecd-cli -p 11017 EVENT bob   ADD lamp 100
nvecd-cli -p 11017 EVENT bob   ADD desk 60
```

まず手がかりごとに見ます。行動から見ると次のとおりです。

```bash
nvecd-cli -p 11017 SIM lamp 5 using=events
```

```text
(2 results, showing 2)
1) bulb (score: 14500)
2) desk (score: 6000)
```

内容から見ると次のとおりです。

```bash
nvecd-cli -p 11017 SIM lamp 5 using=vectors
```

```text
(3 results, showing 3)
1) bulb (score: 0.9978)
2) shade (score: 0.975)
3) desk (score: 0.315)
```

行動は `shade` を知りませんが、内容は 2 位に置きます。適応的な統合を有効にした統合検索では次のようになります。

```bash
nvecd-cli -p 11017 SIM lamp 5 using=fusion
```

```text
(3 results, showing 3)
1) bulb (score: 1)
2) shade (score: 0.8429)
3) desk (score: 0)
```

`lamp` の共起隣接は 2 件で、成熟のしきい値は 50 なので、比は `0.04` になり `alpha` は `0.872` に落ち着きます。同じ検索を固定の重みで走らせると、その効果がわかります。

```bash
nvecd-cli -p 11017 SIM lamp 5 using=fusion adaptive=off
```

```text
(3 results, showing 3)
1) bulb (score: 1)
2) shade (score: 0.58)
3) desk (score: 0)
```

`shade` は固定の `0.6 / 0.4` では `0.58`、適応的な配分では `0.8429` です。適応的な重みが `lamp` のイベント側の薄さを見て取り、代わりにベクトルへ寄りかかったからです。

新しい商品そのものを起点にする検索も、ベクトルを持った時点から動きます。

```bash
nvecd-cli -p 11017 SIM shade 5 using=fusion
```

```text
(3 results, showing 3)
1) bulb (score: 1)
2) lamp (score: 0.9786)
3) desk (score: 0)
```

`shade` にはイベントが 1 件もありません。片側にしか候補がないときは寄与する側の重みが 1 に再正規化されるので、残ったほうの手がかりが欠けたほうの重みの分だけ縮むことはありません。

## カタログの絞り込み

在庫切れの商品を挙げたり、カテゴリの境界を越えたりするレコメンドは、店舗側から見れば不具合です。`filter=` はアイテムのメタデータで結果を縛ります。

```bash
nvecd-cli -p 11017 SIM lamp 5 using=fusion filter=category=lighting,in_stock=true
```

```text
(1 results, showing 1)
1) bulb (score: 0.9981)
```

条件はカンマ区切りで、AND で結合します。使える演算子は `=`、`:`（`=` の別名）、`!=`、`>`、`<`、`>=`、`<=`、`in(a|b|c)` なので、`price<50` も `category=in(lighting|decor)` も書けます。値は受け取る時点で型が決まります。`true` と `false` は真偽値になり、整数や浮動小数として読めるものは数値に、それ以外は文字列になります。

メタデータを持たないアイテムは、空でないフィルタには決して一致しません。`METASET` がベクトルを要求する以上、絞り込みの対象になるのはベクトルを持つアイテムだけです。

フィルタを付けたことで `bulb` のスコアは `1` から `0.9981` に変わりました。正規化は絞り込みを通過した候補の上で計算されるため、スコアは 1 つの結果セットの中での相対値であって、検索をまたいで比較できません。結果は順位として扱い、別の検索で見た数値をしきい値にしないでください。

## 制約

nvecd は 1 プロセスのメモリに 1 つのデータセットを持ちます。レプリケーションもシャーディングもノードをまたぐ検索もなく、別のインスタンスへデータを複製する機能もサーバー側にはありません。読み取り専用の複製は運用側でスクリプトを書けば作れます。複製元で `DUMP SAVE` を実行し、できたファイルをコピーし、複製先で `DUMP LOAD` を実行する手順です。これは手動かつある時点のもので、継続的な同期は一切ありません。保存後に複製元へ届いた書き込みは複製に入りません。

検索のコストはカタログの規模と設定で決まり、このページの内容では決まりません。実際に計測された値は[ベンチマーク](../benchmarks.md)にあります。

重みづけの規則、正規化、片側だけになったときの動きは[統合検索](../fusion.md)で詳しく説明しています。
