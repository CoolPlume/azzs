add_executable(azzs_settings_catalog_contract
  main.cpp
)

azzs_project_target(
  TARGET azzs_settings_catalog_contract
  ROLE test
  PRIVATE_LINKS
    azzs::settings_catalog_file_adapter
    azzs::test_support
)

azzs_register_test(
  NAME settings-catalog.contract
  TARGET azzs_settings_catalog_contract
  LABELS headless contract
)
