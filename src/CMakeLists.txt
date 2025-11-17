# nvecd source files
#
# Reference: ../mygram-db/src/CMakeLists.txt
# Reusability: 60% (simplified for nvecd structure)

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# Utility library
add_library(nvecd_utils STATIC
  utils/network_utils.cpp
  utils/memory_utils.cpp
  utils/string_utils.cpp
)
target_link_libraries(nvecd_utils PUBLIC
  Threads::Threads
  spdlog
)

# Server library
add_library(nvecd_server STATIC
  server/thread_pool.cpp
  server/connection_acceptor.cpp
  server/connection_io_handler.cpp
  server/command_parser.cpp
  server/handlers/info_handler.cpp
  server/handlers/debug_handler.cpp
  server/request_dispatcher.cpp
  server/http_server.cpp
  server/nvecd_server.cpp
)
target_link_libraries(nvecd_server PUBLIC
  nvecd_events
  nvecd_vectors
  nvecd_similarity
  nvecd_storage
  nvecd_utils
  httplib
  nlohmann_json
  Threads::Threads
  spdlog
)

# Events library
add_library(nvecd_events STATIC
  events/event_store.cpp
  events/co_occurrence_index.cpp
)
target_link_libraries(nvecd_events PUBLIC
  nvecd_utils
  Threads::Threads
)

# Vectors library
add_library(nvecd_vectors STATIC
  vectors/vector_store.cpp
)
target_link_libraries(nvecd_vectors PUBLIC
  nvecd_utils
  Threads::Threads
)

# Similarity library
add_library(nvecd_similarity STATIC
  similarity/similarity_engine.cpp
)
target_link_libraries(nvecd_similarity PUBLIC
  nvecd_events
  nvecd_vectors
  nvecd_utils
  Threads::Threads
)

# Config library
add_library(nvecd_config STATIC
  config/config.cpp
)
target_link_libraries(nvecd_config PUBLIC
  nvecd_utils
  yaml-cpp
)

# Storage library
add_library(nvecd_storage STATIC
  storage/snapshot_format_v1.cpp
)
target_link_libraries(nvecd_storage PUBLIC
  nvecd_events
  nvecd_vectors
  nvecd_utils
  ZLIB::ZLIB
  Threads::Threads
)

# Main executable
add_executable(nvecd
  main.cpp
)
target_link_libraries(nvecd PRIVATE
  nvecd_server
  nvecd_storage
  nvecd_config
  nvecd_events
  nvecd_vectors
  nvecd_similarity
  nvecd_utils
  spdlog
  Threads::Threads
)

# Install executable
install(TARGETS nvecd
  RUNTIME DESTINATION bin
)
