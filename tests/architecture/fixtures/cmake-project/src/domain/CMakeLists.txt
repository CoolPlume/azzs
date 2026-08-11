add_library(azzs_domain INTERFACE)
add_library(azzs::domain ALIAS azzs_domain)

target_include_directories(azzs_domain INTERFACE include)

if(AZZS_NEGATIVE_SCENARIO STREQUAL "source-bypass")
  target_sources(azzs_domain INTERFACE
    "${CMAKE_SOURCE_DIR}/tests/support/rogue.cpp")
elseif(AZZS_NEGATIVE_SCENARIO STREQUAL "include-bypass")
  target_include_directories(azzs_domain INTERFACE
    "${CMAKE_SOURCE_DIR}/src/application/include")
endif()

azzs_project_target(TARGET azzs_domain ROLE domain)
