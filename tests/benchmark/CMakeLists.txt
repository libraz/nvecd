# Benchmark Tests
add_executable(similarity_benchmark
  similarity_benchmark.cpp
)

target_link_libraries(similarity_benchmark
  nvecd_server
  nvecd_storage
  gtest_main
)

gtest_discover_tests(similarity_benchmark
  DISCOVERY_MODE POST_BUILD
  DISCOVERY_TIMEOUT 30
  PROPERTIES
    TIMEOUT 600
    LABELS benchmark
)
