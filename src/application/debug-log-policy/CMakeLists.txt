add_library(azzs_debug_log_policy_application STATIC
  src/debug_log_policy.cpp
)

add_library(azzs::debug_log_policy_application ALIAS
  azzs_debug_log_policy_application)

target_include_directories(azzs_debug_log_policy_application
  PUBLIC
    include
)

azzs_project_target(
  TARGET azzs_debug_log_policy_application
  ROLE application
)
