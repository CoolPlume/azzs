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

if(WIN32)
  target_sources(azzs_execution_log_contract PRIVATE
    ../windows-device-data-contract/windows_device_data_environment_test_seam.cpp
  )
  target_include_directories(azzs_execution_log_contract
    PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/../windows-device-data-contract/include"
  )
endif()

azzs_register_test(
  NAME execution-log.contract
  TARGET azzs_execution_log_contract
  LABELS headless contract
)

if(WIN32)
  set(azzs_execution_log_contract_root
    "${CMAKE_BINARY_DIR}/test-fixtures/execution-log")
  file(MAKE_DIRECTORY "${azzs_execution_log_contract_root}")
  set_tests_properties(execution-log.contract PROPERTIES
    ENVIRONMENT
      "AZZS_EXECUTION_LOG_CONTRACT_ROOT=${azzs_execution_log_contract_root}"
  )
endif()
