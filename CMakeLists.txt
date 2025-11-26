cmake_minimum_required(VERSION 3.15)

# Reference: ../mygram-db/CMakeLists.txt
# Reusability: 80% (removed MySQL/ICU detection, simplified)

# Set CMake policies
cmake_policy(SET CMP0048 NEW)
cmake_policy(SET CMP0077 NEW)

# Get version from environment or git tag
if(DEFINED ENV{NVECD_VERSION})
  set(PROJECT_VERSION_FROM_ENV "$ENV{NVECD_VERSION}")
  message(STATUS "Version from environment: ${PROJECT_VERSION_FROM_ENV}")
  project(nvecd VERSION ${PROJECT_VERSION_FROM_ENV} LANGUAGES CXX C)
else()
  execute_process(
    COMMAND git describe --tags --abbrev=0
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_TAG
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )

  if(GIT_TAG)
    string(REGEX REPLACE "^v" "" PROJECT_VERSION_FROM_GIT "${GIT_TAG}")
    message(STATUS "Version from git tag: ${PROJECT_VERSION_FROM_GIT}")
  else()
    set(PROJECT_VERSION_FROM_GIT "0.1.0")
    message(STATUS "No git tag found, using default version: ${PROJECT_VERSION_FROM_GIT}")
  endif()

  project(nvecd VERSION ${PROJECT_VERSION_FROM_GIT} LANGUAGES CXX C)
endif()

# C++ Standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# macOS specific settings
if(APPLE)
  execute_process(
    COMMAND xcrun --show-sdk-path
    OUTPUT_VARIABLE MACOS_SDK_PATH
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )

  if(MACOS_SDK_PATH)
    message(STATUS "Using macOS SDK path: ${MACOS_SDK_PATH}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -isysroot ${MACOS_SDK_PATH}")
    include_directories(SYSTEM "${MACOS_SDK_PATH}/usr/include/c++/v1")
    include_directories(SYSTEM "${MACOS_SDK_PATH}/usr/include")
  endif()

  # Add Homebrew paths
  if(EXISTS "/usr/local/include")
    include_directories(SYSTEM "/usr/local/include")
    link_directories("/usr/local/lib")
  endif()

  if(EXISTS "/opt/homebrew/include")
    include_directories(SYSTEM "/opt/homebrew/include")
    link_directories("/opt/homebrew/lib")
  endif()

  add_compile_options(-Wno-unused-command-line-argument -Qunused-arguments)
endif()

# Compiler flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  # Architecture flags: use -march=x86-64 for CI compatibility, -march=native for local builds
  # CI sets NVECD_PORTABLE_BUILD=ON to avoid "Illegal instruction" on different CPUs
  if(NVECD_PORTABLE_BUILD)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=x86-64")
  else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=native")
  endif()
  set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -fno-omit-frame-pointer")
  set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
endif()

# Export compile commands for clang-tidy
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Options
option(BUILD_TESTS "Build tests" ON)
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(ENABLE_COVERAGE "Enable code coverage" OFF)

# AddressSanitizer
if(ENABLE_ASAN)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
  set(CMAKE_LINKER_FLAGS "${CMAKE_LINKER_FLAGS} -fsanitize=address")
endif()

# ThreadSanitizer
if(ENABLE_TSAN)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=thread")
  set(CMAKE_LINKER_FLAGS "${CMAKE_LINKER_FLAGS} -fsanitize=thread")
endif()

# Code Coverage
if(ENABLE_COVERAGE)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(STATUS "Enabling code coverage")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} --coverage -O0 -g")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} --coverage -O0 -g")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --coverage")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} --coverage")
  else()
    message(WARNING "Code coverage is only supported with GCC or Clang")
  endif()
endif()

# Find packages
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)

# YAML-CPP detection - will be handled by FetchContent in third_party/CMakeLists.txt
# No need to find it on the system since we're using FetchContent

# Third-party dependencies
add_subdirectory(third_party)

# Suppress warnings from third-party libraries
if(TARGET spdlog)
  target_compile_options(spdlog PRIVATE -w)
endif()

# Source
add_subdirectory(src)

# Tests
if(BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# Code Coverage Report
if(ENABLE_COVERAGE)
  find_program(LCOV_PATH lcov)
  find_program(GENHTML_PATH genhtml)

  if(LCOV_PATH AND GENHTML_PATH)
    include(ProcessorCount)
    ProcessorCount(N)
    if(NOT N EQUAL 0)
      set(CTEST_PARALLEL_ARGS --parallel ${N})
    endif()

    add_custom_target(coverage
      COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/coverage
      COMMAND ${LCOV_PATH} --directory . --zerocounters
      COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure ${CTEST_PARALLEL_ARGS}
      COMMAND ${LCOV_PATH} --directory . --capture --output-file ${CMAKE_BINARY_DIR}/coverage/coverage.info
      COMMAND ${LCOV_PATH} --extract ${CMAKE_BINARY_DIR}/coverage/coverage.info
        '${CMAKE_SOURCE_DIR}/src/*'
        --output-file ${CMAKE_BINARY_DIR}/coverage/coverage_filtered.info
      COMMAND ${GENHTML_PATH} ${CMAKE_BINARY_DIR}/coverage/coverage_filtered.info
        --output-directory ${CMAKE_BINARY_DIR}/coverage/html
      COMMAND ${CMAKE_COMMAND} -E echo "Coverage report: ${CMAKE_BINARY_DIR}/coverage/html/index.html"
      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
      COMMENT "Generating code coverage report"
    )

    add_custom_target(coverage-clean
      COMMAND ${LCOV_PATH} --directory . --zerocounters
      COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}/coverage
      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
      COMMENT "Cleaning coverage data"
    )
  else()
    message(WARNING "lcov/genhtml not found. Coverage target unavailable.")
  endif()
endif()

# Install (disabled until main executable is implemented)
# install(TARGETS nvecd DESTINATION bin)

# Install configuration example
# if(EXISTS examples/config.yaml)
#   install(FILES examples/config.yaml DESTINATION etc/nvecd RENAME config.yaml.example)
# endif()

# Install documentation
# install(FILES README.md DESTINATION share/doc/nvecd)
