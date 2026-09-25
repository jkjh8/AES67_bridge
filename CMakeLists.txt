cmake_minimum_required(VERSION 3.21)
project(aes67bridge VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(MSVC)
  add_compile_options(/utf-8 /W3 /permissive-)
  add_compile_definitions(
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    _WINSOCK_DEPRECATED_NO_WARNINGS)
endif()

include(FetchContent)

FetchContent_Declare(json_hpp
  URL https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp
  DOWNLOAD_NO_EXTRACT TRUE)
FetchContent_MakeAvailable(json_hpp)

add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE ${json_hpp_SOURCE_DIR})

set(LIBSAMPLERATE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LIBSAMPLERATE_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(libsamplerate
  GIT_REPOSITORY https://github.com/libsndfile/libsamplerate.git
  GIT_TAG 0.2.2)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(libsamplerate)
unset(CMAKE_POLICY_VERSION_MINIMUM)

FetchContent_Declare(webview2
  URL https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.2903.40
  DOWNLOAD_NAME webview2.zip)
FetchContent_MakeAvailable(webview2)

add_library(webview2 INTERFACE)
target_include_directories(webview2 INTERFACE ${webview2_SOURCE_DIR}/build/native/include)
target_link_libraries(webview2 INTERFACE
  ${webview2_SOURCE_DIR}/build/native/x64/WebView2LoaderStatic.lib)

FetchContent_Declare(asio_sdk
  GIT_REPOSITORY https://github.com/audiosdk/asio.git
  GIT_TAG 496a0765b8bb9c26f764f22f9a9712a937177db2)
FetchContent_GetProperties(asio_sdk)
if(NOT asio_sdk_POPULATED)
  FetchContent_Populate(asio_sdk)
endif()

add_subdirectory(src)
add_subdirectory(asio_driver)
