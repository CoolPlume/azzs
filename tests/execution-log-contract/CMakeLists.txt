add_executable(azzs_execution_log_contract
  main.cpp
)

set(azzs_execution_log_contract_private_links
  azzs::infrastructure_adapter
  azzs::test_support
)
if(WIN32)
  list(APPEND azzs_execution_log_contract_private_links
    azzs::windows_adapter)
endif()

azzs_project_target(
  TARGET azzs_execution_log_contract
  ROLE test
  PRIVATE_LINKS
    ${azzs_execution_log_contract_private_links}
)

azzs_register_test(
  NAME execution-log.contract
  TARGET azzs_execution_log_contract
  LABELS headless contract
)
