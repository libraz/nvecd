/**
 * @file nvecdclient_c.h
 * @brief C API wrapper for nvecd client library
 *
 * Reference: ../mygram-db/src/client/mygramclient_c.h
 * Reusability: 85% (client creation, connection management, common patterns)
 * Adapted for: nvecd-specific commands (EVENT, VECSET, SIM, SIMV)
 *
 * This header provides a C-compatible interface for the nvecd client library,
 * suitable for use with FFI bindings (node-gyp, ctypes, cffi, etc.).
 *
 * All functions return 0 on success, non-zero on error.
 * Use nvecdclient_get_last_error() to retrieve error messages.
 *
 * Note: This is a C API header, so typedef is used instead of using declarations
 * for C compatibility. The modernize-use-using check is disabled for this file.
 */

#pragma once

// NOLINTBEGIN(modernize-use-using) - C API requires typedef for C compatibility

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Opaque handle to nvecd client
 */
typedef struct NvecdClient_C NvecdClient_C;

/**
 * @brief Client configuration
 */
typedef struct {
  const char* host;           ///< Server hostname (default: "127.0.0.1")
  uint16_t port;              ///< Server port (default: 11017)
  uint32_t timeout_ms;        ///< Connection timeout in milliseconds (default: 5000)
  uint32_t recv_buffer_size;  ///< Receive buffer size (default: 65536)
} NvecdClientConfig_C;

/**
 * @brief Similarity search result item
 */
typedef struct {
  char* id;     ///< Document/vector ID
  float score;  ///< Similarity score
} NvecdSimResultItem_C;

/**
 * @brief Similarity search response
 */
typedef struct {
  NvecdSimResultItem_C* results;  ///< Array of result items
  size_t count;                   ///< Number of results
  char* mode;                     ///< Search mode used (events/vectors/fusion)
} NvecdSimResponse_C;

/**
 * @brief Server information
 */
typedef struct {
  char* version;
  uint64_t uptime_seconds;
  uint64_t total_requests;
  uint64_t active_connections;
  uint64_t event_count;
  uint64_t vector_count;
  uint64_t co_occurrence_entries;
} NvecdServerInfo_C;

/**
 * @brief Create a new nvecd client
 *
 * @param config Client configuration
 * @return Client handle, or NULL on error
 */
NvecdClient_C* nvecdclient_create(const NvecdClientConfig_C* config);

/**
 * @brief Destroy a nvecd client and free resources
 *
 * @param client Client handle
 */
void nvecdclient_destroy(NvecdClient_C* client);

/**
 * @brief Connect to nvecd server
 *
 * @param client Client handle
 * @return 0 on success, -1 on error
 */
int nvecdclient_connect(NvecdClient_C* client);

/**
 * @brief Disconnect from server
 *
 * @param client Client handle
 */
void nvecdclient_disconnect(NvecdClient_C* client);

/**
 * @brief Check if connected to server
 *
 * @param client Client handle
 * @return 1 if connected, 0 otherwise
 */
int nvecdclient_is_connected(const NvecdClient_C* client);

//
// nvecd-specific commands
//

/**
 * @brief Register event (EVENT command)
 *
 * @param client Client handle
 * @param ctx Context ID
 * @param id Document/vector ID
 * @param score Event score (0-100)
 * @return 0 on success, -1 on error
 */
int nvecdclient_event(NvecdClient_C* client, const char* ctx, const char* id, int score);

/**
 * @brief Register vector (VECSET command)
 *
 * @param client Client handle
 * @param id Vector ID
 * @param vector Vector data array
 * @param dimension Vector dimension
 * @return 0 on success, -1 on error
 */
int nvecdclient_vecset(NvecdClient_C* client, const char* id, const float* vector, size_t dimension);

/**
 * @brief Similarity search by ID (SIM command)
 *
 * @param client Client handle
 * @param id Document/vector ID
 * @param top_k Number of results to return
 * @param mode Search mode ("events", "vectors", or "fusion"; NULL for default "fusion")
 * @param result Output search results (caller must free with nvecdclient_free_sim_response)
 * @return 0 on success, -1 on error
 */
int nvecdclient_sim(NvecdClient_C* client, const char* id, uint32_t top_k, const char* mode,
                    NvecdSimResponse_C** result);

/**
 * @brief Similarity search by vector (SIMV command)
 *
 * @param client Client handle
 * @param vector Query vector array
 * @param dimension Vector dimension
 * @param top_k Number of results to return
 * @param mode Search mode ("vectors" only; NULL for default "vectors")
 * @param result Output search results (caller must free with nvecdclient_free_sim_response)
 * @return 0 on success, -1 on error
 */
int nvecdclient_simv(NvecdClient_C* client, const float* vector, size_t dimension, uint32_t top_k, const char* mode,
                     NvecdSimResponse_C** result);

//
// MygramDB-compatible commands
//

/**
 * @brief Get server information (INFO command)
 *
 * @param client Client handle
 * @param info Output server info (caller must free with nvecdclient_free_server_info)
 * @return 0 on success, -1 on error
 */
int nvecdclient_info(NvecdClient_C* client, NvecdServerInfo_C** info);

/**
 * @brief Get server configuration (CONFIG SHOW command)
 *
 * @param client Client handle
 * @param config_str Output configuration string (caller must free with nvecdclient_free_string)
 * @return 0 on success, -1 on error
 */
int nvecdclient_get_config(NvecdClient_C* client, char** config_str);

/**
 * @brief Save snapshot to disk (DUMP SAVE command)
 *
 * @param client Client handle
 * @param filepath Optional filepath (NULL for default)
 * @param saved_path Output saved filepath (caller must free with nvecdclient_free_string)
 * @return 0 on success, -1 on error
 */
int nvecdclient_save(NvecdClient_C* client, const char* filepath, char** saved_path);

/**
 * @brief Load snapshot from disk (DUMP LOAD command)
 *
 * @param client Client handle
 * @param filepath Snapshot filepath
 * @param loaded_path Output loaded filepath (caller must free with nvecdclient_free_string)
 * @return 0 on success, -1 on error
 */
int nvecdclient_load(NvecdClient_C* client, const char* filepath, char** loaded_path);

/**
 * @brief Verify snapshot integrity (DUMP VERIFY command)
 *
 * @param client Client handle
 * @param filepath Snapshot filepath
 * @param result_str Output verification result (caller must free with nvecdclient_free_string)
 * @return 0 on success, -1 on error
 */
int nvecdclient_verify(NvecdClient_C* client, const char* filepath, char** result_str);

/**
 * @brief Get snapshot metadata (DUMP INFO command)
 *
 * @param client Client handle
 * @param filepath Snapshot filepath
 * @param info_str Output snapshot metadata (caller must free with nvecdclient_free_string)
 * @return 0 on success, -1 on error
 */
int nvecdclient_dump_info(NvecdClient_C* client, const char* filepath, char** info_str);

/**
 * @brief Enable debug mode (DEBUG ON command)
 *
 * @param client Client handle
 * @return 0 on success, -1 on error
 */
int nvecdclient_debug_on(NvecdClient_C* client);

/**
 * @brief Disable debug mode (DEBUG OFF command)
 *
 * @param client Client handle
 * @return 0 on success, -1 on error
 */
int nvecdclient_debug_off(NvecdClient_C* client);

/**
 * @brief Get last error message
 *
 * @param client Client handle
 * @return Error message string (do not free)
 */
const char* nvecdclient_get_last_error(const NvecdClient_C* client);

/**
 * @brief Free similarity search response
 *
 * @param result Search response to free
 */
void nvecdclient_free_sim_response(NvecdSimResponse_C* result);

/**
 * @brief Free server info
 *
 * @param info Server info to free
 */
void nvecdclient_free_server_info(NvecdServerInfo_C* info);

/**
 * @brief Free string
 *
 * @param str String to free
 */
void nvecdclient_free_string(char* str);

#ifdef __cplusplus
}
#endif

// NOLINTEND(modernize-use-using)
