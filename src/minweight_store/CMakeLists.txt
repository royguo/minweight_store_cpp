add_library(minweight_store STATIC
  store.cc)
add_library(minweight_store::minweight_store ALIAS minweight_store)

target_include_directories(minweight_store
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_features(minweight_store PUBLIC cxx_std_17)

target_link_libraries(minweight_store
  PUBLIC
    minpatricia::minpatricia)

if(MINWEIGHT_BUILD_TESTS)
  add_executable(minweight_store_tests
    store_test.cc)
  target_link_libraries(minweight_store_tests PRIVATE minweight_store::minweight_store)
  add_test(NAME minweight_store_tests COMMAND minweight_store_tests)
endif()

if(MINWEIGHT_BUILD_BENCHMARKS)
  add_executable(minweight_store_bench
    store_bench.cc)
  target_link_libraries(minweight_store_bench PRIVATE minweight_store::minweight_store)
endif()
