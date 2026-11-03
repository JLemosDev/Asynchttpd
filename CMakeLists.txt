cmake_minimum_required(VERSION 3.16)
project(async_httpd LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

add_library(httpcore STATIC src/http_parser.cpp src/router.cpp src/server.cpp)
target_include_directories(httpcore PUBLIC src)
target_compile_options(httpcore PUBLIC -Wall -Wextra -Wpedantic)
target_link_libraries(httpcore PUBLIC Threads::Threads)

add_executable(httpd src/main.cpp)
target_link_libraries(httpd PRIVATE httpcore)

enable_testing()
add_executable(tests tests/tests.cpp)
target_link_libraries(tests PRIVATE httpcore)
add_test(NAME unit COMMAND tests)
