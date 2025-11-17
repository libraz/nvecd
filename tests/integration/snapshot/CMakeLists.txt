# Snapshot Integration Tests
# Reference: ../mygram-db/tests/integration/server/CMakeLists.txt
# Reusability: 90%

# Snapshot Integration Tests
add_executable(snapshot_integration_test
  snapshot_integration_test.cpp
)

target_link_libraries(snapshot_integration_test
  nvecd_server      # Includes all dependencies transitively
  nvecd_storage     # Snapshot functionality
  gtest
  gtest_main
)

# RESOURCE_LOCK: Prevent parallel execution to avoid:
# - Port binding conflicts (server starts on random port)
# - File I/O conflicts (snapshot files in shared test directory)
gtest_discover_tests(snapshot_integration_test
  DISCOVERY_MODE POST_BUILD
  DISCOVERY_TIMEOUT 30
  PROPERTIES
    RESOURCE_LOCK server_port_and_storage_io
    TIMEOUT 300  # 5 minutes timeout for large snapshot test
)
