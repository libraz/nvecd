# nvecd tests
#
# Reference: ../mygram-db/tests/CMakeLists.txt
# Reusability: 90%

include(GoogleTest)

# Include source directory
include_directories(${CMAKE_SOURCE_DIR}/src)

# Utility tests
add_executable(expected_test
  utils/expected_test.cpp
)
target_link_libraries(expected_test
  gtest_main
)
gtest_discover_tests(expected_test)

add_executable(network_utils_test
  utils/network_utils_test.cpp
)
target_link_libraries(network_utils_test
  nvecd_utils
  gtest_main
)
gtest_discover_tests(network_utils_test)

add_executable(path_utils_test
  utils/path_utils_test.cpp
)
target_link_libraries(path_utils_test
  nvecd_utils
  gtest_main
)
gtest_discover_tests(path_utils_test)

add_executable(string_utils_test
  utils/string_utils_test.cpp
)
target_link_libraries(string_utils_test
  nvecd_utils
  gtest_main
)
gtest_discover_tests(string_utils_test)

# Config tests
add_executable(config_test
  config/config_test.cpp
)
target_link_libraries(config_test
  nvecd_config
  gtest_main
)
# Copy test config file to build directory
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/config/test_config.yaml
  ${CMAKE_CURRENT_BINARY_DIR}/test_config.yaml
  COPYONLY
)
gtest_discover_tests(config_test
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

add_executable(runtime_variable_manager_test
  config/runtime_variable_manager_test.cpp
)
target_link_libraries(runtime_variable_manager_test
  nvecd_config
  nvecd_cache
  nvecd_similarity
  nvecd_events
  nvecd_vectors
  nvecd_utils
  gtest
  gtest_main
)
gtest_discover_tests(runtime_variable_manager_test)

# Server tests
add_executable(command_parser_test
  server/command_parser_test.cpp
)
target_link_libraries(command_parser_test
  nvecd_server
  gtest_main
)
gtest_discover_tests(command_parser_test)

add_executable(http_server_test
  server/http_server_test.cpp
)
target_link_libraries(http_server_test
  nvecd_server
  nvecd_cache
  nvecd_similarity
  nvecd_events
  nvecd_vectors
  nvecd_config
  nvecd_utils
  gtest
  gtest_main
)
gtest_discover_tests(http_server_test
  DISCOVERY_TIMEOUT 30
  PROPERTIES
    LABELS "http"
    RESOURCE_LOCK http_port_18081
)

add_executable(thread_pool_test
  server/thread_pool_test.cpp
)
target_link_libraries(thread_pool_test
  nvecd_server
  gtest
  gtest_main
)
gtest_discover_tests(thread_pool_test)

add_executable(server_stats_test
  server/server_stats_test.cpp
)
target_link_libraries(server_stats_test
  nvecd_server
  gtest
  gtest_main
)
gtest_discover_tests(server_stats_test)

add_executable(handler_test
  server/handlers/handler_test.cpp
)
target_link_libraries(handler_test
  nvecd_server
  nvecd_cache
  nvecd_similarity
  nvecd_events
  nvecd_vectors
  nvecd_config
  nvecd_utils
  gtest
  gtest_main
  gmock
)
gtest_discover_tests(handler_test)

add_executable(snapshot_scheduler_test
  server/snapshot_scheduler_test.cpp
)
target_link_libraries(snapshot_scheduler_test
  nvecd_server
  nvecd_storage
  nvecd_events
  nvecd_vectors
  nvecd_config
  nvecd_utils
  gtest
  gtest_main
)
gtest_discover_tests(snapshot_scheduler_test
  PROPERTIES TIMEOUT 30
)

add_executable(rate_limiter_test
  server/rate_limiter_test.cpp
)
target_link_libraries(rate_limiter_test
  nvecd_server
  gtest
  gtest_main
)
gtest_discover_tests(rate_limiter_test)

add_executable(request_dispatcher_test
  server/request_dispatcher_test.cpp
)
target_link_libraries(request_dispatcher_test
  nvecd_server
  nvecd_cache
  nvecd_similarity
  nvecd_events
  nvecd_vectors
  nvecd_config
  nvecd_utils
  gtest
  gtest_main
)
gtest_discover_tests(request_dispatcher_test)

# Events tests
add_executable(ring_buffer_test
  events/ring_buffer_test.cpp
)
target_link_libraries(ring_buffer_test
  gtest_main
)
gtest_discover_tests(ring_buffer_test)

add_executable(event_store_test
  events/event_store_test.cpp
)
target_link_libraries(event_store_test
  nvecd_events
  gtest_main
)
gtest_discover_tests(event_store_test)

add_executable(dedup_cache_test
  events/dedup_cache_test.cpp
)
target_link_libraries(dedup_cache_test
  nvecd_events
  nvecd_utils
  gtest
  gtest_main
)
gtest_discover_tests(dedup_cache_test)

add_executable(event_store_dedup_test
  events/event_store_dedup_test.cpp
)
target_link_libraries(event_store_dedup_test
  nvecd_events
  nvecd_utils
  gtest
  gtest_main
)
gtest_discover_tests(event_store_dedup_test)

add_executable(event_type_test
  events/event_type_test.cpp
)
target_link_libraries(event_type_test
  nvecd_events
  nvecd_utils
  gtest
  gtest_main
)
gtest_discover_tests(event_type_test)

add_executable(co_occurrence_index_test
  events/co_occurrence_index_test.cpp
)
target_link_libraries(co_occurrence_index_test
  nvecd_events
  gtest_main
)
gtest_discover_tests(co_occurrence_index_test)

add_executable(state_cache_test
  events/state_cache_test.cpp
)
target_link_libraries(state_cache_test
  nvecd_events
  gtest_main
)
gtest_discover_tests(state_cache_test)

add_executable(co_occurrence_temporal_test
  events/co_occurrence_temporal_test.cpp
)
target_link_libraries(co_occurrence_temporal_test
  nvecd_events
  gtest_main
)
gtest_discover_tests(co_occurrence_temporal_test)

add_executable(co_occurrence_negative_test
  events/co_occurrence_negative_test.cpp
)
target_link_libraries(co_occurrence_negative_test
  nvecd_events
  gtest_main
)
gtest_discover_tests(co_occurrence_negative_test)

# Vectors tests
add_executable(vector_store_test
  vectors/vector_store_test.cpp
)
target_link_libraries(vector_store_test
  nvecd_vectors
  gtest_main
)
gtest_discover_tests(vector_store_test)

# IVF index tests
add_executable(ivf_index_test
  vectors/ivf_index_test.cpp
)
target_link_libraries(ivf_index_test
  nvecd_vectors
  nvecd_utils
  gtest
  gtest_main
)
gtest_discover_tests(ivf_index_test)

# HNSW index tests
add_executable(hnsw_index_test
  vectors/hnsw_index_test.cpp
)
target_link_libraries(hnsw_index_test
  nvecd_vectors
  nvecd_utils
  gtest
  gtest_main
)
gtest_discover_tests(hnsw_index_test)

# Metadata filter tests
add_executable(metadata_filter_test
  vectors/metadata_filter_test.cpp
)
target_link_libraries(metadata_filter_test
  nvecd_vectors
  nvecd_utils
  gtest
  gtest_main
)
gtest_discover_tests(metadata_filter_test)

# Metadata store tests
add_executable(metadata_store_test
  vectors/metadata_store_test.cpp
)
target_link_libraries(metadata_store_test
  nvecd_vectors
  nvecd_utils
  gtest
  gtest_main
)
gtest_discover_tests(metadata_store_test)

# SIMD correctness tests
add_executable(distance_simd_test
  vectors/distance_simd_test.cpp
)
target_link_libraries(distance_simd_test
  nvecd_vectors
  nvecd_utils
  gtest
  gtest_main
)
gtest_discover_tests(distance_simd_test)

# Distance function tests (high-level API)
add_executable(distance_functions_test
  vectors/distance_functions_test.cpp
)
target_link_libraries(distance_functions_test
  nvecd_vectors
  nvecd_utils
  gtest
  gtest_main
)
gtest_discover_tests(distance_functions_test)

# Similarity tests
add_executable(similarity_engine_test
  similarity/similarity_engine_test.cpp
)
target_link_libraries(similarity_engine_test
  nvecd_similarity
  gtest_main
)
gtest_discover_tests(similarity_engine_test)

add_executable(adaptive_fusion_test
  similarity/adaptive_fusion_test.cpp
)
target_link_libraries(adaptive_fusion_test
  nvecd_similarity
  gtest_main
)
gtest_discover_tests(adaptive_fusion_test)

# Cache tests
add_executable(result_compressor_test
  cache/result_compressor_test.cpp
)
target_link_libraries(result_compressor_test
  nvecd_cache
  nvecd_similarity
  nvecd_utils
  gtest
  gtest_main
)
gtest_discover_tests(result_compressor_test)

add_executable(similarity_cache_test
  cache/similarity_cache_test.cpp
)
target_link_libraries(similarity_cache_test
  nvecd_cache
  nvecd_similarity
  nvecd_utils
  gtest
  gtest_main
)
gtest_discover_tests(similarity_cache_test)

add_executable(cache_key_test
  cache/cache_key_test.cpp
)
target_link_libraries(cache_key_test
  nvecd_cache
  nvecd_utils
  gtest
  gtest_main
)
gtest_discover_tests(cache_key_test)

add_executable(cache_key_generator_test
  cache/cache_key_generator_test.cpp
)
target_link_libraries(cache_key_generator_test
  nvecd_cache
  nvecd_utils
  gtest
  gtest_main
)
gtest_discover_tests(cache_key_generator_test)

add_executable(md5_test
  cache/md5_test.cpp
)
target_link_libraries(md5_test
  nvecd_cache
  nvecd_utils
  gtest
  gtest_main
)
gtest_discover_tests(md5_test)

# Client library tests
add_executable(nvecdclient_test
  client/nvecdclient_test.cpp
)
target_link_libraries(nvecdclient_test
  nvecdclient
  nvecd_server
  nvecd_config
  nvecd_similarity
  nvecd_events
  nvecd_vectors
  nvecd_utils
  gtest
  gtest_main
)
gtest_discover_tests(nvecdclient_test
  DISCOVERY_TIMEOUT 30
  PROPERTIES LABELS "client"
)

add_executable(nvecdclient_c_test
  client/nvecdclient_c_test.cpp
)
target_link_libraries(nvecdclient_c_test
  nvecdclient
  nvecd_server
  nvecd_config
  nvecd_similarity
  nvecd_events
  nvecd_vectors
  nvecd_utils
  gtest
  gtest_main
)
gtest_discover_tests(nvecdclient_c_test
  DISCOVERY_TIMEOUT 30
  PROPERTIES LABELS "client"
)

# Note: CLI tests removed - nvecd-cli should be tested manually
# Interactive REPL testing is difficult to automate reliably
# Manual testing procedure documented in docs/en/libnvecdclient.md

# Storage tests
add_executable(snapshot_format_v1_test
  storage/snapshot_format_v1_test.cpp
)
target_link_libraries(snapshot_format_v1_test
  nvecd_storage
  nvecd_events
  nvecd_vectors
  nvecd_config
  nvecd_utils
  gtest
  gtest_main
)
gtest_discover_tests(snapshot_format_v1_test)

add_executable(snapshot_lock_test
  storage/snapshot_lock_test.cpp
)
target_link_libraries(snapshot_lock_test
  nvecd_storage
  nvecd_events
  nvecd_vectors
  nvecd_config
  nvecd_utils
  gtest
  gtest_main
)
gtest_discover_tests(snapshot_lock_test)

add_executable(snapshot_fork_test
  storage/snapshot_fork_test.cpp
)
target_link_libraries(snapshot_fork_test
  nvecd_storage
  nvecd_events
  nvecd_vectors
  nvecd_config
  nvecd_utils
  gtest
  gtest_main
)
gtest_discover_tests(snapshot_fork_test
  PROPERTIES TIMEOUT 30 LABELS integration
)

# Unix socket tests
add_executable(connection_acceptor_unix_test
  server/connection_acceptor_unix_test.cpp
)
target_link_libraries(connection_acceptor_unix_test
  nvecd_server
  nvecd_storage
  nvecd_cache
  nvecd_similarity
  nvecd_events
  nvecd_vectors
  nvecd_config
  nvecd_utils
  gtest
  gtest_main
)
gtest_discover_tests(connection_acceptor_unix_test
  PROPERTIES TIMEOUT 60
)

# Integration tests
add_subdirectory(integration)

# Benchmark tests (DISABLED by default, run with --gtest_also_run_disabled_tests)
add_subdirectory(benchmark)
