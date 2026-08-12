add_executable(azzs_offline_package_cache_contract
  main.cpp
)

azzs_project_target(
  TARGET azzs_offline_package_cache_contract
  ROLE test
  PRIVATE_LINKS
    azzs::offline_package_cache_application
    azzs::offline_package_cache_domain
)

azzs_register_test(
  NAME offline-package-cache.contract
  TARGET azzs_offline_package_cache_contract
  LABELS headless contract
)
