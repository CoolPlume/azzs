add_library(azzs_domain STATIC
  src/minimum_version_policy.cpp
)

add_library(azzs::domain ALIAS azzs_domain)

target_include_directories(azzs_domain
  PUBLIC
    include
)

azzs_project_target(
  TARGET azzs_domain
  ROLE domain
)

add_subdirectory(software-catalog)
