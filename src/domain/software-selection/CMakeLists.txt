add_library(azzs_software_selection_domain STATIC
  src/software_selection.cpp
)

add_library(azzs::software_selection_domain ALIAS
  azzs_software_selection_domain)

target_include_directories(azzs_software_selection_domain
  PUBLIC
    include
)

azzs_project_target(
  TARGET azzs_software_selection_domain
  ROLE domain
  PUBLIC_LINKS
    azzs::software_catalog_domain
    azzs::architecture_selection_domain
)
