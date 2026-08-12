add_library(azzs_architecture_selection_domain STATIC
  src/architecture_selection.cpp
)

add_library(azzs::architecture_selection_domain ALIAS
  azzs_architecture_selection_domain)

target_include_directories(azzs_architecture_selection_domain
  PUBLIC
    include
    "${PROJECT_SOURCE_DIR}/src/domain/include"
)

azzs_project_target(
  TARGET azzs_architecture_selection_domain
  ROLE domain
)
