# Third-party dependencies for nvecd
#
# Reference: ../mygram-db/third_party/CMakeLists.txt
# Reusability: 90% (added nlohmann/json and httplib for HTTP API support)

include(FetchContent)

# Set policies
if(POLICY CMP0048)
  cmake_policy(SET CMP0048 NEW)
endif()

# yaml-cpp for YAML parsing
FetchContent_Declare(
  yaml-cpp
  GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
  # yaml-cpp 0.9.0. Pin the immutable commit rather than a mutable branch so
  # clean builds are reproducible and reviewable.
  GIT_TAG 56e3bb550c91fd7005566f19c079cb7a503223cf
)
set(YAML_CPP_FORMAT_SOURCE OFF CACHE BOOL "" FORCE)
set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(yaml-cpp)

# Mark yaml-cpp includes as system to exclude from clang-tidy
get_target_property(YAML_CPP_IID yaml-cpp INTERFACE_INCLUDE_DIRECTORIES)
if(YAML_CPP_IID)
  set_target_properties(yaml-cpp PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${YAML_CPP_IID}")
endif()

# Google Test for testing
if(BUILD_TESTS)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.18.0
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  # Mark googletest includes as system
  get_target_property(GTEST_IID gtest INTERFACE_INCLUDE_DIRECTORIES)
  if(GTEST_IID)
    set_target_properties(gtest PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${GTEST_IID}")
  endif()
  get_target_property(GTEST_MAIN_IID gtest_main INTERFACE_INCLUDE_DIRECTORIES)
  if(GTEST_MAIN_IID)
    set_target_properties(gtest_main PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${GTEST_MAIN_IID}")
  endif()
endif()

# spdlog for logging
FetchContent_Declare(
  spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  # Pin a released tag rather than a branch so clean builds are reproducible.
  # Must stay at or above v1.15.2, which carries the fix for unbounded
  # resource consumption in the pattern formatter (CVE-2025-6140).
  GIT_TAG v1.17.0
)
FetchContent_MakeAvailable(spdlog)

# Mark spdlog includes as system
get_target_property(SPDLOG_IID spdlog INTERFACE_INCLUDE_DIRECTORIES)
if(SPDLOG_IID)
  set_target_properties(spdlog PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${SPDLOG_IID}")
endif()

# LZ4 for cache compression
FetchContent_Declare(
  lz4
  GIT_REPOSITORY https://github.com/lz4/lz4.git
  GIT_TAG v1.10.0
  # lz4 keeps its CMake project under build/cmake and builds command line tools
  # and install rules nvecd has no use for. Naming a subdirectory that holds no
  # CMakeLists.txt fetches the sources without configuring the upstream project,
  # leaving the compression core below as the only thing built from them.
  SOURCE_SUBDIR nvecd-does-not-configure-lz4
)
FetchContent_MakeAvailable(lz4)

if(NOT TARGET lz4)
  add_library(lz4 STATIC
    ${lz4_SOURCE_DIR}/lib/lz4.c
    ${lz4_SOURCE_DIR}/lib/lz4hc.c
    ${lz4_SOURCE_DIR}/lib/lz4frame.c
    ${lz4_SOURCE_DIR}/lib/xxhash.c)
  target_include_directories(lz4 PUBLIC ${lz4_SOURCE_DIR}/lib)
  set_target_properties(lz4 PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

get_target_property(LZ4_IID lz4 INTERFACE_INCLUDE_DIRECTORIES)
if(LZ4_IID)
  set_target_properties(lz4 PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${LZ4_IID}")
endif()

# nlohmann/json for JSON parsing (HTTP API)
# Reference: ../mygram-db/third_party/CMakeLists.txt (lines 120-134)
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.12.0
)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json)

# Mark nlohmann_json includes as system to exclude from clang-tidy
get_target_property(NLOHMANN_JSON_IID nlohmann_json INTERFACE_INCLUDE_DIRECTORIES)
if(NLOHMANN_JSON_IID)
  set_target_properties(nlohmann_json PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${NLOHMANN_JSON_IID}")
endif()

# cpp-httplib for HTTP server (header-only)
# Reference: ../mygram-db/third_party/CMakeLists.txt (lines 152-170)
FetchContent_Declare(
  httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG v0.54.1
)
set(HTTPLIB_REQUIRE_OPENSSL OFF CACHE BOOL "" FORCE)
set(HTTPLIB_REQUIRE_ZLIB OFF CACHE BOOL "" FORCE)
set(HTTPLIB_REQUIRE_BROTLI OFF CACHE BOOL "" FORCE)
set(HTTPLIB_REQUIRE_ZSTD OFF CACHE BOOL "" FORCE)
# The REQUIRE_ options above only decide whether a missing library is fatal.
# Detection is governed by the USE_*_IF_AVAILABLE options, which default to ON
# and would link whichever of these the build host happens to have installed --
# so the same source would produce a different binary on two machines, and the
# include directories those packages export leak into every target that links
# httplib. nvecd serves plain HTTP and compresses nothing at this layer, so the
# detection is turned off rather than merely made non-fatal.
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_WOLFSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_MBEDTLS_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(httplib)

# Mark httplib includes as system to exclude from clang-tidy
if(TARGET httplib)
  get_target_property(HTTPLIB_IID httplib INTERFACE_INCLUDE_DIRECTORIES)
  if(HTTPLIB_IID)
    set_target_properties(httplib PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${HTTPLIB_IID}")
  endif()
endif()

# nlohmann_json_schema_validator for config validation
# Reference: ../mygram-db/third_party/CMakeLists.txt
FetchContent_Declare(
  json-schema-validator
  GIT_REPOSITORY https://github.com/pboettch/json-schema-validator.git
  GIT_TAG 2.4.0
)
set(JSON_VALIDATOR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(json-schema-validator)

# Mark json-schema-validator includes as system
if(TARGET nlohmann_json_schema_validator)
  get_target_property(JSON_VALIDATOR_IID nlohmann_json_schema_validator INTERFACE_INCLUDE_DIRECTORIES)
  if(JSON_VALIDATOR_IID)
    set_target_properties(nlohmann_json_schema_validator PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${JSON_VALIDATOR_IID}")
  endif()
endif()
