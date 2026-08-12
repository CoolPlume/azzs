add_library(azzs_architecture_selection_application INTERFACE)
add_library(azzs::architecture_selection_application ALIAS
  azzs_architecture_selection_application)

azzs_project_target(
  TARGET azzs_architecture_selection_application
  ROLE application
  INTERFACE_LINKS
    azzs::application
    azzs::architecture_selection_domain
)
