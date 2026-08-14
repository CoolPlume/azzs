add_executable(azzs_application_settings_contract
  main.cpp
)

azzs_project_target(
  TARGET azzs_application_settings_contract
  ROLE test
  PRIVATE_LINKS
    azzs::application_settings_application
)

azzs_register_test(
  NAME application-settings.contract
  TARGET azzs_application_settings_contract
  LABELS headless contract
)
