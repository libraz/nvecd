# libnvecdclient - Nvecd クライアントライブラリ

## 概要

libnvecdclient は nvecd サーバーに接続してクエリを実行するための C/C++ クライアントライブラリです。モダンな C++ API と、他言語バインディング向けの C API の両方を提供します。

## 機能

- すべての nvecd プロトコルコマンドに完全対応
- RAII と型安全性を備えたモダンな C++17 API
- 他言語との統合が容易な C API（Python、Node.js など）
- スレッドセーフな接続管理
- 静的ライブラリと共有ライブラリの両方のビルド

## ビルド

ライブラリは nvecd と一緒に自動的にビルドされます：

```bash
make
```

以下が生成されます：
- `build/lib/libnvecdclient.a` - 静的ライブラリ
- `build/lib/libnvecdclient.dylib`（macOS）または `libnvecdclient.so`（Linux）- 共有ライブラリ

## インストール

```bash
sudo make install
```

以下がインストールされます：
- ヘッダーファイル → `/usr/local/include/nvecd/`
- ライブラリ → `/usr/local/lib/`

## C++ API

### 基本的な使い方

```cpp
#include <nvecdclient.h>
#include <iostream>

using namespace nvecd::client;

int main() {
    // クライアント設定
    ClientConfig config;
    config.host = "localhost";
    config.port = 11017;
    config.timeout_ms = 5000;

    // クライアント作成
    NvecdClient client(config);

    // 接続
    if (auto err = client.Connect()) {
        std::cerr << "接続失敗: " << err->message << std::endl;
        return 1;
    }

    // ベクトル登録
    std::vector<float> vec1 = {0.1f, 0.2f, 0.3f, 0.4f};
    if (auto err = client.Vecset("item123", vec1)) {
        std::cerr << "Vecset 失敗: " << err->message << std::endl;
        return 1;
    }

    // 類似アイテム検索
    auto result = client.Sim("item123", 10, "vectors");
    if (result) {
        std::cout << result->results.size() << " 件の類似アイテムが見つかりました:\n";
        for (const auto& item : result->results) {
            std::cout << "  " << item.id << ": " << item.score << "\n";
        }
    }

    return 0;
}
```

### libnvecdclient でコンパイル

```bash
g++ -std=c++17 -o myapp myapp.cpp -lnvecdclient
```

### イベントトラッキング

```cpp
// ユーザー行動を追跡
client.Event("user_alice", "product123", 100);  // スコア: 0-100
client.Event("user_alice", "product456", 80);
client.Event("user_bob", "product123", 100);

// 共起に基づく推薦を取得
auto result = client.Sim("product123", 10, "events");
// → ユーザーが一緒に操作することが多い商品を返す
```

### ハイブリッド推薦（Fusion）

```cpp
// コンテンツ類似性のためのベクトル登録
std::vector<float> product1_embedding = {...};  // ML モデルから
client.Vecset("product123", product1_embedding);

// ユーザー行動を追跡
client.Event("user_alice", "product123", 100);
client.Event("user_alice", "product456", 80);

// Fusion 検索: コンテンツ + 行動
auto result = client.Sim("product123", 10, "fusion");
// → 類似しており、かつ product123 と共起する商品を返す
```

### ベクトルクエリ検索

```cpp
// 生のベクトルで検索（例: クエリ埋め込み）
std::vector<float> query_vector = {0.5f, 0.3f, 0.2f, 0.1f};
auto result = client.Simv(query_vector, 10, "vectors");

if (result) {
    for (const auto& item : result->results) {
        std::cout << item.id << ": " << item.score << "\n";
    }
}
```

### サーバー情報

```cpp
auto info = client.Info();
if (info) {
    std::cout << "バージョン: " << info->version << "\n";
    std::cout << "稼働時間: " << info->uptime_seconds << "秒\n";
    std::cout << "総リクエスト数: " << info->total_requests << "\n";
}
```

### スナップショット管理

```cpp
// スナップショット保存
if (auto err = client.Save("/backup/snapshot.dmp")) {
    std::cerr << "保存失敗: " << err->message << std::endl;
}

// スナップショット読み込み
if (auto err = client.Load("/backup/snapshot.dmp")) {
    std::cerr << "読み込み失敗: " << err->message << std::endl;
}
```

### デバッグモード

```cpp
// デバッグモード有効化（クエリタイミングを表示）
client.EnableDebug();

auto result = client.Sim("item123", 10, "vectors");
// サーバーログに詳細なタイミング情報が表示される

// デバッグモード無効化
client.DisableDebug();
```

## C API

### 基本的な使い方

```c
#include <nvecdclient_c.h>
#include <stdio.h>

int main() {
    // クライアント設定
    NvecdClientConfig_C config = {
        .host = "localhost",
        .port = 11017,
        .timeout_ms = 5000,
        .recv_buffer_size = 65536
    };

    // クライアント作成
    NvecdClient_C* client = nvecdclient_create(&config);
    if (!client) {
        fprintf(stderr, "クライアント作成失敗\n");
        return 1;
    }

    // 接続
    if (nvecdclient_connect(client) != 0) {
        fprintf(stderr, "接続失敗: %s\n",
                nvecdclient_get_last_error());
        nvecdclient_destroy(client);
        return 1;
    }

    // ベクトル登録
    float vector[] = {0.1f, 0.2f, 0.3f, 0.4f};
    if (nvecdclient_vecset(client, "item123", vector, 4) != 0) {
        fprintf(stderr, "Vecset 失敗: %s\n",
                nvecdclient_get_last_error());
    }

    // 検索
    NvecdSimResponse_C* result = NULL;
    if (nvecdclient_sim(client, "item123", 10, "vectors", &result) == 0) {
        printf("%zu 件の結果が見つかりました:\n", result->count);
        for (size_t i = 0; i < result->count; i++) {
            printf("  %s: %f\n", result->ids[i], result->scores[i]);
        }
        nvecdclient_free_sim_response(result);
    }

    // クリーンアップ
    nvecdclient_disconnect(client);
    nvecdclient_destroy(client);

    return 0;
}
```

### C プログラムのコンパイル

```bash
gcc -o myapp myapp.c -lnvecdclient
```

### イベントトラッキング（C API）

```c
// イベント追跡
nvecdclient_event(client, "user_alice", "product123", 100);
nvecdclient_event(client, "user_alice", "product456", 80);

// 共起による検索
NvecdSimResponse_C* result = NULL;
nvecdclient_sim(client, "product123", 10, "events", &result);
// 結果を処理...
nvecdclient_free_sim_response(result);
```

### ベクトルクエリ（C API）

```c
float query_vector[] = {0.5f, 0.3f, 0.2f, 0.1f};
NvecdSimResponse_C* result = NULL;

if (nvecdclient_simv(client, query_vector, 4, 10, "vectors", &result) == 0) {
    for (size_t i = 0; i < result->count; i++) {
        printf("%s: %f\n", result->ids[i], result->scores[i]);
    }
    nvecdclient_free_sim_response(result);
}
```

## Python バインディング例

C API を使った ctypes の例：

```python
import ctypes
import os

# ライブラリ読み込み
lib = ctypes.CDLL('/usr/local/lib/libnvecdclient.dylib')  # macOS
# lib = ctypes.CDLL('/usr/local/lib/libnvecdclient.so')   # Linux

# 設定構造体を定義
class NvecdClientConfig(ctypes.Structure):
    _fields_ = [
        ("host", ctypes.c_char_p),
        ("port", ctypes.c_int),
        ("timeout_ms", ctypes.c_int),
        ("recv_buffer_size", ctypes.c_int)
    ]

# クライアント作成
config = NvecdClientConfig(
    host=b"localhost",
    port=11017,
    timeout_ms=5000,
    recv_buffer_size=65536
)

lib.nvecdclient_create.restype = ctypes.c_void_p
client = lib.nvecdclient_create(ctypes.byref(config))

# 接続
if lib.nvecdclient_connect(client) != 0:
    lib.nvecdclient_get_last_error.restype = ctypes.c_char_p
    error = lib.nvecdclient_get_last_error()
    print(f"接続失敗: {error.decode()}")
    exit(1)

# ベクトル登録
vector = (ctypes.c_float * 4)(0.1, 0.2, 0.3, 0.4)
lib.nvecdclient_vecset(client, b"item123", vector, 4)

# クリーンアップ
lib.nvecdclient_disconnect(client)
lib.nvecdclient_destroy(client)
```

本番環境では、適切な Python ラッパークラスの作成を検討してください。

## Node.js バインディング例

C API を使った node-gyp の例：

```javascript
// binding.gyp
{
  "targets": [{
    "target_name": "nvecd",
    "sources": [ "src/nvecd_node.cpp" ],
    "include_dirs": [
      "/usr/local/include",
      "<!(node -p \"require('node-addon-api').include_dir\")"
    ],
    "libraries": [
      "-L/usr/local/lib",
      "-lnvecdclient"
    ],
    "cflags!": [ "-fno-exceptions" ],
    "cflags_cc!": [ "-fno-exceptions" ]
  }]
}
```

```cpp
// src/nvecd_node.cpp（簡略化された例）
#include <napi.h>
#include <nvecdclient_c.h>

Napi::Value Search(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    // パラメータ取得
    std::string id = info[0].As<Napi::String>();
    int top_k = info[1].As<Napi::Number>().Int32Value();

    // クライアント作成と接続
    NvecdClientConfig_C config = {
        .host = "localhost",
        .port = 11017,
        .timeout_ms = 5000,
        .recv_buffer_size = 65536
    };

    NvecdClient_C* client = nvecdclient_create(&config);
    nvecdclient_connect(client);

    // 検索
    NvecdSimResponse_C* result = NULL;
    nvecdclient_sim(client, id.c_str(), top_k, "vectors", &result);

    // JavaScript 配列に変換
    Napi::Array jsResults = Napi::Array::New(env, result->count);
    for (size_t i = 0; i < result->count; i++) {
        Napi::Object obj = Napi::Object::New(env);
        obj.Set("id", Napi::String::New(env, result->ids[i]));
        obj.Set("score", Napi::Number::New(env, result->scores[i]));
        jsResults[i] = obj;
    }

    // クリーンアップ
    nvecdclient_free_sim_response(result);
    nvecdclient_disconnect(client);
    nvecdclient_destroy(client);

    return jsResults;
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
    exports.Set("search", Napi::Function::New(env, Search));
    return exports;
}

NODE_API_MODULE(nvecd, Init)
```

使用例:
```javascript
const nvecd = require('./build/Release/nvecd');

const results = nvecd.search('item123', 10);
console.log(results);
// → [{ id: 'item456', score: 0.95 }, ...]
```

## API リファレンス

### C++ API クラス

#### ClientConfig
- `host` - サーバーホスト名（デフォルト: "127.0.0.1"）
- `port` - サーバーポート（デフォルト: 11017）
- `timeout_ms` - 接続タイムアウト（デフォルト: 5000）
- `recv_buffer_size` - 受信バッファサイズ（デフォルト: 65536）

#### SimResponse
- `results` - SimilarityResult のベクター（id, score のペア）

#### ServerInfo
- `version` - サーバーバージョン文字列
- `uptime_seconds` - サーバー稼働時間（秒）
- `total_requests` - 処理されたリクエスト総数

#### Error
- `message` - エラーメッセージ文字列

### C++ API メソッド

#### 接続管理
- `Expected<void, Error> Connect()` - サーバーに接続
- `void Disconnect()` - サーバーから切断
- `bool IsConnected()` - 接続状態を確認

#### nvecd コマンド
- `Expected<void, Error> Event(ctx, id, score)` - イベント追跡
- `Expected<void, Error> Vecset(id, vector)` - ベクトル登録
- `Expected<SimResponse, Error> Sim(id, top_k, mode)` - ID で検索
- `Expected<SimResponse, Error> Simv(vector, top_k, mode)` - ベクトルで検索

#### 管理コマンド
- `Expected<ServerInfo, Error> Info()` - サーバー情報取得
- `Expected<std::string, Error> GetConfig()` - 設定取得
- `Expected<std::string, Error> Save(filepath)` - スナップショット保存
- `Expected<std::string, Error> Load(filepath)` - スナップショット読み込み
- `Expected<void, Error> EnableDebug()` - デバッグモード有効化
- `Expected<void, Error> DisableDebug()` - デバッグモード無効化

### C API 関数

詳細は `nvecdclient_c.h` を参照してください。

主要な関数：
- `nvecdclient_create()` - クライアント作成
- `nvecdclient_connect()` - サーバーに接続
- `nvecdclient_event()` - イベント追跡
- `nvecdclient_vecset()` - ベクトル登録
- `nvecdclient_sim()` - ID で検索
- `nvecdclient_simv()` - ベクトルで検索
- `nvecdclient_info()` - サーバー情報取得
- `nvecdclient_free_*()` - 結果構造体を解放

## スレッドセーフティ

NvecdClient クラスは単一の TCP 接続を管理しており、**スレッドセーフではありません**。マルチスレッドアプリケーションでは、スレッドごとにクライアントインスタンスを作成するか、適切な同期を使用してください。

## エラーハンドリング

### C++ API

関数は `Expected<T, Error>` を返します：
- 成功: 結果値を含む
- 失敗: メッセージ付き Error を含む

`operator bool()` を使用してエラーをチェック：

```cpp
auto result = client.Sim("item123", 10, "vectors");
if (!result) {
    // エラー: result.error().message
    std::cerr << "エラー: " << result.error().message << "\n";
} else {
    // 成功: result.value()
    for (const auto& item : result->results) {
        std::cout << item.id << ": " << item.score << "\n";
    }
}
```

### C API

関数は成功時に 0、エラー時に -1 を返します。エラーメッセージを取得するには `nvecdclient_get_last_error()` を使用します。

```c
if (nvecdclient_sim(client, "item123", 10, "vectors", &result) != 0) {
    fprintf(stderr, "エラー: %s\n", nvecdclient_get_last_error());
}
```

## ベストプラクティス

### 接続の再利用

✅ **良い例:** 複数クエリで接続を再利用
```cpp
NvecdClient client(config);
client.Connect();

for (const auto& query : queries) {
    auto result = client.Sim(query.id, 10, "vectors");
    // 結果を処理...
}
```

❌ **悪い例:** クエリごとに再接続
```cpp
for (const auto& query : queries) {
    NvecdClient client(config);
    client.Connect();          // 高コスト!
    auto result = client.Sim(query.id, 10, "vectors");
    client.Disconnect();
}
```

### エラーハンドリング

✅ **良い例:** すべてのエラーをチェック
```cpp
if (auto err = client.Connect()) {
    std::cerr << "接続失敗: " << err->message << "\n";
    return 1;
}

auto result = client.Sim("item123", 10, "vectors");
if (!result) {
    std::cerr << "検索失敗: " << result.error().message << "\n";
    return 1;
}
```

❌ **悪い例:** エラーを無視
```cpp
client.Connect();  // 失敗するかも!
auto result = client.Sim("item123", 10, "vectors");
// チェックせずに結果を使用...
```

### バッチ操作

✅ **良い例:** ベクトルアップロードをバッチ処理
```cpp
client.Connect();
for (const auto& [id, vector] : vectors) {
    client.Vecset(id, vector);  // 接続を再利用
}
```

❌ **悪い例:** ベクトルごとに接続
```cpp
for (const auto& [id, vector] : vectors) {
    NvecdClient client(config);
    client.Connect();
    client.Vecset(id, vector);
    client.Disconnect();  // 高コスト!
}
```

## 完全な例

```cpp
#include <nvecdclient.h>
#include <iostream>
#include <vector>

using namespace nvecd::client;

int main() {
    // クライアント設定
    ClientConfig config;
    config.host = "localhost";
    config.port = 11017;
    config.timeout_ms = 5000;

    // クライアント作成
    NvecdClient client(config);

    // 接続
    if (auto err = client.Connect()) {
        std::cerr << "接続失敗: " << err->message << "\n";
        return 1;
    }

    std::cout << "nvecd サーバーに接続しました\n";

    // 商品ベクトル登録（ML モデルから）
    std::vector<std::pair<std::string, std::vector<float>>> products = {
        {"laptop_001", {0.1f, 0.2f, 0.3f, 0.4f}},
        {"laptop_002", {0.15f, 0.25f, 0.28f, 0.38f}},
        {"phone_001", {0.8f, 0.7f, 0.6f, 0.5f}}
    };

    for (const auto& [id, vector] : products) {
        if (auto err = client.Vecset(id, vector)) {
            std::cerr << id << " の Vecset 失敗: "
                      << err->message << "\n";
        } else {
            std::cout << "登録完了: " << id << "\n";
        }
    }

    // ユーザー行動を追跡
    client.Event("user_alice", "laptop_001", 100);  // 購入
    client.Event("user_alice", "laptop_002", 80);   // 閲覧
    client.Event("user_bob", "laptop_001", 100);    // 購入
    client.Event("user_bob", "phone_001", 90);      // 閲覧

    // コンテンツベース推薦
    std::cout << "\nlaptop_001 のコンテンツベース推薦:\n";
    auto content_result = client.Sim("laptop_001", 5, "vectors");
    if (content_result) {
        for (const auto& item : content_result->results) {
            std::cout << "  " << item.id << ": " << item.score << "\n";
        }
    }

    // 行動ベース推薦
    std::cout << "\nlaptop_001 の行動ベース推薦:\n";
    auto behavior_result = client.Sim("laptop_001", 5, "events");
    if (behavior_result) {
        for (const auto& item : behavior_result->results) {
            std::cout << "  " << item.id << ": " << item.score << "\n";
        }
    }

    // ハイブリッド推薦（fusion）
    std::cout << "\nlaptop_001 のハイブリッド推薦:\n";
    auto fusion_result = client.Sim("laptop_001", 5, "fusion");
    if (fusion_result) {
        for (const auto& item : fusion_result->results) {
            std::cout << "  " << item.id << ": " << item.score << "\n";
        }
    }

    // サーバー情報取得
    auto info = client.Info();
    if (info) {
        std::cout << "\nサーバー情報:\n";
        std::cout << "  バージョン: " << info->version << "\n";
        std::cout << "  稼働時間: " << info->uptime_seconds << "秒\n";
        std::cout << "  総リクエスト数: " << info->total_requests << "\n";
    }

    client.Disconnect();
    return 0;
}
```

コンパイルと実行:
```bash
g++ -std=c++17 -o example example.cpp -lnvecdclient
./example
```

## ライセンス

MIT ライセンス（LICENSE ファイルを参照）
