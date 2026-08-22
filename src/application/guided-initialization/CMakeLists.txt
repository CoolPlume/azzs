add_library(azzs_guided_initialization_application STATIC
  src/guided_initialization.cpp
)

add_library(azzs::guided_initialization_application ALIAS
  azzs_guided_initialization_application)

target_include_directories(azzs_guided_initialization_application
  PUBLIC
    include
)

azzs_project_target(
  TARGET azzs_guided_initialization_application
  ROLE application
  PUBLIC_LINKS
    azzs::application
)
