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
  gtest
  gtest_main
)
gtest_discover_tests(expected_test)

add_executable(network_utils_test
  utils/network_utils_test.cpp
)
target_link_libraries(network_utils_test
  nvecd_utils
  gtest
  gtest_main
)
gtest_discover_tests(network_utils_test)

# Config tests
add_executable(config_test
  config/config_test.cpp
)
target_link_libraries(config_test
  nvecd_config
  nvecd_utils
  gtest
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

# Server tests
add_executable(command_parser_test
  server/command_parser_test.cpp
)
target_link_libraries(command_parser_test
  nvecd_server
  nvecd_utils
  gtest
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

# Events tests
add_executable(ring_buffer_test
  events/ring_buffer_test.cpp
)
target_link_libraries(ring_buffer_test
  gtest
  gtest_main
)
gtest_discover_tests(ring_buffer_test)

add_executable(event_store_test
  events/event_store_test.cpp
)
target_link_libraries(event_store_test
  nvecd_events
  nvecd_utils
  gtest
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
  nvecd_utils
  gtest
  gtest_main
)
gtest_discover_tests(co_occurrence_index_test)

# Vectors tests
add_executable(vector_store_test
  vectors/vector_store_test.cpp
)
target_link_libraries(vector_store_test
  nvecd_vectors
  nvecd_utils
  gtest
  gtest_main
)
gtest_discover_tests(vector_store_test)

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

# Similarity tests
add_executable(similarity_engine_test
  similarity/similarity_engine_test.cpp
)
target_link_libraries(similarity_engine_test
  nvecd_similarity
  nvecd_events
  nvecd_vectors
  nvecd_utils
  gtest
  gtest_main
)
gtest_discover_tests(similarity_engine_test)

# Cache tests
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
  PROPERTIES LABELS "client"
)

# Note: CLI tests removed - nvecd-cli should be tested manually
# Interactive REPL testing is difficult to automate reliably
# Manual testing procedure documented in docs/en/libnvecdclient.md

# Integration tests
add_subdirectory(integration)
