add_executable(azzs_history_and_logs_contract
  main.cpp
)

azzs_project_target(
  TARGET azzs_history_and_logs_contract
  ROLE test
  PRIVATE_LINKS
    azzs::history_and_logs_application
)

azzs_register_test(
  NAME history-and-logs.contract
  TARGET azzs_history_and_logs_contract
  LABELS headless contract
)
