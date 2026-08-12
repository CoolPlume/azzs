add_library(azzs_software_selection_application STATIC
  src/software_selection.cpp
)

add_library(azzs::software_selection_application ALIAS
  azzs_software_selection_application)

target_include_directories(azzs_software_selection_application
  PUBLIC
    include
)

azzs_project_target(
  TARGET azzs_software_selection_application
  ROLE application
  PUBLIC_LINKS
    azzs::application
    azzs::software_selection_domain
    azzs::software_catalog_application
    azzs::architecture_selection_application
)
