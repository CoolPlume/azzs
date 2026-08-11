add_executable(azzs_device_state_contract
  main.cpp
)

azzs_project_target(
  TARGET azzs_device_state_contract
  ROLE test
  PRIVATE_LINKS
    azzs::application
    azzs::test_support
)

azzs_register_test(
  NAME device-state.contract
  TARGET azzs_device_state_contract
  LABELS headless contract
)
