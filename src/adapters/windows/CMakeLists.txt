add_library(azzs_windows_adapter STATIC
  src/windows_device_data_environment.cpp
  src/windows_lease_token_source.cpp
  src/windows_platform_info.cpp
  src/windows_state_file_system.cpp
)

add_library(azzs::windows_adapter ALIAS azzs_windows_adapter)

target_include_directories(azzs_windows_adapter
  PUBLIC
    include
)

target_link_libraries(azzs_windows_adapter
  PUBLIC
    azzs::application
  PRIVATE
    advapi32
    ole32
    rpcrt4
    shell32
    wtsapi32
)
