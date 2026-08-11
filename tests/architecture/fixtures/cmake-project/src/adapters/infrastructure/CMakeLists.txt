add_library(azzs_infrastructure_adapter INTERFACE)
add_library(azzs::infrastructure_adapter ALIAS
  azzs_infrastructure_adapter)

azzs_project_target(
  TARGET azzs_infrastructure_adapter
  ROLE infrastructure_adapter
  INTERFACE_LINKS azzs::application
)
