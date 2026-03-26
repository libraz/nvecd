# End-to-End Integration Tests
#
# All tests in this directory use LABELS "integration" so they can be
# excluded from CI with: ctest --label-exclude integration

add_executable(end_to_end_test
  end_to_end_test.cpp
)

target_link_libraries(end_to_end_test
  nvecd_server
  nvecd_storage
  gtest_main
)

gtest_discover_tests(end_to_end_test
  DISCOVERY_MODE POST_BUILD
  DISCOVERY_TIMEOUT 30
  PROPERTIES
    RESOURCE_LOCK server_port
    TIMEOUT 300
    LABELS integration
)

# Similarity End-to-End Tests

add_executable(similarity_e2e_test
  similarity_e2e_test.cpp
)

target_link_libraries(similarity_e2e_test
  nvecd_server
  nvecd_storage
  gtest_main
)

gtest_discover_tests(similarity_e2e_test
  DISCOVERY_MODE POST_BUILD
  DISCOVERY_TIMEOUT 30
  PROPERTIES
    RESOURCE_LOCK server_port
    TIMEOUT 300
    LABELS integration
)

# Performance Benchmark Tests

add_executable(performance_e2e_test
  performance_e2e_test.cpp
)

target_link_libraries(performance_e2e_test
  nvecd_server
  nvecd_storage
  gtest_main
)

gtest_discover_tests(performance_e2e_test
  DISCOVERY_MODE POST_BUILD
  DISCOVERY_TIMEOUT 30
  PROPERTIES
    RESOURCE_LOCK server_port
    TIMEOUT 600
    LABELS integration
)

# Concurrent End-to-End Tests

add_executable(concurrent_e2e_test
  concurrent_e2e_test.cpp
)

target_link_libraries(concurrent_e2e_test
  nvecd_server
  nvecd_storage
  gtest_main
)

gtest_discover_tests(concurrent_e2e_test
  DISCOVERY_MODE POST_BUILD
  DISCOVERY_TIMEOUT 30
  PROPERTIES
    RESOURCE_LOCK server_port
    TIMEOUT 600
    LABELS integration
)

# Cache End-to-End Tests

add_executable(cache_e2e_test
  cache_e2e_test.cpp
)

target_link_libraries(cache_e2e_test
  nvecd_server
  nvecd_storage
  gtest_main
)

gtest_discover_tests(cache_e2e_test
  DISCOVERY_MODE POST_BUILD
  DISCOVERY_TIMEOUT 30
  PROPERTIES
    RESOURCE_LOCK server_port
    TIMEOUT 300
    LABELS integration
)

# Metrics Accuracy End-to-End Tests

add_executable(metrics_e2e_test
  metrics_e2e_test.cpp
)

target_link_libraries(metrics_e2e_test
  nvecd_server
  nvecd_storage
  gtest_main
)

gtest_discover_tests(metrics_e2e_test
  DISCOVERY_MODE POST_BUILD
  DISCOVERY_TIMEOUT 30
  PROPERTIES
    RESOURCE_LOCK server_port
    TIMEOUT 300
    LABELS integration
)

# Edge Case End-to-End Tests

add_executable(edge_case_e2e_test
  edge_case_e2e_test.cpp
)

target_link_libraries(edge_case_e2e_test
  nvecd_server
  nvecd_storage
  gtest_main
)

gtest_discover_tests(edge_case_e2e_test
  DISCOVERY_MODE POST_BUILD
  DISCOVERY_TIMEOUT 30
  PROPERTIES
    RESOURCE_LOCK server_port
    TIMEOUT 300
    LABELS integration
)

# Adversarial End-to-End Tests

add_executable(adversarial_e2e_test
  adversarial_e2e_test.cpp
)

target_link_libraries(adversarial_e2e_test
  nvecd_server
  nvecd_storage
  gtest_main
)

gtest_discover_tests(adversarial_e2e_test
  DISCOVERY_MODE POST_BUILD
  DISCOVERY_TIMEOUT 30
  PROPERTIES
    RESOURCE_LOCK server_port
    TIMEOUT 120
    LABELS integration
)

# TikTok Scenario End-to-End Tests

add_executable(tiktok_scenario_e2e_test
  tiktok_scenario_e2e_test.cpp
)

target_link_libraries(tiktok_scenario_e2e_test
  nvecd_server
  nvecd_storage
  gtest_main
)

gtest_discover_tests(tiktok_scenario_e2e_test
  DISCOVERY_MODE POST_BUILD
  DISCOVERY_TIMEOUT 30
  PROPERTIES
    RESOURCE_LOCK server_port
    TIMEOUT 900
    LABELS integration
)

# TikTok 1M Scale Benchmark Tests

add_executable(tiktok_1m_scale_test
  tiktok_1m_scale_test.cpp
)

target_link_libraries(tiktok_1m_scale_test
  nvecd_server
  nvecd_storage
  gtest_main
)

gtest_discover_tests(tiktok_1m_scale_test
  DISCOVERY_MODE POST_BUILD
  DISCOVERY_TIMEOUT 30
  PROPERTIES
    RESOURCE_LOCK server_port
    TIMEOUT 1800
    LABELS integration
)

# Differentiation Features End-to-End Tests

add_executable(differentiation_e2e_test
  differentiation_e2e_test.cpp
)

target_link_libraries(differentiation_e2e_test
  nvecd_server
  nvecd_storage
  gtest_main
)

gtest_discover_tests(differentiation_e2e_test
  DISCOVERY_MODE POST_BUILD
  DISCOVERY_TIMEOUT 30
  PROPERTIES
    RESOURCE_LOCK server_port
    TIMEOUT 300
    LABELS integration
)
