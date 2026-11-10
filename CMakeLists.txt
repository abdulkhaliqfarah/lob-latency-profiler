cmake_minimum_required(VERSION 3.20)
project(lob_latency_profiler CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# The matching engine under measurement, vendored from
# github.com/abdulkhaliqfarah/limit-order-book (same author) so this repo
# builds standalone.
add_library(lob third_party/lob/src/order_book.cpp)
target_include_directories(lob PUBLIC third_party/lob/include)

add_library(prof INTERFACE)
target_include_directories(prof INTERFACE include)
target_link_libraries(prof INTERFACE lob)

add_executable(profile tools/profile.cpp)
target_link_libraries(profile PRIVATE prof)

add_executable(run_stream tools/run_stream.cpp)
target_link_libraries(run_stream PRIVATE prof)

enable_testing()
add_executable(test_profiler tests/test_profiler.cpp)
target_link_libraries(test_profiler PRIVATE prof)
add_test(NAME profiler_scenarios COMMAND test_profiler)
