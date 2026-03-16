# End-to-End Integration Tests

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
)
