add_executable(azzs_software_catalog_contract
  main.cpp
)

target_compile_definitions(azzs_software_catalog_contract
  PRIVATE
    AZZS_SOFTWARE_CATALOG_PATH="${PROJECT_SOURCE_DIR}/catalog/software-catalog.toml"
)

azzs_project_target(
  TARGET azzs_software_catalog_contract
  ROLE test
  PRIVATE_LINKS
    azzs::software_catalog_file_adapter
    azzs::software_catalog_application
    azzs::software_catalog_domain
    azzs::application
    azzs::application_settings_application
    azzs::test_support
)

azzs_register_test(
  NAME software-catalog.contract
  TARGET azzs_software_catalog_contract
  LABELS headless contract
)
