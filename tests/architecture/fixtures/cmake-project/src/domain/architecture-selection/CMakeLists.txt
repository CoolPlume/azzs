add_library(azzs_architecture_selection_domain INTERFACE)
add_library(azzs::architecture_selection_domain ALIAS
  azzs_architecture_selection_domain)

azzs_project_target(
  TARGET azzs_architecture_selection_domain
  ROLE domain
)
