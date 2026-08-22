add_library(azzs_software_catalog_application STATIC
  src/software_catalog_lifecycle.cpp
)

add_library(azzs::software_catalog_application ALIAS
  azzs_software_catalog_application)

target_include_directories(azzs_software_catalog_application
  PUBLIC
    include
)

target_compile_definitions(azzs_software_catalog_application
  PRIVATE
    AZZS_APPLICATION_VERSION="${AZZS_APPLICATION_VERSION}"
    AZZS_APPLICATION_RELEASE_CHANNEL="${AZZS_APPLICATION_RELEASE_CHANNEL}"
)

azzs_project_target(
  TARGET azzs_software_catalog_application
  ROLE application
  PUBLIC_LINKS
    azzs::application
    azzs::software_catalog_domain
)
