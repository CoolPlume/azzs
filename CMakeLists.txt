cmake_minimum_required(VERSION 4.1.2)

cmake_policy(SET CMP0091 NEW)

set(AZZS_PRODUCT_VERSION_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/release/product-version.json")
if(NOT EXISTS "${AZZS_PRODUCT_VERSION_SOURCE}")
  message(FATAL_ERROR
    "The authoritative product version source is missing: ${AZZS_PRODUCT_VERSION_SOURCE}")
endif()

file(READ "${AZZS_PRODUCT_VERSION_SOURCE}" azzs_product_version_json)

function(azzs_read_product_version_field field destination)
  string(JSON value ERROR_VARIABLE error GET "${azzs_product_version_json}" "${field}")
  if(NOT error STREQUAL "NOTFOUND")
    message(FATAL_ERROR
      "The authoritative product version source has no usable '${field}' field: ${error}")
  endif()
  set("${destination}" "${value}" PARENT_SCOPE)
endfunction()

azzs_read_product_version_field(schemaVersion AZZS_PRODUCT_VERSION_SCHEMA)
azzs_read_product_version_field(applicationVersion AZZS_APPLICATION_VERSION)
azzs_read_product_version_field(releaseChannel AZZS_APPLICATION_RELEASE_CHANNEL)
azzs_read_product_version_field(cmakeVersion AZZS_CMAKE_PROJECT_VERSION)
azzs_read_product_version_field(windowsVersion AZZS_WINDOWS_VERSION)
azzs_read_product_version_field(wixVersion AZZS_WIX_VERSION)

if(NOT "${AZZS_PRODUCT_VERSION_SCHEMA}" STREQUAL "1")
  message(FATAL_ERROR "Unsupported product version schema: ${AZZS_PRODUCT_VERSION_SCHEMA}")
endif()
if(NOT "${AZZS_APPLICATION_VERSION}" MATCHES
       "^[0-9]+\\.[0-9]+\\.[0-9]+(-[0-9A-Za-z]+(\\.[0-9A-Za-z]+)*)?$")
  message(FATAL_ERROR
    "applicationVersion must be a three-part semantic version with an optional prerelease suffix")
endif()
if(NOT "${AZZS_APPLICATION_RELEASE_CHANNEL}" STREQUAL "stable" AND
   NOT "${AZZS_APPLICATION_RELEASE_CHANNEL}" STREQUAL "prerelease")
  message(FATAL_ERROR "releaseChannel must be stable or prerelease")
endif()
if("${AZZS_APPLICATION_VERSION}" MATCHES "-" AND
   NOT "${AZZS_APPLICATION_RELEASE_CHANNEL}" STREQUAL "prerelease")
  message(FATAL_ERROR "Prerelease applicationVersion requires releaseChannel prerelease")
endif()
foreach(version_name IN ITEMS AZZS_CMAKE_PROJECT_VERSION AZZS_WIX_VERSION)
  if(NOT "${${version_name}}" MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "${version_name} must be a three-part numeric version")
  endif()
endforeach()
if(NOT "${AZZS_WINDOWS_VERSION}" MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "windowsVersion must be a four-part numeric version")
endif()

# Numeric application versions use the direct mapping. Prerelease mappings
# remain explicit fields in the same source because Windows and WiX accept
# numbers only.
if("${AZZS_APPLICATION_VERSION}" MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  if(NOT "${AZZS_CMAKE_PROJECT_VERSION}" STREQUAL "${AZZS_APPLICATION_VERSION}" OR
     NOT "${AZZS_WIX_VERSION}" STREQUAL "${AZZS_APPLICATION_VERSION}" OR
     NOT "${AZZS_WINDOWS_VERSION}" STREQUAL "${AZZS_APPLICATION_VERSION}.0")
    message(FATAL_ERROR
      "Numeric product versions must map directly to CMake, WiX, and Windows versions")
  endif()
endif()

string(REPLACE "." "," AZZS_WINDOWS_VERSION_COMMAS "${AZZS_WINDOWS_VERSION}")
set(AZZS_WINUI_VERSIONED_RESOURCE_DIRECTORY
    "${CMAKE_BINARY_DIR}/generated/winui")
file(MAKE_DIRECTORY "${AZZS_WINUI_VERSIONED_RESOURCE_DIRECTORY}")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/adapters/ui/winui/app.manifest"
  "${AZZS_WINUI_VERSIONED_RESOURCE_DIRECTORY}/app.manifest"
  @ONLY)

project(
  azzs
  VERSION ${AZZS_CMAKE_PROJECT_VERSION}
  DESCRIPTION "Windows Initial Setup Workbench"
  LANGUAGES CXX
)

option(
  AZZS_ENABLE_STARTUP_DIAGNOSTIC_DEVICE_DATA_ROOT
  "Allow an explicitly enabled diagnostic build to read a device-data root override"
  OFF
)
set(AZZS_STARTUP_DIAGNOSTIC_DEVICE_DATA_ROOT_ENABLED 0)
if(AZZS_ENABLE_STARTUP_DIAGNOSTIC_DEVICE_DATA_ROOT)
  set(AZZS_STARTUP_DIAGNOSTIC_DEVICE_DATA_ROOT_ENABLED 1)
endif()
set(AZZS_STARTUP_DIAGNOSTIC_CONFIG_DIRECTORY
    "${CMAKE_CURRENT_SOURCE_DIR}/out/obj/winui/generated")
file(MAKE_DIRECTORY "${AZZS_STARTUP_DIAGNOSTIC_CONFIG_DIRECTORY}")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/startup_diagnostic_config.hpp.in"
  "${AZZS_STARTUP_DIAGNOSTIC_CONFIG_DIRECTORY}/startup_diagnostic_config.hpp"
  @ONLY
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
add_subdirectory(src/adapters/infrastructure)
add_subdirectory(src/adapters/windows)

if(BUILD_TESTING)
  add_subdirectory(src/adapters/ui/winui/DesignSystem)
  add_subdirectory(tests)
endif()

azzs_finalize_project_guardrails()
