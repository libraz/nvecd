# クライアントライブラリ

`libnvecdclient` は、管理された接続の上で [TCP プロトコル](./protocol.md)を話す共有ライブラリです。ひとつのバイナリから 2 つの入口を提供します。`Expected<T, Error>` を返す C++ クラスと、FFI バインディングに適した整数の戻り値を持つ C API です。

## インストールされるもの

`make install` はライブラリとヘッダをインストール先の接頭辞の下に配置します。

```text
<prefix>/lib/libnvecdclient.so         （macOS では libnvecdclient.dylib）
<prefix>/include/nvecd/nvecdclient.h
<prefix>/include/nvecd/nvecdclient_c.h
<prefix>/include/nvecd/utils/error.h
<prefix>/include/nvecd/utils/expected.h
<prefix>/lib/cmake/nvecd/nvecdConfig.cmake
<prefix>/lib/cmake/nvecd/nvecdTargets.cmake
```

`nvecdclient.h` は自身からの相対で `utils/error.h` を取り込むため、追加すべきインクルードディレクトリは `<prefix>/include/nvecd` であり、ヘッダは名前だけで指定します。

## ライブラリに対してビルドする

CMake であれば、エクスポートされたターゲットが両方を担います。

```cmake
find_package(nvecd CONFIG REQUIRED)

add_executable(app main.cpp)
target_link_libraries(app PRIVATE nvecd::client)
```

`nvecd::client` は `nvecdclient` ターゲットの別名で、インクルードディレクトリを持ち回るため `target_include_directories` は不要です。

手で書く場合は次のようになります。

```bash
c++ -std=c++17 -I /usr/local/include/nvecd main.cpp -L /usr/local/lib -lnvecdclient -o app
```

C のプログラムには C ヘッダだけが必要で、C++ 標準ライブラリのフラグは要りません。

```bash
cc -I /usr/local/include/nvecd main.c -L /usr/local/lib -lnvecdclient -o app
```

ライブラリのビルドには C++17 が必要ですが、C API の利用者は通常の共有ライブラリとしてリンクするだけで、それは不要です。

## 接続する

どちらの API もハンドルごとに接続を 1 本開き、切断するかハンドルを破棄するまで開いたままにします。

```cpp
nvecd::client::ClientConfig config;
config.host = "127.0.0.1";
config.port = 11017;
config.timeout_ms = 5000;
config.recv_buffer_size = 65536;
```

| フィールド | 型 | 既定値 | 意味 |
|---|---|---|---|
| `host` | `std::string` | `"127.0.0.1"` | サーバーのホスト名または IPv4 アドレス |
| `port` | `uint16_t` | `11017` | サーバーのポート |
| `timeout_ms` | `uint32_t` | `5000` | 送受信のタイムアウト |
| `recv_buffer_size` | `uint32_t` | `65536` | 受信バッファのサイズ |
| `unix_socket_path` | `std::string` | `""` | Unix ドメインソケットのパス |

`unix_socket_path` を設定すると転送方式が切り替わり、クライアントはそのソケットに接続して `host` と `port` を無視します。

```cpp
nvecd::client::ClientConfig config;
config.unix_socket_path = "/var/run/nvecd.sock";
```

`NvecdClientConfig_C` には `unix_socket_path` にあたるフィールドがないため、C API は TCP でしか接続できません。Unix ソケットが必要な場合は C++ API を使います。

サーバーが `security.requirepass` を設定している場合、接続後かつ最初の書き込みや管理コマンドの前に `Auth`（または `nvecdclient_auth`）を 1 回呼びます。認証は接続に属するため、再接続したときは呼び直す必要があります。

## C++ API

`nvecd::client::NvecdClient` はムーブ構築・ムーブ代入が可能で、コピーはできません。ムーブ元のインスタンスは有効なまま破棄可能です。`IsConnected()` は false を返し、`Disconnect()` は何もせず、すべてのコマンドは譲り渡した実装を参照するのではなく `ErrorCode::kClientNotConnected` を返します。

### ライフサイクル

| シグネチャ | 戻り値 |
|---|---|
| `explicit NvecdClient(ClientConfig config)` | — |
| `Expected<void, Error> Connect()` | 成功、または接続エラー |
| `void Disconnect()` | — |
| `bool IsConnected() const` | ソケットが開いているか |

デストラクタは切断します。

### データコマンド

| シグネチャ | 戻り値 |
|---|---|
| `Expected<void, Error> Event(const std::string& ctx, const std::string& type, const std::string& id, int score = 0) const` | 成功またはエラー |
| `Expected<void, Error> Vecset(const std::string& id, const std::vector<float>& vector) const` | 成功またはエラー |
| `Expected<void, Error> Vecdel(const std::string& id) const` | 成功またはエラー |
| `Expected<void, Error> Metaset(const std::string& id, const std::string& metadata) const` | 成功またはエラー |

`type` は大文字の `"ADD"`、`"SET"`、`"DEL"` のいずれかでなければなりません。それ以外はクライアント側で `kClientInvalidArgument` として拒否し、送信しません。`ADD` と `SET` のスコアは 0〜100 に収まる必要があり、これもクライアント側で検査します。`DEL` は `score` を無視し、ワイヤ上のコマンドからも省きます。

`Metaset` が取るのは**文字列の式**であって、構造化されたマップではありません。

```cpp
client.Metaset("item1", "category:books,active:true");
```

これは TCP の `METASET` が受け付ける形であり、HTTP の `/metaset` ルートが受け付ける形ではありません。あちらは `metadata` キーの下に JSON オブジェクトを取ります。HTTP 面から移植したコードは、マップを `key:value,key:value` に変換してからこのメソッドを呼ぶ必要があります。式に空白を含めることはできず、空の式はクライアント側で拒否されます。

すべての文字列引数は、行プロトコルが運べないバイト（NUL、CR、LF）と、コマンドが空白で区切られる位置の空白について検査されます。それ以外は何も濾さないため、クライアントはサーバーが受け付けるのとまったく同じバイト列を受け付けます。

### 検索

```cpp
struct SearchOptions {
  std::string filter;              // メタデータのフィルタ式
  std::optional<float> min_score;  // スコアの下限
  std::optional<bool> adaptive;    // 適応的な統合の切り替え。SIM の統合検索のみ
};
```

| シグネチャ |
|---|
| `Expected<SimResponse, Error> Sim(const std::string& id, uint32_t top_k = 10, const std::string& mode = "fusion", const SearchOptions& options = {}) const` |
| `Expected<SimResponse, Error> Simv(const std::vector<float>& vector, uint32_t top_k = 10, const std::string& mode = "vectors", const SearchOptions& options = {}) const` |

```cpp
struct SimResultItem { std::string id; float score; };
struct SimResponse { std::vector<SimResultItem> results; std::string mode; };
```

`SearchOptions` の未設定フィールドはコマンドから省かれ、サーバーの既定値がそのまま効きます。`filter` は [protocol.md](./protocol.md) に記載した文法を使います。

`Sim` は `mode` が空でなければ `using=<mode>` を、`options.adaptive` が設定されていれば `adaptive=on|off` を送ります。`Simv` はどちらも送りません。ワイヤ上のコマンドにモードも adaptive オプションも存在しないため、`Simv` の `mode` 引数はプロトコルトークンとして検査されたうえで、戻り値の `SimResponse::mode` を埋めるためだけに使われます。`Simv` に `"events"` を渡しても共起検索にはなりません。

空のクエリベクトルはクライアント側で `kClientInvalidArgument` として拒否されます。

### 管理

| シグネチャ | 戻り値 |
|---|---|
| `Expected<void, Error> Auth(const std::string& password) const` | 成功、またはパスワード誤りのエラー |
| `Expected<ServerInfo, Error> Info() const` | 解析済みの `INFO` カウンタ |
| `Expected<std::string, Error> GetConfig() const` | `CONFIG SHOW` の生ブロック |
| `Expected<SaveResult, Error> Save(const std::string& filepath = "") const` | スナップショットのパスと完了状態 |
| `Expected<std::string, Error> Load(const std::string& filepath) const` | 読み込んだパス |
| `Expected<std::string, Error> Verify(const std::string& filepath) const` | 検証の応答 |
| `Expected<std::string, Error> DumpInfo(const std::string& filepath) const` | `DUMP INFO` の生ブロック |
| `Expected<std::string, Error> DumpStatus() const` | `DUMP STATUS` の生ブロック |
| `Expected<std::string, Error> CacheStats() const` | `CACHE STATS` の生ブロック |
| `Expected<void, Error> CacheClear() const` | 成功またはエラー |
| `Expected<void, Error> CacheEnable() const` | 成功またはエラー |
| `Expected<void, Error> CacheDisable() const` | 成功またはエラー |
| `Expected<void, Error> EnableDebug() const` | 成功またはエラー |
| `Expected<void, Error> DisableDebug() const` | 成功またはエラー |
| `Expected<std::string, Error> SendCommand(const std::string& command) const` | 応答の生の行またはブロック |

`ServerInfo` は `INFO` のキーに対応します。

```cpp
struct ServerInfo {
  std::string version;
  uint64_t uptime_seconds;
  uint64_t total_commands_processed;
  uint64_t failed_commands;
  uint64_t total_connections;      // INFO のキー: total_connections_received
  uint64_t active_connections;
  uint64_t event_count;
  uint64_t vector_count;
  uint64_t id_count;
  uint64_t ctx_count;
};
```

`SaveResult` は 2 つの成功応答を区別します。これは 2 通りの言い方ではなく、2 つの異なる永続性の保証です。

```cpp
struct SaveResult {
  std::string filepath;   // サーバーが報告したパス
  bool completed;         // ファイルが完成していて読み込めるとき true
};
```

既定の `snapshot.mode: fork` では `completed` は false で、`filepath` はまだ読めません。存在しないか、以前のスナップショットのままである可能性があります。コピーや読み込みの前に `DumpStatus()` をポーリングして、書き込みの完了を待ちます。`snapshot.mode: lock` では `completed` は true で、`Save` が返った時点でファイルは使えます。

`SendCommand` はクラスがラップしていないコマンド（`SET`、`GET`、`SHOW VARIABLES` など）のための逃げ道です。末尾の CRLF を取り除いた応答を返し、内容の分類はしないため、`ERROR` で始まるかどうかは呼び出し側が判定します。

`EnableDebug` と `DisableDebug` は接続のデバッグモードを切り替え、以降のすべての `SIM` と `SIMV` の応答に `# DEBUG` ブロックが追加されます。`Sim` と `Simv` のパーサーは先頭の結果セットを読んで止まるため、このブロックは誤って解析されるのではなく捨てられます。

### エラー処理

すべてのメソッドが `nvecd::utils::Expected<T, nvecd::utils::Error>` を返します。文脈依存で `bool` に変換でき、`*result` と `result->` で値に、`result.error()` でエラーに到達します。

```cpp
auto result = client.Sim("item1", 10, "fusion");
if (!result) {
  std::cerr << result.error().message() << '\n';        // 人が読めるテキスト
  std::cerr << static_cast<int>(result.error().code()); // 数値の ErrorCode
  return;
}
for (const auto& item : result->results) {
  std::cout << item.id << ' ' << item.score << '\n';
}
```

`Error::to_string()` は両方をまとめて `[Connection failed (7001)] Connection failed: Connection refused` の形で描画します。

エラーの発生源はコードで 3 つに区別できます。

| 範囲 | 発生源 |
|---|---|
| `kClientInvalidArgument`（7009） | クライアントが何も送らずに引数を拒否した |
| `kClientNotConnected`（7000）、`kClientConnectionFailed`（7001）、`kClientSendFailed`（7002）、`kClientReceiveFailed`（7003）、`kClientTimeout`（7005）、`kClientConnectionClosed`（7008） | 転送層 |
| `kClientServerError`（7010） | サーバーが `ERROR` を返した。メッセージはサーバーのテキスト |
| `kClientProtocolError`（7011） | 応答が期待した形でなかった |

サーバー側のエラーでは接続は使えるままです。転送層のエラーではそうならず、クライアントは自分で切断します。

### 完全な例

```cpp
#include <iostream>
#include <vector>

#include <nvecdclient.h>

int main() {
  nvecd::client::ClientConfig config;
  config.host = "127.0.0.1";
  config.port = 11017;

  nvecd::client::NvecdClient client(config);
  auto connected = client.Connect();
  if (!connected) {
    std::cerr << connected.error().message() << '\n';
    return 1;
  }
  if (auto authed = client.Auth("s3cret"); !authed) {
    std::cerr << authed.error().message() << '\n';
    return 1;
  }

  const std::vector<float> vector = {0.1F, 0.2F, 0.3F, 0.4F};
  if (auto stored = client.Vecset("item1", vector); !stored) {
    std::cerr << stored.error().message() << '\n';
    return 1;
  }
  client.Metaset("item1", "category:books,active:true");
  client.Event("user_alice", "ADD", "item1", 90);

  nvecd::client::SearchOptions options;
  options.filter = "category:books";
  options.min_score = 0.5F;
  auto results = client.Sim("item1", 5, "vectors", options);
  if (!results) {
    std::cerr << results.error().message() << '\n';
    return 1;
  }
  for (const auto& item : results->results) {
    std::cout << item.id << ' ' << item.score << '\n';
  }
  return 0;
}
```

## C API

`nvecdclient_c.h` は同じ実装を不透明な `NvecdClient_C*` の背後に包みます。FFI バインディングが使うのはこの面です。

すべての関数は成功で `0`、失敗で `-1` を返します。例外は `nvecdclient_save`（`1` も返します）と、後述の問い合わせ関数および解放関数です。境界を越えて例外が飛ぶことはありません。想定外の例外はラッパーで捕捉され、同じ失敗コードに変換されます。

### ライフサイクル

```c
NvecdClient_C* nvecdclient_create(const NvecdClientConfig_C* config);
void           nvecdclient_destroy(NvecdClient_C* client);
int            nvecdclient_connect(NvecdClient_C* client);
void           nvecdclient_disconnect(NvecdClient_C* client);
int            nvecdclient_is_connected(const NvecdClient_C* client);
```

```c
typedef struct {
  const char* host;
  uint16_t    port;
  uint32_t    timeout_ms;
  uint32_t    recv_buffer_size;
} NvecdClientConfig_C;
```

`nvecdclient_create` は `config` が `NULL` の場合と確保に失敗した場合に `NULL` を返します。各フィールドは `NULL` またはゼロのとき既定値（`"127.0.0.1"`、`11017`、`5000`、`65536`）にフォールバックします。構造体はコピーされるため、呼び出し側はすぐに解放できます。`nvecdclient_is_connected` は `1` または `0` を返します。

接続はハンドルが所有し、`nvecdclient_destroy` が切断して解放します。どの関数も `NULL` を渡されるとクラッシュせずに失敗コードを返し、`nvecdclient_destroy(NULL)` は何もしません。

### データコマンド

```c
int nvecdclient_event(NvecdClient_C* client, const char* ctx, const char* type,
                      const char* id, int score);
int nvecdclient_vecset(NvecdClient_C* client, const char* id,
                       const float* vector, size_t dimension);
int nvecdclient_vecdel(NvecdClient_C* client, const char* id);
int nvecdclient_metaset(NvecdClient_C* client, const char* id, const char* metadata);
```

`type` は `"ADD"`、`"SET"`、`"DEL"` のいずれかで、`"DEL"` では `score` は無視されます。`vector` は `dimension` 個の float を指し、呼び出しが返る前にコピーされます。

`metadata` は C++ の `Metaset` と同じ文字列の式（`"category:books,active:true"`）であって、JSON ではありません。HTTP の `/metaset` のオブジェクト引数を模したバインディングは、先に平坦化する必要があります。

### 検索

```c
typedef struct {
  const char* filter;      /* NULL または "" = フィルタなし */
  float       min_score;
  int         has_min_score;
  int         adaptive;
  int         has_adaptive;
} NvecdSearchOptions_C;

typedef struct { char* id; float score; } NvecdSimResultItem_C;

typedef struct {
  NvecdSimResultItem_C* results;
  size_t                count;
  char*                 mode;
} NvecdSimResponse_C;

int nvecdclient_sim(NvecdClient_C* client, const char* id, uint32_t top_k,
                    const char* mode, NvecdSimResponse_C** result);
int nvecdclient_sim_ex(NvecdClient_C* client, const char* id, uint32_t top_k,
                       const char* mode, const NvecdSearchOptions_C* options,
                       NvecdSimResponse_C** result);
int nvecdclient_simv(NvecdClient_C* client, const float* vector, size_t dimension,
                     uint32_t top_k, const char* mode, NvecdSimResponse_C** result);
int nvecdclient_simv_ex(NvecdClient_C* client, const float* vector, size_t dimension,
                        uint32_t top_k, const char* mode,
                        const NvecdSearchOptions_C* options,
                        NvecdSimResponse_C** result);
```

`mode` が `NULL` の場合、`sim` 系の 2 関数では `"fusion"`、`simv` 系の 2 関数では `"vectors"` になります。`_ex` 系は絞り込みを加えます。`options` は `NULL` でもよく、`min_score` と `adaptive` はそれぞれの `has_` が非ゼロのときにだけ読まれます。`adaptive` がワイヤに届くのは `sim_ex` からだけです。`SIMV` には adaptive オプションがないためです。

成功時、`*result` にはヒープ上に確保された応答が入り、その所有権は呼び出し側にあります。解放には `nvecdclient_free_sim_response` を使います。この 1 回の呼び出しで各 `id`、`results` 配列、`mode`、そして応答自身が解放されるため、メンバを個別に解放すると二重解放になります。失敗時、`*result` は書き換えられません。

### 管理

```c
int nvecdclient_auth(NvecdClient_C* client, const char* password);
int nvecdclient_info(NvecdClient_C* client, NvecdServerInfo_C** info);
int nvecdclient_get_config(NvecdClient_C* client, char** config_str);
int nvecdclient_save(NvecdClient_C* client, const char* filepath, char** saved_path);
int nvecdclient_load(NvecdClient_C* client, const char* filepath, char** loaded_path);
int nvecdclient_verify(NvecdClient_C* client, const char* filepath, char** result_str);
int nvecdclient_dump_info(NvecdClient_C* client, const char* filepath, char** info_str);
int nvecdclient_dump_status(NvecdClient_C* client, char** status_str);
int nvecdclient_cache_stats(NvecdClient_C* client, char** stats_str);
int nvecdclient_cache_clear(NvecdClient_C* client);
int nvecdclient_cache_enable(NvecdClient_C* client);
int nvecdclient_cache_disable(NvecdClient_C* client);
int nvecdclient_debug_on(NvecdClient_C* client);
int nvecdclient_debug_off(NvecdClient_C* client);
```

3 通りの結果を持つ関数は `nvecdclient_save` だけです。

| 戻り値 | 意味 |
|---|---|
| `0` | スナップショットは完成しており読み込める |
| `1` | バックグラウンドの書き込みが始まった。ファイルはまだ使えない |
| `-1` | 保存に失敗した |

`*saved_path` は成功の 2 コードのどちらでも書き込まれます。`1` は、ファイルに触れる前に `nvecdclient_dump_status` をポーリングすることを意味します。`!= 0` を失敗として扱うと fork モードでの成功した保存を取りこぼし、`>= 0` を「ファイルが使える」と解釈するとバックアップスクリプトにまだ存在しないパスを渡すことになります。

`nvecdclient_save` の `filepath` は `NULL` でもよく、その場合はサーバーの設定済みの既定ファイル名を使います。`load`、`verify`、`dump_info` では必須です。

### 所有権

| 生成する関数 | 解放に使う関数 |
|---|---|
| `nvecdclient_create` | `nvecdclient_destroy` |
| `nvecdclient_sim`、`nvecdclient_sim_ex`、`nvecdclient_simv`、`nvecdclient_simv_ex` | `nvecdclient_free_sim_response` |
| `nvecdclient_info` | `nvecdclient_free_server_info` |
| `nvecdclient_get_config`、`nvecdclient_save`、`nvecdclient_load`、`nvecdclient_verify`、`nvecdclient_dump_info`、`nvecdclient_dump_status`、`nvecdclient_cache_stats` | `nvecdclient_free_string` |
| `nvecdclient_get_last_error` | 解放しない |

```c
void nvecdclient_free_sim_response(NvecdSimResponse_C* result);
void nvecdclient_free_server_info(NvecdServerInfo_C* info);
void nvecdclient_free_string(char* str);
```

3 つとも `NULL` を受け付け、その場合は何もしません。`nvecdclient_free_server_info` は `version` 文字列と構造体を解放します。

### エラー処理

```c
const char* nvecdclient_get_last_error(const NvecdClient_C* client);
```

この関数はクライアントハンドルを引数に取ります。引数なしで呼ぶこと、あるいは FFI 層にシグネチャを宣言しないことが、誤ったメモリを読む最もよくある原因です。

返されるバッファは呼び出したスレッドに属し、そのスレッドが次にこの関数を呼ぶまで有効です。同じハンドルを使う他のスレッドがこれを無効化することはありません。解放してはならず、次の呼び出しより長く保持する必要があるならコピーします。`NULL` ハンドルを渡した場合は `"Invalid client handle"` を返します。メッセージは `Error::to_string()` の描画なので、コードも運びます。

```text
[Connection failed (7001)] Connection failed: Connection refused
```

メッセージが記録されるのは失敗したときだけなので、`-1` の直後に読みます。

### 完全な例

```c
#include <stdio.h>
#include <nvecdclient_c.h>

int main(void) {
  NvecdClientConfig_C config = {0};
  config.host = "127.0.0.1";
  config.port = 11017;

  NvecdClient_C* client = nvecdclient_create(&config);
  if (client == NULL) {
    fprintf(stderr, "client creation failed\n");
    return 1;
  }

  if (nvecdclient_connect(client) != 0) {
    fprintf(stderr, "%s\n", nvecdclient_get_last_error(client));
    nvecdclient_destroy(client);
    return 1;
  }

  const float vector[4] = {0.1f, 0.2f, 0.3f, 0.4f};
  if (nvecdclient_vecset(client, "item1", vector, 4) != 0) {
    fprintf(stderr, "%s\n", nvecdclient_get_last_error(client));
  }
  nvecdclient_metaset(client, "item1", "category:books,active:true");

  NvecdSearchOptions_C options = {0};
  options.filter = "category:books";
  options.min_score = 0.5f;
  options.has_min_score = 1;

  NvecdSimResponse_C* result = NULL;
  if (nvecdclient_sim_ex(client, "item1", 5, "vectors", &options, &result) == 0) {
    for (size_t i = 0; i < result->count; ++i) {
      printf("%s %f\n", result->results[i].id, result->results[i].score);
    }
    nvecdclient_free_sim_response(result);
  } else {
    fprintf(stderr, "%s\n", nvecdclient_get_last_error(client));
  }

  nvecdclient_disconnect(client);
  nvecdclient_destroy(client);
  return 0;
}
```

## スレッド安全性

どちらの API でも、ひとつのハンドルを複数のスレッドから同時に使えます。ハンドル上のコマンドは内部で直列化され、C のハンドルのエラー領域は同期されているため、破損したメモリや解放済みのエラーメッセージを観測する呼び出しはありません。

直列化は並列とは違います。遅いコマンドは後ろに並んだコマンドを待たせます。スレッドをまたいだスループットは、ひとつのハンドルを共有することではなく、それぞれが自分の接続を持つ複数のハンドルから得られます。

ハンドルの生成と破棄は同期されません。`nvecdclient_destroy` の実行中に進行中の呼び出しがあってはならず、C++ 側でもオブジェクトのムーブ元になる間や破棄の間に進行中の呼び出しがあってはなりません。

## 接続の再利用

接続には TCP のハンドシェイクが要り、`security.requirepass` が設定されている場合は `AUTH` の往復も要ります。リクエストをまたいで開いたままのハンドルはそのどちらも払い直さないため、リクエストごとに接続するのではなく、寿命の長いハンドル（あるいはその小さなプール）を土台にします。

`IsConnected()` が報告するのはローカルのソケット状態であって到達性ではありません。前回のコマンド以降に消えたサーバーは、次のコマンドが転送層のエラーで失敗することで判明します。再接続は同じハンドルで `Connect()` を呼び直し、サーバーが要求するなら `Auth` を続けます。

転送層の失敗はハンドルを切断するため、再試行のループはコマンドを再送する前に `Connect()` を呼びます。サーバー側の `ERROR` は接続を使える状態に保つので、再接続は不要です。

## ctypes による Python バインディング

C API は `ctypes` から直接呼び出せます。使うすべての関数について `argtypes` と `restype` を宣言することは、64 ビット環境では任意ではありません。宣言しないと `ctypes` は戻り値を `int` と仮定してポインタを切り詰めます。

```python
import ctypes

lib = ctypes.CDLL("/usr/local/lib/libnvecdclient.so")  # macOS では .dylib


class NvecdClientConfig(ctypes.Structure):
    _fields_ = [
        ("host", ctypes.c_char_p),
        ("port", ctypes.c_uint16),
        ("timeout_ms", ctypes.c_uint32),
        ("recv_buffer_size", ctypes.c_uint32),
    ]


class NvecdSimResultItem(ctypes.Structure):
    _fields_ = [("id", ctypes.c_char_p), ("score", ctypes.c_float)]


class NvecdSimResponse(ctypes.Structure):
    _fields_ = [
        ("results", ctypes.POINTER(NvecdSimResultItem)),
        ("count", ctypes.c_size_t),
        ("mode", ctypes.c_char_p),
    ]


lib.nvecdclient_create.restype = ctypes.c_void_p
lib.nvecdclient_create.argtypes = [ctypes.POINTER(NvecdClientConfig)]
lib.nvecdclient_destroy.argtypes = [ctypes.c_void_p]
lib.nvecdclient_connect.argtypes = [ctypes.c_void_p]
lib.nvecdclient_disconnect.argtypes = [ctypes.c_void_p]
lib.nvecdclient_auth.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
lib.nvecdclient_vecset.argtypes = [
    ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_float), ctypes.c_size_t
]
lib.nvecdclient_metaset.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
lib.nvecdclient_sim.argtypes = [
    ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint32, ctypes.c_char_p,
    ctypes.POINTER(ctypes.POINTER(NvecdSimResponse)),
]
lib.nvecdclient_free_sim_response.argtypes = [ctypes.POINTER(NvecdSimResponse)]
lib.nvecdclient_get_last_error.argtypes = [ctypes.c_void_p]
lib.nvecdclient_get_last_error.restype = ctypes.c_char_p

config = NvecdClientConfig(
    host=b"127.0.0.1", port=11017, timeout_ms=5000, recv_buffer_size=65536
)
client = lib.nvecdclient_create(ctypes.byref(config))
if not client:
    raise SystemExit("client creation failed")

if lib.nvecdclient_connect(client) != 0:
    raise SystemExit(lib.nvecdclient_get_last_error(client).decode())
if lib.nvecdclient_auth(client, b"s3cret") != 0:
    raise SystemExit(lib.nvecdclient_get_last_error(client).decode())

vector = (ctypes.c_float * 4)(0.1, 0.2, 0.3, 0.4)
if lib.nvecdclient_vecset(client, b"item1", vector, 4) != 0:
    raise SystemExit(lib.nvecdclient_get_last_error(client).decode())
lib.nvecdclient_metaset(client, b"item1", b"category:books,active:true")

response = ctypes.POINTER(NvecdSimResponse)()
if lib.nvecdclient_sim(client, b"item1", 5, b"vectors", ctypes.byref(response)) != 0:
    raise SystemExit(lib.nvecdclient_get_last_error(client).decode())
for i in range(response.contents.count):
    item = response.contents.results[i]
    print(item.id.decode(), item.score)
lib.nvecdclient_free_sim_response(response)

lib.nvecdclient_disconnect(client)
lib.nvecdclient_destroy(client)
```

`nvecdclient_get_last_error` はハンドルを引数に取り、その `restype` は `c_char_p` でなければなりません。どちらも書き落としやすく、どちらもエラーにならずに誤った答えを返します。メタデータの引数は `dict` ではなく平坦な `key:value,key:value` のバイト列です。

他の FFI 層でも規則は同じです。すべてのシグネチャを宣言し、所有権の表に従い、失敗の直後に `nvecdclient_get_last_error(handle)` を読みます。バインディングが何を宣言すべきかの基準はヘッダです。
