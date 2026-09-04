# TCP プロトコル

nvecd は TCP ポート上で行区切りのテキストプロトコルを提供し、設定した場合は Unix ドメインソケット上でも同じものを提供します。このページは全コマンドのリファレンスです。引数の個数、オプショントークン、サーバーが返す正確なバイト列、そして必要な権限を扱います。

## 接続する

TCP リスナーは `api.tcp.bind`（既定値 `127.0.0.1`）の `api.tcp.port`（既定値 `11017`）にバインドします。`network.allow_cidrs` に一致しないアドレスからの接続は応答を返さず切断され、`network.allow_cidrs` が空の場合はすべてのアドレスを拒否するため、到達可能な構成では最低ひとつの CIDR を列挙します。[configuration.md](./configuration.md) を参照してください。

行単位で扱えるクライアントであれば何でも使えます。`nc` の場合は次のようになります。

```bash
$ printf 'VECSET item1 0.1 0.2 0.3 0.4\nSIM item1 5 using=vectors\n' | nc 127.0.0.1 11017
OK VECSET
OK RESULTS 1
item2 0.9940
```

対話的に操作する場合は `telnet` を使います。

```bash
$ telnet 127.0.0.1 11017
Trying 127.0.0.1...
Connected to localhost.
Escape character is '^]'.
INFO
OK INFO
...
END
```

サーバーは接続時にバナーを送りません。クライアントが最初に受け取るバイト列は、最初のコマンドに対する応答です。

`api.unix_socket.path` を設定すると、同じプロトコルがそのソケット上でも提供されます。このソケットは CIDR チェックを完全に迂回し（ソケットファイルのファイルシステム権限がアクセス制御になります）、IP 単位の接続数上限とレート制限も適用されません。

```bash
$ printf 'INFO\n' | nc -U /var/run/nvecd.sock
```

同梱の[クライアントライブラリ](./client-library.md)と `nvecd-cli` はこのプロトコルを話します。`nvecd-cli` については後述します。

## フレーミング

リクエストは `\n` または `\r\n` で終端された 1 行です。コマンドが複数行にまたがることはできません。行中に改行や NUL バイトが含まれる場合は、黙って切り詰めるのではなく拒否します。複数のコマンドを 1 回の書き込みにパイプラインで詰め込むことができ、サーバーは順番に応答します。

応答は CRLF で終端された行の並びで、末尾の CRLF はちょうど 1 個です。ハンドラ本体が素の `\n` で組み立てたテキストもソケットに出る前に正規化されるため、クライアントは常に `\r\n` で分割できます。

`performance.max_query_length`（既定値 1 MiB）を超えるリクエストには `ERROR Request too large` を返し、上限に達するまで改行が届かなかった場合は `ERROR Request too large (no newline detected)` を返します。同じ `ERROR Request too large` は、リアクタのプロセス全体のバッファ予算 `performance.reactor_max_total_buffered_bytes` を使い切った接続にも返されます。ワーカープールがリクエストを受け付けられない場合の応答は `ERROR Server busy` です。どちらの条件でも、応答を送り出したあとに接続を閉じます。

応答の形は 5 種類です。

| 形 | 例 | 使うコマンド |
|---|---|---|
| ステータス行 | `OK VECSET` | 書き込み系、`CACHE`、`DEBUG` |
| 結果セット | `OK RESULTS <n>` に続く `<id> <score>` 行 | `SIM`、`SIMV` |
| ブロック | `+OK` または `OK <VERB>`、本文行、`END` | `INFO`、`CONFIG`、`DUMP INFO`、`DUMP STATUS`、`CACHE STATS` |
| RESP バルク／配列 | `$<len>` と値、`*<n>` と `$<len>`／値の組 | `GET`、`SHOW VARIABLES` |
| エラー | `ERROR <message>` | すべての失敗 |

スコアは小数点以下ちょうど 4 桁で描画されます（`0.9940`）。HTTP 面も同じ精度に丸めるため、同一のクエリに対して両者は同じ数値を報告します。

## 認証と権限

各コマンドは 3 段階の権限のいずれかを持ちます。`security.requirepass` が空の場合、3 段階すべてが開放されています。設定されている場合、未認証の接続で提供されるのは読み取りコマンドだけで、書き込みコマンドと管理コマンドは、その接続で `AUTH` が成功するまで `ERROR NOAUTH Authentication required` で拒否されます。

| 権限 | コマンド |
|---|---|
| 読み取り | `SIM`、`SIMV`、`INFO`、`CONFIG HELP`、`CONFIG SHOW`、`CACHE STATS`、`DEBUG ON`、`DEBUG OFF`、`GET`、`SHOW VARIABLES` |
| 書き込み | `EVENT`、`VECSET`、`VECDEL`、`METASET`、`SET`、`CACHE CLEAR`、`CACHE ENABLE`、`CACHE DISABLE` |
| 管理 | `DUMP SAVE`、`DUMP LOAD`、`DUMP VERIFY`、`DUMP INFO`、`DUMP STATUS`、`CONFIG VERIFY` |

認証は接続単位であり、再接続には引き継がれません。

### `AUTH <password>`

```text
AUTH <password>
```

パスワードは最初の空白またはタブ以降のすべてで、不透明なバイト列として扱われます。トリムも分割も大文字化もしないため、空白を含むパスワードも設定どおりに認証できます。比較は定数時間です。

```text
> AUTH s3cret
+OK

> AUTH wrong
ERROR ERR invalid password
```

パスワードが設定されていないサーバーでは、`AUTH` は成功し、その旨を返します。

```text
> AUTH anything
+OK (no password required)
```

## 書き込みコマンド

### `EVENT`

```text
EVENT <ctx> ADD <id> <score> [timestamp=<epoch_sec>]
EVENT <ctx> SET <id> <score> [timestamp=<epoch_sec>]
EVENT <ctx> DEL <id> [timestamp=<epoch_sec>]
```

アイテム `<id>` がコンテキスト `<ctx>` に現れたことを記録します。`ADD` と `SET` はスコアを取り、`DEL` は取りません。スコアは 0〜100 の範囲の整数（両端を含む）で、サブコマンドのキーワードは大文字小文字を区別しません。`timestamp=` は符号なしの epoch 秒を取り、省略した場合はサーバー自身の時計で刻印します。3 つの形すべてが `OK EVENT` を返します。これは何も変えない重複排除済みの再送でも同じです。

```text
> EVENT user_alice ADD item1 100
OK EVENT

> EVENT user_alice SET item2 90 timestamp=1730000000
OK EVENT

> EVENT user_alice DEL item2
OK EVENT
```

`ADD` は加算し、`SET` はそのコンテキストにおけるそのアイテムの保存済みスコアを置き換え、`DEL` は取り除きます。それぞれが共起グラフに何をするかは [events-and-co-occurrence.md](./events-and-co-occurrence.md) で説明します。

2 つの識別子はどちらも検証されます。空であってはならず、`0x20` 以下のバイトと `0x7F` の削除バイトを含んでもなりません。ID にそうしたバイトが入ると、ある取り込み面が別の面のフレーミングを壊せてしまうためです。`0x80` 以上のバイトはそのまま通すので、マルチバイトの UTF-8 識別子は使えます。空白区切りのコマンドではどちらの違反も送れないため、これらの拒否に届くのは JSON 面からです。

### `VECSET`

```text
VECSET <id> <f1> <f2> ... <fN>
```

`<id>` のベクトルを登録または置換します。次元は浮動小数点トークンの個数から決まり、ストアがすでに使っている次元と一致しなければなりません。最初のベクトルが次元を確定するまでは、その値は `vectors.default_dimension` です。

```text
> VECSET item1 0.1 0.2 0.3 0.4
OK VECSET

> VECSET item1 0.1 0.2 0.3
ERROR Vector dimension mismatch: expected 4, got 3
```

TCP の形はメタデータを運びません。メタデータには `METASET` を使います。

### `VECDEL`

```text
VECDEL <id>
```

ベクトルとそのメタデータを削除します。未知の ID は黙って成功するのではなくエラーになります。

```text
> VECDEL item2
OK VECDEL

> VECDEL nope
ERROR Vector not found: nope
```

### `METASET`

```text
METASET <id> <key:value[,key:value...]>
```

すでにベクトルを持つアイテムにメタデータを付けます。組は空白を含まない 1 トークンで、`filter=` と同じ式の文法をここでも受け付け、各条件のフィールドと値がメタデータの 1 エントリになります。値は綴りから型付けされます。`true`／`false` は真偽値、裸の整数は整数、裸の小数は倍精度浮動小数点、それ以外は文字列のままです。

```text
> METASET item1 category:books,price:12,active:true
OK METASET

> METASET nope category:books
ERROR Vector not found for metadata: nope
```

メタデータは絞り込み結果に広く影響するため、`METASET` が成功するとクエリキャッシュ全体が消去されます。

## 検索コマンド

### `SIM`

```text
SIM <id> <top_k> [using=events|vectors|fusion] [adaptive=on|off] [filter=<expr>] [min_score=<float>]
```

`<id>` に似たアイテムを検索します。`top_k` は正の整数で、`similarity.max_top_k` を超えてはなりません。オプションは `top_k` の後ろであれば任意の順に書けます。認識できないトークンは無視されるのではなく拒否されます。

| オプション | 値 | 既定値 |
|---|---|---|
| `using=` | `events`、`vectors`、`fusion` | `fusion` |
| `adaptive=` | `on`、`off` | サーバーの `similarity.adaptive_fusion` |
| `filter=` | フィルタ式（後述） | フィルタなし |
| `min_score=` | 有限の浮動小数点数 | `0.0` |

`adaptive=` は統合検索にのみ適用されます。`min_score=` は検索後に適用されるため、結果集合を広げるのではなく削ります。

```text
> SIM item1 5 using=vectors
OK RESULTS 1
item2 0.9940

> SIM item1 5 using=fusion adaptive=on min_score=0.5
OK RESULTS 1
item2 0.9947
```

2 つのオプショントークンは認識したうえで明示的に拒否されます。これらを使うクライアントは、絞り込まれていない答えを受け取るのではなく、はっきり失敗します。

```text
> SIM item1 5 candidate_limit=100
ERROR candidate_limit option is not supported

> SIM item1 5 explain=1
ERROR explain option is not supported
```

その他の未知のトークンは構文エラーです。

```text
> SIM item1 5 bogus=1
ERROR Invalid SIM option: bogus=1
```

ベクトルを持たない ID はベクトル検索や統合検索の起点になれません。

```text
> SIM nosuch 5
ERROR Query vector not found: nosuch
```

3 つのモードについては [vector-search.md](./vector-search.md) と [fusion.md](./fusion.md) で説明します。

### `SIMV`

```text
SIMV <top_k> [filter=<expr>] [min_score=<float>] <f1> <f2> ... <fN>
```

既存の ID ではなくクエリベクトルで検索します。すべてのオプショントークンはベクトルより前に置きます。パーサーは `=` を含まない最初のトークンでオプションの解釈をやめ、それ以降を浮動小数点数として読みます。`SIMV` は `using=` も `adaptive=` も受け付けず、常にベクトル検索を行います。

```text
> SIMV 3 0.1 0.2 0.3 0.4
OK RESULTS 2
item1 1.0000
item2 0.9940

> SIMV 3 filter=active:true 0.1 0.2 0.3 0.4
OK RESULTS 1
item1 1.0000
```

クエリベクトルの次元はストアの次元と一致しなければなりません。

```text
> SIMV 3 1 2 3
ERROR Query vector dimension mismatch: expected 4, got 3
```

### `filter=` の文法

フィルタ式はカンマ区切りの条件の並びで、そのすべてが一致する必要があります。各条件は `<field><operator><value>` で、トークン内のどこにも空白を置けません。フィールドは空であってはならず、値も空であってはなりません。値の型付けは `METASET` と同じです。

| 演算子 | 意味 | 例 |
|---|---|---|
| `=` | 等しい | `filter=category=books` |
| `:` | 等しい（`=` の別名） | `filter=category:books` |
| `!=` | 等しくない | `filter=category!=books` |
| `>` | より大きい | `filter=price>10` |
| `<` | より小さい | `filter=price<10` |
| `>=` | 以上 | `filter=price>=20` |
| `<=` | 以下 | `filter=price<=5` |
| `in(a\|b\|c)` | 列挙のいずれか | `filter=category=in(books\|music)` |

`in(...)` は `=` または `:` の後ろの値として書き、要素を `|` で区切ります。空の要素があると式全体が無効になります。条件は `,` で連言として結合され、条件どうしの選言もグループ化もありません。集合を表現する手段が `in(...)` です。

```text
> SIM item1 5 using=vectors filter=active:true,price>10
OK RESULTS 1
item2 0.9940

> SIM item1 5 using=vectors filter=bogus
ERROR Invalid filter condition: 'bogus'
```

メタデータを持たないアイテムはどの条件にも一致しないため、フィルタは `METASET` または HTTP の `/metaset` と `/vecset` を通ったアイテムに結果を絞り込みます。

## 管理コマンド

### `INFO`

```text
INFO
```

サーバー、トラフィック、メモリ、キャッシュ、データの各カウンタを `END` 終端のブロックで報告します。

```text
> INFO
OK INFO

# Server
version: 0.1.0
uptime_seconds: 29

# Stats
total_commands_processed: 29
failed_commands: 8
total_connections_received: 3
active_connections: 1
event_commands: 4
sim_commands: 10
vecset_commands: 2
wal_replay_records_skipped: 0

# Memory
used_memory_bytes: 16
used_memory_human: 0.00 MB
memory_health: HEALTHY

# Cache
cache_entries: 0
cache_hits: 0
cache_misses: 9
cache_hit_rate: 0.0000

# Data
id_count: 2
ctx_count: 1
vector_count: 1
event_count: 4
END
```

`used_memory_bytes` が数えるのはベクトル行列だけです（`vector_count × dimension × 4`）。HTTP の `/info` はストアごとの合計とプロセスの RSS も報告します。`wal_replay_records_skipped` は復旧が復元できなかった WAL レコードの累計で、[persistence.md](./persistence.md) で説明します。

### `CONFIG`

```text
CONFIG HELP [path]
CONFIG SHOW [path]
CONFIG VERIFY <filepath>
```

`CONFIG HELP` はパスなしで設定セクションの一覧を、パスありでそのキーの型、既定値、許容値、説明を表示します。

```text
> CONFIG HELP similarity.index_type
+OK
similarity.index_type

Type: string (enum)
Default: "flat"
Allowed values:
  - "hnsw"
  - "ivf"
  - "flat"
Description: ANN index type: hnsw, ivf, or flat (brute-force)
END
```

`CONFIG SHOW` は稼働中の設定を YAML で表示し、1 セクションに絞ることもできます。`security.requirepass` は `***` に伏せられ、その隣に派生値の `security.auth_enabled` フラグが表示されます。

```text
> CONFIG SHOW cache
+OK
compression_enabled: true
enabled: true
eviction_batch_size: 10
max_memory_mb: 32
min_query_cost_ms: 10.0
ttl_seconds: 3600
END
```

`CONFIG VERIFY` は設定ファイルを適用せずに読み込んで検証します。パスは稼働中の設定ファイルがあったディレクトリの内側で解決され、設定ファイルなしで起動したサーバーの場合はスナップショットディレクトリの内側で解決されます。そのルートの外側に解決されるパス、存在しないパス、開けないパスはすべて同じメッセージで拒否されるため、このコマンドは周囲のファイルシステムについて何も答えません。

```text
> CONFIG VERIFY /etc/passwd
ERROR Configuration file is not accessible
```

### `DUMP`

```text
DUMP SAVE [filename]
DUMP LOAD <filename>
DUMP VERIFY <filename>
DUMP INFO <filename>
DUMP STATUS
```

ファイル名はすべて `snapshot.dir` の内側で解決され、そこから逃げ出すパスは `ERROR Invalid filepath: path traversal detected` で拒否されます。ファイル名を省略できるのは `DUMP SAVE` だけで、その場合は `snapshot.default_filename` に書き込み、これも同じ検証を通ります。`LOAD`、`VERIFY`、`INFO` は空のパスをそれぞれ固有のメッセージで拒否します。

`DUMP LOAD`、`DUMP VERIFY`、`DUMP INFO` は下層で起きたことを解決済みのパスを含む 1 つのメッセージに包むため、ファイルが存在しない場合と壊れている場合はメッセージの形ではなく理由の側で区別します。

`DUMP SAVE` の応答は `snapshot.mode` によって異なり、その違いは言い回しではなく永続性の保証そのものです。`fork` モードではファイルはまだ読めません。

```text
> DUMP SAVE
OK DUMP_SAVE_STARTED /var/lib/nvecd/snapshots/nvecd.nvec
```

`lock` モードでは応答が届いた時点でファイルは完成しています。

```text
> DUMP SAVE
OK DUMP_SAVED /var/lib/nvecd/snapshots/nvecd.nvec
```

`DUMP STATUS` はバックグラウンドの書き込みプロセスを報告します。まだ何も動いていないときのフィールドは `status: idle` だけで、他の 3 状態はパスと時刻を、失敗はさらにメッセージを運びます。

```text
> DUMP STATUS
OK DUMP_STATUS
status: completed
filepath: /var/lib/nvecd/snapshots/nvecd.nvec
start_time: 1788427643
end_time: 1788427643
END
```

`in_progress` 状態は `pid` を、`failed` 状態は `error` を追加します。

`DUMP LOAD` は稼働中のストアをスナップショットの内容で置き換え、ANN 索引を再構築し、キャッシュを消去し、そのスナップショットを復旧の基点にします。`DUMP VERIFY` は読み込まずに整合性を検査します。`DUMP INFO` はヘッダを読みます。

```text
> DUMP INFO nvecd.nvec
OK DUMP_INFO /var/lib/nvecd/snapshots/nvecd.nvec
version: 1
stores: 4
flags: 16
file_size: 476
timestamp: 1788427643
has_statistics: false
END
```

保存や読み込みの実行中は、ストアに触れるコマンドが `ERROR READONLY Snapshot in progress` または `ERROR LOADING Snapshot load in progress` で拒否されます。仕組みは [persistence.md](./persistence.md) で扱います。

### `CACHE`

```text
CACHE STATS
CACHE CLEAR
CACHE ENABLE
CACHE DISABLE
```

`CACHE ENABLE` と `CACHE DISABLE` は実行時変数 `cache.enabled` を設定する短縮形です。この 2 つは `CACHE CLEAR` と同じく書き込み権限を必要とします。

4 つのいずれも、依存先が欠けているときに黙って縮退することはありません。`CACHE STATS` と `CACHE CLEAR` はキャッシュコントローラに触れ、それがない場合は `Cache controller is not initialized` で失敗します。`CACHE ENABLE` と `CACHE DISABLE` はコントローラをまったく参照せず、実行時変数マネージャを経由するため、それがない場合は `Runtime variable manager is not initialized` で失敗します。

```text
> CACHE CLEAR
OK CACHE_CLEARED

> CACHE DISABLE
OK CACHE_DISABLED

> CACHE ENABLE
OK CACHE_ENABLED
```

`CACHE STATS` は `END` 終端のブロックです。

```text
> CACHE STATS
OK CACHE_STATS
cache_enabled: true
cache_entries: 0
cache_memory_bytes: 0
current_memory_mb: 0.00
min_query_cost_ms: 10.00
ttl_seconds: 3600
compression_enabled: true
eviction_batch_size: 10
total_queries: 9
cache_hits: 0
cache_misses: 9
cache_misses_invalidated: 0
cache_misses_not_found: 9
cache_hit_rate: 0.0000
evictions: 0
ttl_expirations: 0
avg_hit_latency_ms: 0.000
avg_miss_latency_ms: 0.000
total_time_saved_ms: 0.00
END
```

各カウンタの意味は [caching.md](./caching.md) で説明します。

### `DEBUG`

```text
DEBUG ON
DEBUG OFF
```

デバッグモードはそれを発行した接続の性質であってサーバーの性質ではなく、接続が閉じると失われます。

```text
> DEBUG ON
OK DEBUG_ON
```

有効な間は、すべての `SIM` と `SIMV` の応答が結果の後ろに追加のブロックを運びます。

```text
> SIM item1 3 using=vectors
OK RESULTS 0
# DEBUG 4
mode: vectors
query_time_us: 2
candidates: 0
results: 0
```

`# DEBUG 4` は続くフィールド数を表し、状態を持つクライアントがこのブロックを枠付けできるようにします。`mode` は検索モード（`SIMV` では `vector`）、`candidates` は `min_score=` を適用する前の結果数、`results` は適用後の数です。キャッシュヒット時の `query_time_us` は `0` です。この計測が対象とするのは、ヒットが置き換えた検索そのものだからです。

### 実行時変数

```text
SET <variable> <value>
GET <variable>
SHOW VARIABLES [LIKE <pattern>]
```

`SET` は `+OK` を返し、`GET` は RESP のバルク文字列を返します。変更可能な変数は `logging.level`、`logging.json`、`cache.enabled`、`cache.min_query_cost_ms`、`cache.ttl_seconds` の 5 つだけで、既知のその他の変数は読み取れますが書き込みは拒否されます。

```text
> SET cache.ttl_seconds 600
+OK

> GET cache.enabled
$4
true

> SET vectors.default_dimension 8
ERROR Variable 'vectors.default_dimension' is immutable (requires restart)

> GET nosuch
ERROR Unknown variable: nosuch
```

真偽値は `true`／`false`、`on`／`off`、`1`／`0`、`yes`／`no` を受け付け、`true` または `false` の正規形で保存されます。

`SHOW VARIABLES` は `<name>=<value> (mutable|immutable)` 行の RESP 配列を返します。`LIKE` は前方一致のパターンを取り、末尾の `%` は取り除かれて残りが接頭辞になります。

```text
> SHOW VARIABLES LIKE cache.%
*6
$42
cache.compression_enabled=true (immutable)
$28
cache.enabled=true (mutable)
$40
cache.eviction_batch_size=10 (immutable)
$34
cache.max_memory_mb=32 (immutable)
$43
cache.min_query_cost_ms=10.000000 (mutable)
$31
cache.ttl_seconds=600 (mutable)
```

入力では `performance.` の接頭辞を `perf.` と綴ることもできますが、内省は常に `performance.` の綴りで報告します。変数の一覧と可変性は [configuration.md](./configuration.md) にあります。

## エラー応答

すべての失敗は `ERROR <message>` の 1 行です。拒否されたコマンドも `total_commands_processed` と `failed_commands` の両方に計上されるため、運用者がアラートを設定する失敗率が 1 を超えることはありません。

| メッセージ | 原因 |
|---|---|
| `Empty command` | 空行 |
| `Command must be a single line` | CR または LF が行中にある |
| `Command must not contain embedded NUL bytes` | リクエストに NUL バイトがある |
| `Unknown command: <NAME>` | 先頭トークンが未知 |
| `Request too large` | `performance.max_query_length` 超過、またはリアクタの共有バッファ予算の枯渇 |
| `Request too large (no newline detected)` | 改行が届く前に上限に達した |
| `Server busy` | ワーカープールがリクエストを受け付けられなかった |
| `NOAUTH Authentication required` | 未認証接続での書き込み／管理コマンド |
| `ERR invalid password` | `AUTH` のパスワード誤り |
| `LOADING Snapshot load in progress` | `DUMP LOAD` が反映中 |
| `READONLY Snapshot in progress` | `lock` モードの `DUMP SAVE` が実行中 |
| `EVENT requires at least 3 arguments: <ctx> <type> <id> [<score>]` | `EVENT` の引数個数 |
| `EVENT ADD requires 4-5 arguments: <ctx> ADD <id> <score> [timestamp=<value>]` | `EVENT ADD` の引数個数 |
| `Invalid EVENT type: <T> (must be ADD, SET, or DEL)` | `EVENT` のサブコマンドが未知 |
| `Score must be in range [0, 100], got <n>` | イベントスコアが範囲外 |
| `Invalid integer: <t>` | 末尾に余分な文字が付いた整数トークン（小数のスコアや `top_k` など） |
| `Failed to parse integer: <t>` | そもそも数値でない整数トークン |
| `Failed to parse timestamp: <v>` | `timestamp=` が数値でない |
| `Context cannot be empty` ／ `ID cannot be empty` | `EVENT` のコンテキストまたはアイテム ID が空 |
| `Context must not contain whitespace or control characters` | `EVENT` のコンテキストに `0x20` 以下または `0x7F` のバイトがある |
| `ID must not contain whitespace or control characters` | `EVENT` のアイテム ID に対する同じ規則 |
| `VECSET requires at least 2 arguments: <id> <floats>` | `VECSET` の引数個数 |
| `Invalid float: <t>` | 末尾に余分な文字が付いた浮動小数点トークン、または `nan` や `inf` などの非有限の綴り |
| `Failed to parse float: <t>` | そもそも数値でない浮動小数点トークン |
| `ID cannot be empty` | アイテム ID が空の `VECSET` |
| `Vector cannot be empty` | 成分のない `VECSET` |
| `Vector dimension exceeds maximum of <n>` | ストアの上限を超える `VECSET` |
| `Vector dimension mismatch: expected <n>, got <m>` | `VECSET` の次元 |
| `Query vector cannot be empty` | 成分のない `SIMV` |
| `Query vector components must be finite` | `SIMV` のクエリベクトルに非有限の成分がある |
| `Query vector norm must be finite and non-zero` | `SIMV` のクエリベクトルのノルムが使えない |
| `Query vector dimension mismatch: expected <n>, got <m>` | `SIMV` の次元 |
| `VECDEL requires 1 argument: <id>` | `VECDEL` の引数個数 |
| `Vector not found: <id>` | 未知の ID への `VECDEL` |
| `Vector not found for metadata: <id>` | ベクトルのないアイテムへの `METASET` |
| `METASET requires 2 arguments: <id> <key:value[,key:value...]>` | `METASET` の引数個数 |
| `SIM requires at least 2 arguments: <id> <top_k>` | `SIM` の引数個数 |
| `SIMV requires at least 2 arguments: <top_k> <floats>` | `SIMV` の引数個数 |
| `SIMV requires at least one vector float` | トークンがすべてオプションだった `SIMV` |
| `top_k must be positive, got <n>` | `top_k` が正でない |
| `top_k <n> exceeds maximum allowed: <m>` | `top_k` が `similarity.max_top_k` 超過 |
| `Invalid using value: <v> (must be events, vectors, or fusion)` | モードが未知 |
| `Invalid adaptive value: <v> (must be on or off)` | `adaptive=` の値が未知 |
| `Invalid SIM option: <t>` ／ `Invalid SIMV option: <t>` | オプショントークンが未知 |
| `candidate_limit option is not supported` | `candidate_limit=` |
| `explain option is not supported` | `explain=` |
| `Invalid filter condition: '<pair>'` | `filter=` の条件が不正 |
| `Query vector not found: <id>` | ベクトルのない ID への `SIM` |
| `CONFIG requires subcommand: HELP\|SHOW\|VERIFY` | `CONFIG` の引数個数 |
| `Unknown CONFIG subcommand: <S>` | `CONFIG` のサブコマンドが未知 |
| `Configuration file is not accessible` | `CONFIG VERIFY` のパスが許可ルート外 |
| `DUMP requires subcommand: SAVE\|LOAD\|VERIFY\|INFO\|STATUS` | `DUMP` の引数個数 |
| `DUMP LOAD requires a filepath` | 引数のない `DUMP LOAD` |
| `DUMP VERIFY requires a filepath` | 引数のない `DUMP VERIFY` |
| `DUMP INFO requires a filepath` | 引数のない `DUMP INFO` |
| `Invalid filepath: path traversal detected` | スナップショットパスが `snapshot.dir` から逃げている |
| `Failed to load snapshot from <path>: <reason>` | `DUMP LOAD` のすべての失敗。整合性の詳細がある場合は括弧で追記される |
| `Snapshot verification failed for <path>: <reason>` | `DUMP VERIFY` のすべての失敗。同じ詳細が付くことがある |
| `Failed to read snapshot info from <path>: <reason>` | `DUMP INFO` がヘッダを読めなかった |
| `Another snapshot load is already in progress` | 2 つ目の `DUMP LOAD` が同時に来た |
| `Another snapshot save is already in progress` | 2 つ目の `lock` モード `DUMP SAVE` が同時に来た |
| `A snapshot save is already in progress` | 保存がストアを掴んでいる間の `DUMP LOAD` |
| `Cannot save snapshot while a snapshot load is in progress` | 読み込み中の `DUMP SAVE` |
| `CACHE requires subcommand: STATS\|CLEAR\|ENABLE\|DISABLE` | `CACHE` の引数個数 |
| `Cache controller is not initialized` | キャッシュコントローラのないサーバーでの `CACHE STATS` または `CACHE CLEAR` |
| `Runtime variable manager is not initialized` | 変数マネージャがない状態での `CACHE ENABLE` または `CACHE DISABLE` |
| `DEBUG requires exactly one argument: ON\|OFF` | `DEBUG` の引数個数 |
| `SET requires 2 arguments: <variable_name> <value>` | `SET` の引数個数 |
| `Unknown variable: <name>` | 実行時変数が未知 |
| `Variable '<name>' is immutable (requires restart)` | 変更不可の変数への書き込み |

エラーコードはモジュールごとに区切られています。1000〜1999 は設定、2000〜2999 はイベント処理、3000〜3999 はコマンド解析、4000〜4999 はベクトルと類似度、5000〜5999 はストレージとスナップショット、6000〜6999 はネットワークとサーバー、7000〜7999 はクライアント、8000〜8999 はキャッシュです。TCP 面が報告するのはメッセージだけで、[HTTP 面](./http-api.md)はコードをステータスに対応付けます。

## nvecd-cli

`nvecd-cli` は同じプロトコルを話す REPL であり、単発コマンドの実行環境でもあります。接続は TCP のみで、Unix ソケットのオプションはありません。

```text
Usage: nvecd-cli [OPTIONS] [COMMAND]
```

| フラグ | 意味 |
|---|---|
| `-h HOST` | サーバーのホスト名または IPv4 アドレス（既定値 `127.0.0.1`） |
| `-p PORT` | サーバーのポート（既定値 `11017`） |
| `--retry N` | 接続を拒否されたとき N 回再試行する（既定値 `0`） |
| `--wait-ready` | サーバーが受け付けるまで最大 100 回再試行する |
| `--password-file FILE` | `AUTH` のパスワードを専用ファイルから読む |
| `--password-env NAME` | `AUTH` のパスワードを環境変数から読む |
| `--help` | 使い方を表示して終了する |

既知のフラグでない最初の引数からコマンドが始まります。そこから先はすべて半角空白 1 個で連結されて 1 つのコマンドとして送られ、プロセスは終了します。コマンドを与えない場合は対話モードに入り、`help` でコマンド一覧を表示し、`quit` または `exit` で抜けます。

```bash
$ nvecd-cli -p 11017 INFO
INFO

# Server
version: 0.1.0
...

$ nvecd-cli -p 11017 BOGUS; echo "exit=$?"
(error) Unknown command: BOGUS
exit=1
```

エラー応答は非ゼロで終了するため、単発の形はスクリプトで使えます。再試行が効くのは接続を拒否された場合だけです。名前解決に失敗した場合やその他の接続エラーは即座に失敗します。

`--password-file` と `--password-env` は排他で、どちらか一方だけを指定できます。パスワードファイルは実行ユーザーが所有するモード `0600` 以下の通常ファイルであり、4096 バイトを超えず、CR、LF、NUL を含まないことが必要です。末尾の改行 1 個は取り除かれます。パスワードは接続直後に `AUTH` で送られ、拒否された場合はコマンドを 1 つも実行せずに中断します。

```bash
$ NVECD_PASSWORD=s3cret nvecd-cli --password-env NVECD_PASSWORD VECSET item1 0.1 0.2 0.3 0.4
VECSET
```

readline が利用できる環境でビルドした場合、対話モードには履歴と、コマンド名・サブコマンド・`using=` の値に対する文脈依存のタブ補完が付きます。
