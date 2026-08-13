add_executable(azzs_hardware_overview_contract
  main.cpp
)

azzs_project_target(
  TARGET azzs_hardware_overview_contract
  ROLE test
  PRIVATE_LINKS
    azzs::application
    azzs::test_support
)

azzs_register_test(
  NAME hardware-overview.contract
  TARGET azzs_hardware_overview_contract
  LABELS headless contract
)
