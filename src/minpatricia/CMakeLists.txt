add_library(minpatricia STATIC
  minpatricia.cc)
add_library(minpatricia::minpatricia ALIAS minpatricia)

target_include_directories(minpatricia
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_features(minpatricia PUBLIC cxx_std_20)

if(MINWEIGHT_BUILD_TESTS)
  add_executable(minpatricia_tests
    minpatricia_test.cc)
  target_link_libraries(minpatricia_tests PRIVATE minpatricia::minpatricia)
  target_compile_definitions(minpatricia_tests
    PRIVATE
      MINPATRICIA_TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata")
  add_test(NAME minpatricia_tests COMMAND minpatricia_tests)
endif()

if(MINWEIGHT_BUILD_BENCHMARKS)
  add_executable(minpatricia_bench
    minpatricia_bench.cc)
  target_link_libraries(minpatricia_bench PRIVATE minpatricia::minpatricia)
  target_compile_definitions(minpatricia_bench
    PRIVATE
      MINPATRICIA_TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata")
endif()
