set(AZZS_EMERGENCY_WITHDRAWAL_NOTICE_ENDPOINT
  "https://raw.githubusercontent.com/CoolPlume/azzs/main/release/emergency-withdrawal-notice.txt"
  CACHE INTERNAL
  "Fixed project HTTPS endpoint embedded in the Windows emergency withdrawal notice adapter"
  FORCE)

if("${AZZS_EMERGENCY_WITHDRAWAL_NOTICE_ENDPOINT}" STREQUAL "" OR
   NOT "${AZZS_EMERGENCY_WITHDRAWAL_NOTICE_ENDPOINT}" MATCHES "^https://")
  message(FATAL_ERROR
    "AZZS_EMERGENCY_WITHDRAWAL_NOTICE_ENDPOINT must be a non-empty fixed HTTPS URL")
endif()

if(WIN32)
  add_library(azzs_windows_adapter STATIC
    src/windows_application_update_platform.cpp
    src/windows_device_data_environment.cpp
    src/windows_emergency_withdrawal_notice_source.cpp
    src/windows_external_address_launcher.cpp
    src/windows_hardware_observer.cpp
    src/windows_lease_token_source.cpp
    src/windows_platform_info.cpp
    src/windows_state_file_system.cpp
    src/windows_sogou_optimization_adapter.cpp
  )

  add_library(azzs::windows_adapter ALIAS azzs_windows_adapter)

  target_include_directories(azzs_windows_adapter
    PUBLIC
      include
  )

  target_compile_definitions(azzs_windows_adapter
    PRIVATE
      AZZS_APPLICATION_VERSION="${PROJECT_VERSION}"
      AZZS_EMERGENCY_WITHDRAWAL_NOTICE_ENDPOINT="${AZZS_EMERGENCY_WITHDRAWAL_NOTICE_ENDPOINT}"
  )
endif()

azzs_project_target(
  TARGET azzs_windows_adapter
  ROLE platform_adapter
  OPTIONAL
  PUBLIC_LINKS
    azzs::application
    azzs::software_selection_application
    azzs::software_optimization_catalog_application
  PRIVATE_LINKS
    advapi32
    ole32
    oleaut32
    rpcrt4
    shell32
    wbemuuid
    winhttp
    wtsapi32
)
