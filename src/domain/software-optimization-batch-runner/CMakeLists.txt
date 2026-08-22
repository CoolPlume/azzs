add_library(azzs_software_optimization_batch_runner_domain STATIC
  src/software_optimization_batch.cpp
)

add_library(azzs::software_optimization_batch_runner_domain ALIAS
  azzs_software_optimization_batch_runner_domain)

target_include_directories(azzs_software_optimization_batch_runner_domain
  PUBLIC
    include
)

azzs_project_target(
  TARGET azzs_software_optimization_batch_runner_domain
  ROLE domain
  PUBLIC_LINKS
    azzs::software_optimization_catalog_domain
)
