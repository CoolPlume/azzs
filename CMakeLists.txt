cmake_minimum_required(VERSION 4.1.2)

cmake_policy(SET CMP0091 NEW)

project(
  azzs
  VERSION 0.1.0
  DESCRIPTION "Windows Initial Setup Workbench"
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

foreach(configuration IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER "${configuration}" configuration_upper)
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${configuration_upper}
      "${CMAKE_BINARY_DIR}/lib/${configuration}")
endforeach()

if(MSVC)
  add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

include(CTest)

add_subdirectory(src/domain)
add_subdirectory(src/application)

if(WIN32)
  add_subdirectory(src/adapters/windows)
endif()

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
