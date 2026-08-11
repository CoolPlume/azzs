add_library(azzs_windows_adapter STATIC
  src/windows_platform_info.cpp
)

add_library(azzs::windows_adapter ALIAS azzs_windows_adapter)

target_include_directories(azzs_windows_adapter
  PUBLIC
    include
)

target_link_libraries(azzs_windows_adapter
  PUBLIC
    azzs::application
)
