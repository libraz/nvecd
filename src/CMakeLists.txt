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
  server/handlers/admin_handler.cpp
  server/handlers/info_handler.cpp
  server/handlers/debug_handler.cpp
  server/request_dispatcher.cpp
  server/http_server.cpp
  server/nvecd_server.cpp
)
target_link_libraries(nvecd_server PUBLIC
  nvecd_cache
  nvecd_config
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
  events/dedup_cache.cpp
  events/state_cache.cpp
)
target_link_libraries(nvecd_events PUBLIC
  nvecd_utils
  Threads::Threads
)

# Vectors library with SIMD support
add_library(nvecd_vectors STATIC
  vectors/vector_store.cpp
  vectors/cpu_features.cpp
)
target_link_libraries(nvecd_vectors PUBLIC
  nvecd_utils
  Threads::Threads
)

# Platform-specific SIMD optimizations
if(CMAKE_SYSTEM_PROCESSOR MATCHES "(x86)|(X86)|(amd64)|(AMD64)")
  # x86_64: Enable AVX2 if supported
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag("-mavx2" COMPILER_SUPPORTS_AVX2)

  if(COMPILER_SUPPORTS_AVX2)
    target_compile_options(nvecd_vectors PRIVATE -mavx2)
    message(STATUS "AVX2 SIMD enabled for vectors")
  else()
    message(WARNING "AVX2 not supported by compiler")
  endif()

elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm|aarch64|ARM|ARM64")
  # ARM: NEON is baseline for AArch64, explicit flag for ARMv7
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64")
    message(STATUS "NEON SIMD enabled (AArch64 baseline)")
  else()
    # ARMv7: Explicitly enable NEON
    target_compile_options(nvecd_vectors PRIVATE -mfpu=neon)
    message(STATUS "NEON SIMD enabled (ARMv7)")
  endif()
endif()

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

# Config library with JSON Schema support
# Reference: ../mygram-db/src/config/CMakeLists.txt
# Generates embedded schema header at build time

# Read schema file content
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/config/config-schema.json SCHEMA_JSON_CONTENT)

# Configure the embedded header
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/config/config_schema_embedded.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/config/config_schema_embedded.h
  @ONLY
)

# Make CMake reconfigure when schema file changes (enables auto-regeneration)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/config/config-schema.json
)

add_library(nvecd_config STATIC
  config/config.cpp
  config/config_help.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/config/config_schema_embedded.h  # Add as source to track changes
)

target_include_directories(nvecd_config PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}  # For generated header
)

target_link_libraries(nvecd_config PUBLIC
  nvecd_utils
  yaml-cpp
  nlohmann_json::nlohmann_json
  nlohmann_json_schema_validator
  spdlog
)

# Cache library
add_library(nvecd_cache STATIC
  cache/cache_key.cpp
  cache/cache_key_generator.cpp
  cache/md5.cpp
  cache/result_compressor.cpp
  cache/similarity_cache.cpp
)
target_link_libraries(nvecd_cache PUBLIC
  nvecd_similarity
  nvecd_utils
  lz4
  Threads::Threads
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

# Client library (C++ API)
add_library(nvecdclient SHARED
  client/nvecdclient.cpp
  client/nvecdclient_c.cpp
)
target_link_libraries(nvecdclient PUBLIC
  nvecd_utils
  Threads::Threads
)
set_target_properties(nvecdclient PROPERTIES
  VERSION 0.1.0
  SOVERSION 0
  PUBLIC_HEADER "client/nvecdclient.h;client/nvecdclient_c.h"
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

# CLI executable (nvecd-cli)
# Reference: ../mygram-db/src/cli/CMakeLists.txt
# Reusability: 90% (namespace/command changes only)
add_executable(nvecd-cli
  cli/nvecd-cli.cpp
)
target_link_libraries(nvecd-cli PRIVATE
  Threads::Threads
)

# Check for readline library (optional)
find_library(READLINE_LIBRARY readline)
if(READLINE_LIBRARY)
  target_link_libraries(nvecd-cli PRIVATE ${READLINE_LIBRARY})
  target_compile_definitions(nvecd-cli PRIVATE HAVE_READLINE)
  message(STATUS "readline library found - tab completion enabled")
else()
  message(STATUS "readline library not found - tab completion disabled")
endif()

# Install CLI executable
install(TARGETS nvecd-cli
  RUNTIME DESTINATION bin
)

# Install client library
install(TARGETS nvecdclient
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  PUBLIC_HEADER DESTINATION include/nvecd
)
