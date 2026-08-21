add_library(azzs_installation_batch_runner_application STATIC
  src/installation_batch.cpp
)

add_library(azzs::installation_batch_runner_application ALIAS
  azzs_installation_batch_runner_application)

target_include_directories(azzs_installation_batch_runner_application
  PUBLIC
    include
)

azzs_project_target(
  TARGET azzs_installation_batch_runner_application
  ROLE application
  PUBLIC_LINKS
    azzs::application
    azzs::installation_batch_runner_domain
    azzs::software_catalog_application
    azzs::software_selection_application
    azzs::offline_package_cache_application
)
