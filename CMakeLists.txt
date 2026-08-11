cmake_minimum_required(VERSION 4.1.2)

cmake_policy(SET CMP0091 NEW)

project(
  azzs
  VERSION 0.1.0
  DESCRIPTION "Windows Initial Setup Workbench"
  LANGUAGES CXX
)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

foreach(configuration IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER "${configuration}" configuration_upper)
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${configuration_upper}
      "${CMAKE_BINARY_DIR}/lib/${configuration}")
endforeach()

if(MSVC)
  # CMake 4.3 does not yet map CXX26 for MSVC 19.51; /std:c++latest is the
  # stable compiler switch for the project's C++26 target mode.
  add_compile_options(/std:c++latest /W4 /permissive- /Zc:__cplusplus /utf-8)
else()
  set(CMAKE_CXX_STANDARD 26)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

include(CTest)
include(cmake/AzzsProjectGuardrails.cmake)

add_subdirectory(src/domain)
add_subdirectory(src/application)
add_subdirectory(src/adapters/windows)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

azzs_finalize_project_guardrails()
