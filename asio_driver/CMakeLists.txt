add_library(aes67asio SHARED AsioDriver.cpp AsioDriver.def AsioDriver.rc)

target_include_directories(aes67asio PRIVATE
  ${asio_sdk_SOURCE_DIR}/common
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_BINARY_DIR}/generated)

target_link_libraries(aes67asio PRIVATE ole32 advapi32 user32 shell32 winmm avrt)

set_target_properties(aes67asio PROPERTIES
  OUTPUT_NAME "AES67BridgeASIO"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/src")
