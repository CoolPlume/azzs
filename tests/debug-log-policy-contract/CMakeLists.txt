add_executable(azzs_debug_log_policy_contract
  main.cpp
)

azzs_project_target(
  TARGET azzs_debug_log_policy_contract
  ROLE test
  PRIVATE_LINKS
    azzs::debug_log_policy_application
)

azzs_register_test(
  NAME debug-log-policy.contract
  TARGET azzs_debug_log_policy_contract
  LABELS headless contract
)
