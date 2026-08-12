add_library(azzs_application INTERFACE)
add_library(azzs::application ALIAS azzs_application)

target_include_directories(azzs_application INTERFACE include)

azzs_project_target(
  TARGET azzs_application
  ROLE application
  INTERFACE_LINKS
    azzs::domain
    Threads::Threads
)

add_library(azzs_software_catalog_application INTERFACE)
add_library(azzs::software_catalog_application ALIAS
  azzs_software_catalog_application)

azzs_project_target(
  TARGET azzs_software_catalog_application
  ROLE application
  INTERFACE_LINKS
    azzs::application
    azzs::software_catalog_domain
)

add_library(azzs_software_selection_application INTERFACE)
add_library(azzs::software_selection_application ALIAS
  azzs_software_selection_application)

azzs_project_target(
  TARGET azzs_software_selection_application
  ROLE application
  INTERFACE_LINKS
    azzs::application
    azzs::software_selection_domain
    azzs::software_catalog_application
    azzs::architecture_selection_application
)
