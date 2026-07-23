cmake_minimum_required(VERSION 3.16)
project(nvecd_install_consumer LANGUAGES C CXX)

find_package(nvecd CONFIG REQUIRED)

add_executable(nvecd_cpp_consumer main.cpp)
target_compile_features(nvecd_cpp_consumer PRIVATE cxx_std_17)
target_link_libraries(nvecd_cpp_consumer PRIVATE nvecd::client)

add_executable(nvecd_c_consumer main.c)
target_link_libraries(nvecd_c_consumer PRIVATE nvecd::client)
