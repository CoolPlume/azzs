add_library(azzs_infrastructure_adapter STATIC
  src/local_package_cache_storage.cpp
  src/local_file_log_storage.cpp
  src/state_operation_occupancy_storage.cpp
  src/structured_execution_log.cpp
)

add_library(azzs::infrastructure_adapter ALIAS azzs_infrastructure_adapter)

target_include_directories(azzs_infrastructure_adapter
  PUBLIC
    include
)

set(azzs_infrastructure_private_links)
if(WIN32)
  list(APPEND azzs_infrastructure_private_links advapi32)
endif()

azzs_project_target(
  TARGET azzs_infrastructure_adapter
  ROLE infrastructure_adapter
  PUBLIC_LINKS
    azzs::application
    azzs::offline_package_cache_application
  PRIVATE_LINKS
    ${azzs_infrastructure_private_links}
)

add_subdirectory(software-catalog)

add_library(azzs_software_optimization_catalog_file_adapter STATIC
  src/local_software_optimization_catalog_file.cpp
)

add_library(
  azzs::software_optimization_catalog_file_adapter
  ALIAS azzs_software_optimization_catalog_file_adapter
)

target_include_directories(azzs_software_optimization_catalog_file_adapter
  PUBLIC
    include
)

azzs_project_target(
  TARGET azzs_software_optimization_catalog_file_adapter
  ROLE infrastructure_adapter
  PUBLIC_LINKS
    azzs::software_optimization_catalog_application
)

add_subdirectory(settings_catalog)
