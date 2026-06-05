add_library(minweight_store INTERFACE)
add_library(minweight_store::minweight_store ALIAS minweight_store)

target_include_directories(minweight_store
  INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(minweight_store
  INTERFACE
    minpatricia::minpatricia)
