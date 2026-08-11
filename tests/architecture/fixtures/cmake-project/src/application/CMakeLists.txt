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
