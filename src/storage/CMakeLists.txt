add_library(minweight_storage INTERFACE)

target_include_directories(minweight_storage
  INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(minweight_storage
  INTERFACE
    minweight_minpatricia
    minweight_runtime)
