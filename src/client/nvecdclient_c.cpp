/**
 * @file nvecdclient_c.cpp
 * @brief C API wrapper implementation
 *
 * Reference: ../mygram-db/src/client/mygramclient_c.cpp
 * Reusability: 85% (memory management, helper functions, error handling)
 * Adapted for: nvecd-specific C API (EVENT, VECSET, SIM, SIMV)
 *
 * This file implements a C API wrapper which requires manual memory management
 * and uses C naming conventions. All related warnings are suppressed for the entire file.
 */

// NOLINTBEGIN(readability-identifier-naming, cppcoreguidelines-owning-memory,
// cppcoreguidelines-no-malloc, readability-implicit-bool-conversion)

#include "client/nvecdclient_c.h"

#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "client/nvecdclient.h"

using namespace nvecd::client;

// Opaque handle structure
struct NvecdClient_C {
  std::unique_ptr<NvecdClient> client;
  std::string last_error;
};

// Helper: Allocate C string copy
static char* strdup_safe(const std::string& str) {
  char* result = static_cast<char*>(malloc(str.size() + 1));
  if (result != nullptr) {
    std::memcpy(result, str.c_str(), str.size() + 1);
  }
  return result;
}

// Transfer an owned string to a C out-parameter. Do not report success with a
// null output when allocation fails: callers otherwise cannot distinguish OOM
// from a valid empty response.
static int copy_string_result(const std::string& value, char** out, std::string& last_error) {
  char* copy = strdup_safe(value);
  if (copy == nullptr) {
    last_error = "Memory allocation failed";
    return -1;
  }
  *out = copy;
  return 0;
}

// Helper: Translate C search options to the C++ SearchOptions struct.
static SearchOptions to_cpp_options(const NvecdSearchOptions_C* options) {
  SearchOptions cpp_options;
  if (options == nullptr) {
    return cpp_options;
  }
  if (options->filter != nullptr) {
    cpp_options.filter = options->filter;
  }
  if (options->has_min_score != 0) {
    cpp_options.min_score = options->min_score;
  }
  if (options->has_adaptive != 0) {
    cpp_options.adaptive = (options->adaptive != 0);
  }
  return cpp_options;
}

// Helper: Marshal a C++ SimResponse into a freshly allocated C result struct.
// On success *out is set and 0 is returned; on allocation failure last_error is
// set, *out is left untouched, and -1 is returned. Ownership of the returned
// struct transfers to the caller (free via nvecdclient_free_sim_response).
static int build_sim_response(const SimResponse& src, NvecdSimResponse_C** out, std::string& last_error) {
  auto* c_result = static_cast<NvecdSimResponse_C*>(malloc(sizeof(NvecdSimResponse_C)));
  if (c_result == nullptr) {
    last_error = "Memory allocation failed";
    return -1;
  }

  c_result->count = src.results.size();
  c_result->mode = strdup_safe(src.mode);
  if (c_result->mode == nullptr) {
    free(c_result);
    last_error = "Memory allocation failed";
    return -1;
  }

  if (c_result->count > 0) {
    c_result->results = static_cast<NvecdSimResultItem_C*>(malloc(sizeof(NvecdSimResultItem_C) * c_result->count));
    if (c_result->results == nullptr) {
      free(c_result->mode);
      free(c_result);
      last_error = "Memory allocation failed";
      return -1;
    }

    for (size_t i = 0; i < c_result->count; ++i) {
      c_result->results[i].id = strdup_safe(src.results[i].id);
      if (c_result->results[i].id == nullptr) {
        for (size_t j = 0; j < i; ++j) {
          free(c_result->results[j].id);
        }
        free(c_result->results);
        free(c_result->mode);
        free(c_result);
        last_error = "Memory allocation failed";
        return -1;
      }
      c_result->results[i].score = src.results[i].score;
    }
  } else {
    c_result->results = nullptr;
  }

  *out = c_result;
  return 0;
}

NvecdClient_C* nvecdclient_create(const NvecdClientConfig_C* config) {
  if (config == nullptr) {
    return nullptr;
  }

  auto* client_c = new (std::nothrow) NvecdClient_C();
  if (client_c == nullptr) {
    return nullptr;
  }

  ClientConfig cpp_config;
  cpp_config.host = (config->host != nullptr) ? config->host : "127.0.0.1";
  cpp_config.port = config->port != 0 ? config->port : 11017;                                      // NOLINT
  cpp_config.timeout_ms = config->timeout_ms != 0 ? config->timeout_ms : 5000;                     // NOLINT
  cpp_config.recv_buffer_size = config->recv_buffer_size != 0 ? config->recv_buffer_size : 65536;  // NOLINT

  client_c->client = std::make_unique<NvecdClient>(cpp_config);

  return client_c;
}

void nvecdclient_destroy(NvecdClient_C* client) {
  delete client;
}

int nvecdclient_connect(NvecdClient_C* client) {
  if (client == nullptr || client->client == nullptr) {
    return -1;
  }

  auto result = client->client->Connect();
  if (!result) {
    client->last_error = result.error().to_string();
    return -1;
  }

  return 0;
}

void nvecdclient_disconnect(NvecdClient_C* client) {
  if (client != nullptr && client->client != nullptr) {
    client->client->Disconnect();
  }
}

int nvecdclient_is_connected(const NvecdClient_C* client) {
  if (client == nullptr || client->client == nullptr) {
    return 0;
  }

  return client->client->IsConnected() ? 1 : 0;
}

//
// nvecd-specific commands
//

int nvecdclient_event(NvecdClient_C* client, const char* ctx, const char* type, const char* id, int score) {
  if (client == nullptr || client->client == nullptr || ctx == nullptr || type == nullptr || id == nullptr) {
    return -1;
  }

  auto result = client->client->Event(ctx, type, id, score);
  if (!result) {
    client->last_error = result.error().to_string();
    return -1;
  }

  return 0;
}

int nvecdclient_vecset(NvecdClient_C* client, const char* id, const float* vector, size_t dimension) {
  if (client == nullptr || client->client == nullptr || id == nullptr || vector == nullptr || dimension == 0) {
    return -1;
  }

  std::vector<float> vec(vector, vector + dimension);
  auto result = client->client->Vecset(id, vec);
  if (!result) {
    client->last_error = result.error().to_string();
    return -1;
  }

  return 0;
}

int nvecdclient_vecdel(NvecdClient_C* client, const char* id) {
  if (client == nullptr || client->client == nullptr || id == nullptr) {
    return -1;
  }

  auto result = client->client->Vecdel(id);
  if (!result) {
    client->last_error = result.error().to_string();
    return -1;
  }
  return 0;
}

int nvecdclient_metaset(NvecdClient_C* client, const char* id, const char* metadata) {
  if (client == nullptr || client->client == nullptr || id == nullptr || metadata == nullptr) {
    return -1;
  }

  auto result = client->client->Metaset(id, metadata);
  if (!result) {
    client->last_error = result.error().to_string();
    return -1;
  }

  return 0;
}

int nvecdclient_sim(NvecdClient_C* client, const char* id, uint32_t top_k, const char* mode,
                    NvecdSimResponse_C** result) {
  return nvecdclient_sim_ex(client, id, top_k, mode, nullptr, result);
}

int nvecdclient_sim_ex(NvecdClient_C* client, const char* id, uint32_t top_k, const char* mode,
                       const NvecdSearchOptions_C* options, NvecdSimResponse_C** result) {
  if (client == nullptr || client->client == nullptr || id == nullptr || result == nullptr) {
    return -1;
  }

  std::string mode_str = (mode != nullptr) ? mode : "fusion";
  auto cpp_result = client->client->Sim(id, top_k, mode_str, to_cpp_options(options));
  if (!cpp_result) {
    client->last_error = cpp_result.error().to_string();
    return -1;
  }

  return build_sim_response(*cpp_result, result, client->last_error);
}

int nvecdclient_simv(NvecdClient_C* client, const float* vector, size_t dimension, uint32_t top_k, const char* mode,
                     NvecdSimResponse_C** result) {
  return nvecdclient_simv_ex(client, vector, dimension, top_k, mode, nullptr, result);
}

int nvecdclient_simv_ex(NvecdClient_C* client, const float* vector, size_t dimension, uint32_t top_k, const char* mode,
                        const NvecdSearchOptions_C* options, NvecdSimResponse_C** result) {
  if (client == nullptr || client->client == nullptr || vector == nullptr || dimension == 0 || result == nullptr) {
    return -1;
  }

  std::vector<float> vec(vector, vector + dimension);
  std::string mode_str = (mode != nullptr) ? mode : "vectors";
  auto cpp_result = client->client->Simv(vec, top_k, mode_str, to_cpp_options(options));
  if (!cpp_result) {
    client->last_error = cpp_result.error().to_string();
    return -1;
  }

  return build_sim_response(*cpp_result, result, client->last_error);
}

//
// MygramDB-compatible commands
//

int nvecdclient_auth(NvecdClient_C* client, const char* password) {
  if (client == nullptr || client->client == nullptr || password == nullptr) {
    return -1;
  }

  auto result = client->client->Auth(password);
  if (!result) {
    client->last_error = result.error().to_string();
    return -1;
  }

  return 0;
}

int nvecdclient_info(NvecdClient_C* client, NvecdServerInfo_C** info) {
  if (client == nullptr || client->client == nullptr || info == nullptr) {
    return -1;
  }

  auto cpp_result = client->client->Info();
  if (!cpp_result) {
    client->last_error = cpp_result.error().to_string();
    return -1;
  }

  // Allocate C result structure
  auto* c_info = static_cast<NvecdServerInfo_C*>(malloc(sizeof(NvecdServerInfo_C)));
  if (c_info == nullptr) {
    client->last_error = "Memory allocation failed";
    return -1;
  }

  c_info->version = strdup_safe(cpp_result->version);
  if (c_info->version == nullptr) {
    free(c_info);
    client->last_error = "Memory allocation failed";
    return -1;
  }
  c_info->uptime_seconds = cpp_result->uptime_seconds;
  c_info->total_commands_processed = cpp_result->total_commands_processed;
  c_info->failed_commands = cpp_result->failed_commands;
  c_info->total_connections = cpp_result->total_connections;
  c_info->active_connections = cpp_result->active_connections;
  c_info->event_count = cpp_result->event_count;
  c_info->vector_count = cpp_result->vector_count;
  c_info->id_count = cpp_result->id_count;
  c_info->ctx_count = cpp_result->ctx_count;

  *info = c_info;
  return 0;
}

int nvecdclient_get_config(NvecdClient_C* client, char** config_str) {
  if (client == nullptr || client->client == nullptr || config_str == nullptr) {
    return -1;
  }

  auto result = client->client->GetConfig();
  if (!result) {
    client->last_error = result.error().to_string();
    return -1;
  }

  return copy_string_result(*result, config_str, client->last_error);
}

int nvecdclient_save(NvecdClient_C* client, const char* filepath, char** saved_path) {
  if (client == nullptr || client->client == nullptr || saved_path == nullptr) {
    return -1;
  }

  std::string filepath_str = (filepath != nullptr) ? filepath : "";
  auto result = client->client->Save(filepath_str);
  if (!result) {
    client->last_error = result.error().to_string();
    return -1;
  }

  return copy_string_result(*result, saved_path, client->last_error);
}

int nvecdclient_load(NvecdClient_C* client, const char* filepath, char** loaded_path) {
  if (client == nullptr || client->client == nullptr || filepath == nullptr || loaded_path == nullptr) {
    return -1;
  }

  auto result = client->client->Load(filepath);
  if (!result) {
    client->last_error = result.error().to_string();
    return -1;
  }

  return copy_string_result(*result, loaded_path, client->last_error);
}

int nvecdclient_verify(NvecdClient_C* client, const char* filepath, char** result_str) {
  if (client == nullptr || client->client == nullptr || filepath == nullptr || result_str == nullptr) {
    return -1;
  }

  auto result = client->client->Verify(filepath);
  if (!result) {
    client->last_error = result.error().to_string();
    return -1;
  }

  return copy_string_result(*result, result_str, client->last_error);
}

int nvecdclient_dump_info(NvecdClient_C* client, const char* filepath, char** info_str) {
  if (client == nullptr || client->client == nullptr || filepath == nullptr || info_str == nullptr) {
    return -1;
  }

  auto result = client->client->DumpInfo(filepath);
  if (!result) {
    client->last_error = result.error().to_string();
    return -1;
  }

  return copy_string_result(*result, info_str, client->last_error);
}

int nvecdclient_dump_status(NvecdClient_C* client, char** status_str) {
  if (client == nullptr || client->client == nullptr || status_str == nullptr) {
    return -1;
  }

  auto result = client->client->DumpStatus();
  if (!result) {
    client->last_error = result.error().to_string();
    return -1;
  }

  return copy_string_result(*result, status_str, client->last_error);
}

int nvecdclient_cache_stats(NvecdClient_C* client, char** stats_str) {
  if (client == nullptr || client->client == nullptr || stats_str == nullptr) {
    return -1;
  }

  auto result = client->client->CacheStats();
  if (!result) {
    client->last_error = result.error().to_string();
    return -1;
  }

  return copy_string_result(*result, stats_str, client->last_error);
}

int nvecdclient_cache_clear(NvecdClient_C* client) {
  if (client == nullptr || client->client == nullptr) {
    return -1;
  }

  auto result = client->client->CacheClear();
  if (!result) {
    client->last_error = result.error().to_string();
    return -1;
  }

  return 0;
}

int nvecdclient_cache_enable(NvecdClient_C* client) {
  if (client == nullptr || client->client == nullptr) {
    return -1;
  }

  auto result = client->client->CacheEnable();
  if (!result) {
    client->last_error = result.error().to_string();
    return -1;
  }

  return 0;
}

int nvecdclient_cache_disable(NvecdClient_C* client) {
  if (client == nullptr || client->client == nullptr) {
    return -1;
  }

  auto result = client->client->CacheDisable();
  if (!result) {
    client->last_error = result.error().to_string();
    return -1;
  }

  return 0;
}

int nvecdclient_debug_on(NvecdClient_C* client) {
  if (client == nullptr || client->client == nullptr) {
    return -1;
  }

  auto result = client->client->EnableDebug();
  if (!result) {
    client->last_error = result.error().to_string();
    return -1;
  }

  return 0;
}

int nvecdclient_debug_off(NvecdClient_C* client) {
  if (client == nullptr || client->client == nullptr) {
    return -1;
  }

  auto result = client->client->DisableDebug();
  if (!result) {
    client->last_error = result.error().to_string();
    return -1;
  }

  return 0;
}

const char* nvecdclient_get_last_error(const NvecdClient_C* client) {
  if (client == nullptr) {
    return "Invalid client handle";
  }
  return client->last_error.c_str();
}

//
// Memory management functions
//

void nvecdclient_free_sim_response(NvecdSimResponse_C* result) {
  if (result == nullptr) {
    return;
  }

  if (result->results != nullptr) {
    for (size_t i = 0; i < result->count; ++i) {
      free(result->results[i].id);
    }
    free(result->results);
  }

  free(result->mode);
  free(result);
}

void nvecdclient_free_server_info(NvecdServerInfo_C* info) {
  if (info == nullptr) {
    return;
  }

  free(info->version);
  free(info);
}

void nvecdclient_free_string(char* str) {
  free(str);
}

// NOLINTEND(readability-identifier-naming, cppcoreguidelines-owning-memory,
// cppcoreguidelines-no-malloc, readability-implicit-bool-conversion)
