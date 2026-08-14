add_library(azzs_software_optimization_batch_runner_application STATIC
  src/software_optimization_batch.cpp
)

add_library(azzs::software_optimization_batch_runner_application ALIAS
  azzs_software_optimization_batch_runner_application)

target_include_directories(azzs_software_optimization_batch_runner_application
  PUBLIC
    include
)

azzs_project_target(
  TARGET azzs_software_optimization_batch_runner_application
  ROLE application
  PUBLIC_LINKS
    azzs::application
    azzs::software_optimization_batch_runner_domain
    azzs::software_optimization_catalog_application
)
