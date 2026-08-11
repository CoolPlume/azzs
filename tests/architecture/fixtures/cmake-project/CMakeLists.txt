cmake_minimum_required(VERSION 4.1.2)

project(azzs_guardrail_negative LANGUAGES NONE)

if(NOT DEFINED AZZS_GUARDRAILS_MODULE_DIR)
  message(FATAL_ERROR "AZZS_GUARDRAILS_MODULE_DIR is required")
endif()
if(NOT DEFINED AZZS_NEGATIVE_SCENARIO)
  message(FATAL_ERROR "AZZS_NEGATIVE_SCENARIO is required")
endif()

include(CTest)
include("${AZZS_GUARDRAILS_MODULE_DIR}/AzzsProjectGuardrails.cmake")

add_library(Threads::Threads INTERFACE IMPORTED GLOBAL)
if(AZZS_NEGATIVE_SCENARIO STREQUAL "imported-cycle")
  set_property(TARGET Threads::Threads PROPERTY
    INTERFACE_LINK_LIBRARIES azzs_test_support)
elseif(AZZS_NEGATIVE_SCENARIO STREQUAL "imported-source-bypass")
  set_property(TARGET Threads::Threads PROPERTY
    INTERFACE_SOURCES "${CMAKE_SOURCE_DIR}/tests/support/rogue.cpp")
endif()

add_subdirectory(src/domain)
add_subdirectory(src/application)
add_subdirectory(src/adapters/infrastructure)
add_subdirectory(src/adapters/windows)
if(BUILD_TESTING)
  add_subdirectory(tests/support)
endif()

azzs_finalize_project_guardrails()

if(AZZS_NEGATIVE_SCENARIO STREQUAL "raw-test")
  add_test(NAME bypass.raw COMMAND "${CMAKE_COMMAND}" -E true)
  set_tests_properties(bypass.raw PROPERTIES LABELS architecture)
endif()
