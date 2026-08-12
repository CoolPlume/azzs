add_library(azzs_offline_package_cache_application STATIC
  src/offline_package_cache.cpp
)

add_library(azzs::offline_package_cache_application ALIAS
  azzs_offline_package_cache_application)

target_include_directories(azzs_offline_package_cache_application
  PUBLIC
    include
)

azzs_project_target(
  TARGET azzs_offline_package_cache_application
  ROLE application
  PUBLIC_LINKS
    azzs::application
    azzs::offline_package_cache_domain
    azzs::software_selection_domain
)
