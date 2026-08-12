add_library(azzs_architecture_selection_application STATIC
  src/architecture_selection.cpp
)

add_library(azzs::architecture_selection_application ALIAS
  azzs_architecture_selection_application)

target_include_directories(azzs_architecture_selection_application
  PUBLIC
    include
)

azzs_project_target(
  TARGET azzs_architecture_selection_application
  ROLE application
  PUBLIC_LINKS
    azzs::application
    azzs::architecture_selection_domain
)
