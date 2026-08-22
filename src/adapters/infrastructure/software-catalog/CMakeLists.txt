add_library(azzs_software_catalog_file_adapter STATIC
  src/local_software_catalog_file.cpp
  src/toml_software_catalog.cpp
)

add_library(azzs::software_catalog_file_adapter ALIAS
  azzs_software_catalog_file_adapter)

target_include_directories(azzs_software_catalog_file_adapter
  PUBLIC
    include
)

azzs_project_target(
  TARGET azzs_software_catalog_file_adapter
  ROLE infrastructure_adapter
  PUBLIC_LINKS
    azzs::software_catalog_application
    azzs::software_catalog_domain
)
