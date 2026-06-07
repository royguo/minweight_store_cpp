cmake_minimum_required(VERSION 3.20)

project(minweight_store_cpp
  VERSION 0.1.0
  LANGUAGES CXX)

if(NOT DEFINED CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MINWEIGHT_BUILD_TESTS "Build minweight_store_cpp tests" ON)
option(MINWEIGHT_BUILD_BENCHMARKS "Build minweight_store_cpp benchmarks" ON)

include(CTest)

add_subdirectory(src)
