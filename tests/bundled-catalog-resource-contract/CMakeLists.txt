add_executable(azzs_bundled_catalog_resource_contract
  main.cpp
)

azzs_project_target(
  TARGET azzs_bundled_catalog_resource_contract
  ROLE test
  PRIVATE_LINKS
    azzs::windows_adapter
    azzs::software_catalog_file_adapter
)

azzs_register_test(
  NAME bundled-catalog-resource.contract
  TARGET azzs_bundled_catalog_resource_contract
  LABELS headless contract
)
