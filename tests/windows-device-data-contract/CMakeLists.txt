add_executable(azzs_windows_device_data_contract
  main.cpp
)

azzs_project_target(
  TARGET azzs_windows_device_data_contract
  ROLE test
  PRIVATE_LINKS
    azzs::infrastructure_adapter
    azzs::windows_adapter
    azzs::test_support
)

azzs_register_test(
  NAME windows-device-data.contract
  TARGET azzs_windows_device_data_contract
  LABELS headless contract
)
