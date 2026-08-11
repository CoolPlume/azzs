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

add_library(azzs_software_optimization_catalog_domain STATIC
  src/software_optimization_catalog.cpp
)

add_library(
  azzs::software_optimization_catalog_domain
  ALIAS azzs_software_optimization_catalog_domain
)

target_include_directories(azzs_software_optimization_catalog_domain
  PUBLIC
    include
)

azzs_project_target(
  TARGET azzs_software_optimization_catalog_domain
  ROLE domain
)

add_subdirectory(settings_catalog)
