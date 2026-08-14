add_library(azzs_application_settings_application STATIC
  src/application_settings.cpp
)

add_library(azzs::application_settings_application ALIAS
  azzs_application_settings_application)

target_include_directories(azzs_application_settings_application
  PUBLIC
    include
)

azzs_project_target(
  TARGET azzs_application_settings_application
  ROLE application
  PUBLIC_LINKS
    azzs::application
    azzs::architecture_selection_application
    azzs::offline_package_cache_application
    azzs::history_and_logs_application
    azzs::settings_catalog_lifecycle
    azzs::software_catalog_application
    azzs::software_selection_application
    azzs::software_optimization_catalog_application
)
