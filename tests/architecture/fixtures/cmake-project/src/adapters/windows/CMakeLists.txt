add_library(azzs_windows_adapter INTERFACE)
add_library(azzs::windows_adapter ALIAS azzs_windows_adapter)

target_include_directories(azzs_windows_adapter INTERFACE include)

azzs_project_target(
  TARGET azzs_windows_adapter
  ROLE platform_adapter
  INTERFACE_LINKS azzs::application
)
