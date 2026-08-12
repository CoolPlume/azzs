add_library(azzs_offline_package_cache_domain STATIC
  src/offline_package_cache.cpp
)

add_library(azzs::offline_package_cache_domain ALIAS
  azzs_offline_package_cache_domain)

target_include_directories(azzs_offline_package_cache_domain
  PUBLIC
    include
)

azzs_project_target(
  TARGET azzs_offline_package_cache_domain
  ROLE domain
)
