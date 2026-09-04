# 意味検索

文書コーパスに対する検索で、クエリはストアにあるアイテムではなく利用者が入力したテキストです。このガイドでは `SIMV`、ANN 索引の選択とそれが再現率に及ぼす影響、関連度の下限としての `min_score`、そして検索範囲を絞るメタデータフィルタを扱います。

イベントはここでは登場しません。このページの内容に `EVENT` は不要で、共起索引は空のままです。

## `SIM` ではなく `SIMV` を使う理由

`SIM` はすでに保存されているアイテムを起点にします。そのアイテムを引き、ベクトルか共起の隣接を取り出して、そこから検索します。検索ボックスにはその起点がありません。利用者が入力したのは文であり、エンコーダがそれをベクトルに変え、そのベクトルはリクエストの間しか存在しません。

`SIMV` はクエリベクトルを直接受け取ります。

```text
SIMV <top_k> [filter=<式>] [min_score=<浮動小数>] <f1> <f2> ... <fN>
```

オプションはベクトルの成分より前に置かなければなりません。オプションの解釈は `=` を含まない最初のトークンで打ち切られ、そこから先はすべて浮動小数として読まれます。

```bash
nvecd-cli -p 11017 SIMV 3 0.90 0.25 0.12 0.05 filter=lang=en
```

```text
(error) Failed to parse float: filter=lang=en
```

クエリベクトルはアイテムではないので、同一性を理由に結果から外れるものはありません。ベクトルがクエリと一致する保存済み文書も、ほかの候補と同じように、順位の先頭に返ります。

## コーパスの投入

nvecd は埋め込みを作りません。アプリケーションがすでに使っているモデルで各文書をエンコードし、その結果を安定した識別子のもとに保存します。

```bash
nvecd-cli -p 11017 VECSET doc_sorting  0.91 0.22 0.10 0.04
nvecd-cli -p 11017 VECSET doc_indexing 0.86 0.31 0.15 0.07
nvecd-cli -p 11017 VECSET doc_billing  0.09 0.14 0.90 0.35
```

ストアは最初に受理したベクトルで次元を確定し、それ以降の `VECSET` はその幅に一致していなければなりません。クエリベクトルも同じです。幅が違えば埋められるのではなく拒否されます。

```text
(error) Query vector dimension mismatch: expected 4, got 3
```

`vectors.default_dimension` は幅を強制するのではなく ANN 索引を事前に確保するための値です。`vectors.distance_metric` は `cosine`（既定値）、`dot`、`l2` から選び、エンコーダが学習した前提に一致させます。コサイン類似度で学習したモデルを素の内積で順位づけると、長さの異なる文書に対して体系的に誤った答えが返ります。

検索を絞るために必要なものを付けます。

```bash
nvecd-cli -p 11017 METASET doc_sorting  lang:en,year:2024
nvecd-cli -p 11017 METASET doc_indexing lang:en,year:2021
nvecd-cli -p 11017 METASET doc_billing  lang:ja,year:2024
```

`METASET` は先にベクトルが存在していることを求めるので、文書を投入してから注釈を付けます。

## 検索する

```bash
nvecd-cli -p 11017 SIMV 3 0.90 0.25 0.12 0.05
```

```text
(3 results, showing 3)
1) doc_sorting (score: 0.9992)
2) doc_indexing (score: 0.9964)
3) doc_billing (score: 0.2613)
```

ワイヤ上の形式は `SIM` と同じで、件数の行に続いて結果 1 件につき `<id> <スコア>` の 1 行が並び、スコアは小数点以下 4 桁です。

```text
OK RESULTS 3
doc_sorting 0.9992
doc_indexing 0.9964
doc_billing 0.2613
```

## 関連度の下限としての `min_score`

`SIMV` のスコアは距離尺度の素の値です。正規化は入らないので、`cosine` なら `[-1, 1]` に収まり、そのコーパスが扱うどの検索でも同じ意味を持ちます。ここで絶対的なしきい値が使えるのはそのためで、統合検索の結果に同じしきい値を当てても意味を成しません。統合検索は結果セットごとに独自の範囲へ収め直すからです。

`top_k` だけでは、どれほど無関係でも常に `top_k` 件が返ります。検索ボックスが欲しいのは、何にも当たらないクエリに対する空の結果で、それを与えるのが `min_score` です。

```bash
nvecd-cli -p 11017 SIMV 3 min_score=0.8 0.90 0.25 0.12 0.05
```

```text
(2 results, showing 2)
1) doc_sorting (score: 0.9992)
2) doc_indexing (score: 0.9964)
```

比較は以上判定で、下限は取得のあとに適用されます。見つかった `top_k` 件から取り除くだけで、追加で取りに行くことはありません。値は実際のクエリを実際のコーパスに当てて決めてください。妥当なしきい値はエンコーダの性質であって、nvecd の性質ではありません。

## フィルタで範囲を絞る

```bash
nvecd-cli -p 11017 SIMV 3 filter=lang=en,year>=2022 0.90 0.25 0.12 0.05
```

```text
(1 results, showing 1)
1) doc_sorting (score: 0.9992)
```

条件はカンマ区切りで AND で結合し、演算子は `=`、`:`、`!=`、`>`、`<`、`>=`、`<=`、`in(a|b|c)` を使えます。値は受け取る時点で型が決まるので、`year>=2022` は数値として、`lang=en` は文字列として比較されます。メタデータを持たない文書は、空でないフィルタには決して一致しません。

絞り込みは距離尺度が候補を順位づけたあとに行われるため、選択性の高いフィルタは絞らない検索より 1 回あたりのコストが上がります。ANN 索引を使う場合、検索は `top_k` 件が残るか索引を汲み尽くすまで取得件数を広げて再試行します。

## 索引の選択と再現率

`similarity.index_type` は、検索が候補をどう見つけるかを選びます。

**`flat`** は既定値で、コーパスを走査するので厳密です。ただし規模が大きくなると効いてくる注意点が 1 つあります。`similarity.sample_size` の既定値は `10000` で、コーパスがその倍を超えると、走査は全件ではなくその件数の無作為標本を対象にします。厳密なはずの経路の中で再現率が落ちるということです。`flat` を本当に全件走査にするには `similarity.sample_size: 0` とし、そのぶん走査がコーパスに比例して伸びることを受け入れます。

**`hnsw`** はたどれるグラフを構築し、そこから答えます。検索時のつまみは `similarity.hnsw_ef_search` で、探索幅を広げるほどグラフを多く辿って真の近傍を多く拾い直しますが、1 回あたりの仕事量もそれに比例します。`similarity.hnsw_m` と `similarity.hnsw_ef_construction` はグラフそのものを決める構築時のコストで、上げると挿入が遅くなり、グラフの連結はよくなります。索引は挿入を逐次受け付けるので、コーパスは作り直しなしに増やせます。

**`ivf`** はコーパスを `similarity.ivf_nlist` 個のセルに分割し、1 回の検索で `similarity.ivf_nprobe` 個を探ります。再現率は `ivf_nprobe` を上げると上がり、`ivf_nprobe` を固定したまま `ivf_nlist` を増やすと下がります。分割は学習が必要で、コーパスが `similarity.ivf_train_threshold` 件に達するまで索引は使われず、検索は全件走査の経路に落ちます。新しいベクトルはいったん書き込みバッファに入り、`similarity.ivf_seal_threshold` に従ってまとめて転置リストへ公開されます。バッファにあるベクトルにも検索は届きますが、セルの探索ではなくバッファの走査によって届きます。

これらがあるコーパスでどれだけの再現率を出すかは、推測ではなく計測できます。計測しているのは `tests/benchmark/ann_recall_benchmark.cpp` で、結果は[ベンチマーク](../benchmarks.md)に記録しています。

## `SIMV` に統合検索はありません

`SIMV` が受け取るオプションは `filter` と `min_score` の 2 つで、`using=` はその中にありません。

```bash
nvecd-cli -p 11017 SIMV 3 using=fusion 0.90 0.25 0.12 0.05
```

```text
(error) Invalid SIMV option: using=fusion
```

HTTP の `/simv` にも `mode` フィールドはありません。理由は構造的なものです。統合検索はアイテムのベクトル近傍と共起近傍を混ぜるもので、共起はアイテム識別子を鍵にしています。クエリベクトルには識別子がなく、したがって共起近傍もないので、混ぜる相手が存在しません。

両方が欲しいアプリケーションは、`SIMV` で文書を見つけてから、その結果のひとつに対して `SIM <id> <k> using=fusion` を実行して行動で広げられます。これは 1 回の統合された検索ではなく、2 回の往復と 2 つの順位です。

## 組み込みの例

HTTP API は同じ操作を JSON で扱えるので、検索サービスには行指向のプロトコルより向いています。`api.http.enable: true` で公開します。

```python
import json
import urllib.request

BASE = "http://127.0.0.1:8080"


def post(path, payload):
    request = urllib.request.Request(
        BASE + path,
        data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(request) as response:
        return json.load(response)


def index_document(doc_id, vector, metadata):
    post("/vecset", {"id": doc_id, "vector": vector, "metadata": metadata})


def search(query_vector, top_k=10, min_score=0.0, metadata_filter=None):
    body = {"vector": query_vector, "top_k": top_k, "min_score": min_score}
    if metadata_filter is not None:
        body["filter"] = metadata_filter
    return post("/simv", body)["results"]


index_document("doc_pricing", [0.88, 0.28, 0.13, 0.06], {"lang": "en", "year": 2023})

for hit in search(encode("how to sort a large file"), top_k=3, min_score=0.8, metadata_filter="lang=en"):
    print(hit["id"], hit["score"])
```

`encode` はアプリケーション自身の埋め込み呼び出しです。`POST /vecset` はベクトルとメタデータを 1 回のリクエストで受け取るので、文書の投入は往復 1 回で済みます。`POST /simv` は件数、クエリの次元、順位づけされた結果を返します。

```json
{"count":2,"dimension":4,"results":[{"id":"doc_sorting","score":0.9992},{"id":"doc_indexing","score":0.9964}],"status":"ok"}
```

JSON 応答のスコアは TCP 側が描画するのと同じ小数点以下 4 桁に丸められるので、2 つの面は桁まで一致します。

## 制約

コーパスは 1 プロセスのメモリに載ります。メモリに載らない規模は対象外で、レプリケーションもシャーディングもノードをまたぐ検索もありません。読み取り専用の複製は、複製元で `DUMP SAVE`、ファイルをコピー、複製先で `DUMP LOAD` という手順を運用側でスクリプトにすれば作れます。サーバー側がやってくれるわけではなく、ある時点の複製であって継続的な同期はありません。

テキスト処理は一切ありません。トークナイザも、キーワード索引も、字句とベクトルを組み合わせた順位づけもありません。nvecd が順位づけるのはベクトルで、ベクトルより手前はすべてアプリケーションの領分です。

距離尺度、索引の実装、検索経路の詳細は[ベクトル検索](../vector-search.md)で説明しています。
