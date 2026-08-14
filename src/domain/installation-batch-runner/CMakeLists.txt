add_library(azzs_installation_batch_runner_domain STATIC
  src/installation_batch.cpp
)

add_library(azzs::installation_batch_runner_domain ALIAS
  azzs_installation_batch_runner_domain)

target_include_directories(azzs_installation_batch_runner_domain
  PUBLIC
    include
)

azzs_project_target(
  TARGET azzs_installation_batch_runner_domain
  ROLE domain
  PUBLIC_LINKS
    azzs::software_catalog_domain
    azzs::software_selection_domain
    azzs::offline_package_cache_domain
)
