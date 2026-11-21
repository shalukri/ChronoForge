cmake_minimum_required(VERSION 3.16)
project(ChronoForge VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)            # <bit>: std::bit_cast, std::countl_zero / countr_zero
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

add_library(chronoforge_core STATIC
  src/models/label_set.cpp
  src/algorithms/gorilla_codec.cpp
  src/services/inverted_index.cpp
  src/services/cardinality_guard.cpp
  src/services/storage_engine.cpp
  src/services/query_engine.cpp)
target_include_directories(chronoforge_core PUBLIC include)
target_link_libraries(chronoforge_core PUBLIC Threads::Threads)
if(MSVC)
  target_compile_options(chronoforge_core PRIVATE /W4)
else()
  target_compile_options(chronoforge_core PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
endif()

add_executable(chronoforge src/main.cpp)
target_include_directories(chronoforge PRIVATE src)
target_link_libraries(chronoforge PRIVATE chronoforge_core)

add_executable(chronoforge_tests tests/test_suite.cpp)
target_link_libraries(chronoforge_tests PRIVATE chronoforge_core)

enable_testing()
add_test(NAME chronoforge_tests COMMAND chronoforge_tests)

# Optional sanitizer builds:  cmake -B build-asan -DCF_SANITIZE=address,undefined   (or: thread)
set(CF_SANITIZE "" CACHE STRING "Comma-separated sanitizers, e.g. address,undefined or thread")
if(CF_SANITIZE)
  foreach(t chronoforge_core chronoforge chronoforge_tests)
    target_compile_options(${t} PRIVATE -fsanitize=${CF_SANITIZE} -fno-omit-frame-pointer -g)
    target_link_options(${t} PRIVATE -fsanitize=${CF_SANITIZE})
  endforeach()
endif()
