add_executable(azzs_application_update_contract
  main.cpp
)

azzs_project_target(
  TARGET azzs_application_update_contract
  ROLE test
  PRIVATE_LINKS
    azzs::infrastructure_adapter
    azzs::application
    azzs::test_support
)

azzs_register_test(
  NAME application-update.contract
  TARGET azzs_application_update_contract
  LABELS headless contract
)
