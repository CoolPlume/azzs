add_executable(azzs_driver_acquisition_contract
  main.cpp
)

azzs_project_target(
  TARGET azzs_driver_acquisition_contract
  ROLE test
  PRIVATE_LINKS
    azzs::application
    azzs::test_support
)

azzs_register_test(
  NAME driver-acquisition.contract
  TARGET azzs_driver_acquisition_contract
  LABELS headless contract
)
