add_library(azzs_settings_catalog_lifecycle STATIC
  src/settings_catalog_lifecycle.cpp
)

add_library(azzs::settings_catalog_lifecycle ALIAS
  azzs_settings_catalog_lifecycle)

target_include_directories(azzs_settings_catalog_lifecycle
  PUBLIC
    include
)

azzs_project_target(
  TARGET azzs_settings_catalog_lifecycle
  ROLE application
  PUBLIC_LINKS
    azzs::settings_catalog_domain
    azzs::application
)
