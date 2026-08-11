add_library(azzs_settings_catalog_domain STATIC
  src/settings_catalog.cpp
)

add_library(azzs::settings_catalog_domain ALIAS
  azzs_settings_catalog_domain)

target_include_directories(azzs_settings_catalog_domain
  PUBLIC
    include
)

azzs_project_target(
  TARGET azzs_settings_catalog_domain
  ROLE domain
)
