add_library(azzs_software_catalog_domain STATIC
  src/software_catalog.cpp
)

add_library(azzs::software_catalog_domain ALIAS azzs_software_catalog_domain)

target_include_directories(azzs_software_catalog_domain
  PUBLIC
    include
)

azzs_project_target(
  TARGET azzs_software_catalog_domain
  ROLE domain
)
