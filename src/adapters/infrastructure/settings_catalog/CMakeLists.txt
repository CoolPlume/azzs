add_library(azzs_settings_catalog_file_adapter STATIC
  src/settings_catalog_file_adapter.cpp
)

add_library(azzs::settings_catalog_file_adapter ALIAS
  azzs_settings_catalog_file_adapter)

target_include_directories(azzs_settings_catalog_file_adapter
  PUBLIC
    include
)

azzs_project_target(
  TARGET azzs_settings_catalog_file_adapter
  ROLE infrastructure_adapter
  PUBLIC_LINKS
    azzs::settings_catalog_lifecycle
)
