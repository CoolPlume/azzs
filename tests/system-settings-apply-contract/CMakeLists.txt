add_executable(azzs_system_settings_apply_contract
  main.cpp
)

azzs_project_target(
  TARGET azzs_system_settings_apply_contract
  ROLE test
  PRIVATE_LINKS
    azzs::application
    azzs::infrastructure_adapter
    azzs::test_support
)

azzs_register_test(
  NAME system-settings-apply.contract
  TARGET azzs_system_settings_apply_contract
  LABELS headless contract
)
