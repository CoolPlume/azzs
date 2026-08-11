if(WIN32)
  add_library(azzs_windows_adapter STATIC
    src/windows_platform_info.cpp
  )

  add_library(azzs::windows_adapter ALIAS azzs_windows_adapter)

  target_include_directories(azzs_windows_adapter
    PUBLIC
      include
  )
endif()

azzs_project_target(
  TARGET azzs_windows_adapter
  ROLE platform_adapter
  OPTIONAL
  PUBLIC_LINKS
    azzs::application
)
