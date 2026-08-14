find_package(Threads REQUIRED)

add_library(azzs_application STATIC
  src/advanced_view_preferences.cpp
  src/application_update.cpp
  src/bounded_cpu_scheduler.cpp
  src/device_state_store.cpp
  src/emergency_withdrawal_service.cpp
  src/hardware_overview.cpp
  src/operation_occupancy.cpp
  src/restart_resume.cpp
  src/system_settings_apply.cpp
  src/workbench.cpp
)

add_library(azzs::application ALIAS azzs_application)

target_include_directories(azzs_application
  PUBLIC
    include
)

azzs_project_target(
  TARGET azzs_application
  ROLE application
  PUBLIC_LINKS
    azzs::domain
    azzs::settings_catalog_domain
    Threads::Threads
)

add_subdirectory(software-catalog)
add_subdirectory(architecture-selection)
add_subdirectory(software-selection)
add_subdirectory(offline-package-cache)
add_subdirectory(installation-batch-runner)

add_library(azzs_software_optimization_catalog_application STATIC
  src/sogou_optimization.cpp
  src/software_optimization_discovery.cpp
  src/software_optimization_catalog_lifecycle.cpp
)

add_library(
  azzs::software_optimization_catalog_application
  ALIAS azzs_software_optimization_catalog_application
)

target_include_directories(azzs_software_optimization_catalog_application
  PUBLIC
    include
)

azzs_project_target(
  TARGET azzs_software_optimization_catalog_application
  ROLE application
  PUBLIC_LINKS
    azzs::application
    azzs::software_selection_application
    azzs::software_optimization_catalog_domain
)

add_subdirectory(software-optimization-batch-runner)

add_subdirectory(history-and-logs)

add_subdirectory(settings_catalog)
