add_library(azzs_history_and_logs_application STATIC
  src/history_and_logs.cpp
)

add_library(azzs::history_and_logs_application ALIAS azzs_history_and_logs_application)

target_include_directories(azzs_history_and_logs_application
  PUBLIC
    include
)

azzs_project_target(
  TARGET azzs_history_and_logs_application
  ROLE application
  PUBLIC_LINKS
    azzs::application
    azzs::debug_log_policy_application
    azzs::installation_batch_runner_application
    azzs::software_catalog_application
    azzs::software_optimization_batch_runner_application
    azzs::software_selection_application
)
