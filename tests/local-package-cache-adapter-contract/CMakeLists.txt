add_executable(azzs_local_package_cache_adapter_contract
  main.cpp
)

azzs_project_target(
  TARGET azzs_local_package_cache_adapter_contract
  ROLE test
  PRIVATE_LINKS
    azzs::infrastructure_adapter
    azzs::offline_package_cache_application
    azzs::offline_package_cache_domain
)

azzs_register_test(
  NAME local-package-cache-adapter.contract
  TARGET azzs_local_package_cache_adapter_contract
  LABELS headless contract
)
