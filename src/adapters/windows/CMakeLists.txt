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
  set(AZZS_CPPWINRT_VERSION "3.0.260715.1")
  if(DEFINED ENV{NUGET_PACKAGES})
    set(azzs_nuget_packages "$ENV{NUGET_PACKAGES}")
  else()
    set(azzs_nuget_packages "$ENV{USERPROFILE}/.nuget/packages")
  endif()

  set(azzs_cppwinrt_executable
    "${azzs_nuget_packages}/microsoft.windows.cppwinrt/${AZZS_CPPWINRT_VERSION}/bin/cppwinrt.exe")
  get_filename_component(azzs_windows_kits_root
    "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots;KitsRoot10]"
    ABSOLUTE)
  if(DEFINED CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION AND
     NOT "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}" STREQUAL "")
    set(azzs_windows_metadata_version
      "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
  else()
    set(azzs_windows_metadata_version "${CMAKE_SYSTEM_VERSION}")
  endif()
  set(azzs_windows_metadata
    "${azzs_windows_kits_root}/UnionMetadata/${azzs_windows_metadata_version}/Windows.winmd")
  set(azzs_cppwinrt_generated_directory
    "${CMAKE_CURRENT_BINARY_DIR}/cppwinrt")
  set(azzs_cppwinrt_base_header
    "${azzs_cppwinrt_generated_directory}/winrt/base.h")

  if(NOT EXISTS "${azzs_cppwinrt_executable}")
    message(FATAL_ERROR
      "Microsoft.Windows.CppWinRT ${AZZS_CPPWINRT_VERSION} is required at ${azzs_cppwinrt_executable}")
  endif()
  if(NOT EXISTS "${azzs_windows_metadata}")
    message(FATAL_ERROR
      "Windows metadata is required at ${azzs_windows_metadata}")
  endif()

  add_custom_command(
    OUTPUT "${azzs_cppwinrt_base_header}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${azzs_cppwinrt_generated_directory}"
    COMMAND "${azzs_cppwinrt_executable}" -in "${azzs_windows_metadata}" -out "${azzs_cppwinrt_generated_directory}"
    DEPENDS "${azzs_cppwinrt_executable}" "${azzs_windows_metadata}"
    VERBATIM
  )
  add_custom_target(azzs_cppwinrt_projection
    DEPENDS "${azzs_cppwinrt_base_header}")
  file(MAKE_DIRECTORY "${azzs_cppwinrt_generated_directory}")
  add_library(azzs_cppwinrt_headers INTERFACE IMPORTED GLOBAL)
  set_target_properties(azzs_cppwinrt_headers PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${azzs_cppwinrt_generated_directory}")

  add_library(azzs_windows_adapter STATIC
    src/bundled_catalog_resource_reader.cpp
    src/windows_application_update_platform.cpp
    src/windows_controlled_acquisition.cpp
    src/windows_device_data_environment.cpp
    src/windows_driver_acquisition.cpp
    src/windows_emergency_withdrawal_notice_source.cpp
    src/windows_external_address_launcher.cpp
    src/windows_hardware_observer.cpp
    src/windows_internet_availability_observer.cpp
    src/windows_installation_batch_adapters.cpp
    src/windows_lease_token_source.cpp
    src/windows_platform_info.cpp
    src/windows_restart_resume_registration.cpp
    src/windows_state_file_system.cpp
    src/windows_sogou_optimization_adapter.cpp
    src/windows_system_settings_adapter.cpp
    src/windows_view_preferences.cpp
  )

  add_library(azzs::windows_adapter ALIAS azzs_windows_adapter)

  target_include_directories(azzs_windows_adapter
    PUBLIC
      include
  )
  add_dependencies(azzs_windows_adapter azzs_cppwinrt_projection)

  target_compile_definitions(azzs_windows_adapter
    PRIVATE
      AZZS_APPLICATION_VERSION="${AZZS_APPLICATION_VERSION}"
      AZZS_APPLICATION_RELEASE_CHANNEL="${AZZS_APPLICATION_RELEASE_CHANNEL}"
      AZZS_EMERGENCY_WITHDRAWAL_NOTICE_ENDPOINT="${AZZS_EMERGENCY_WITHDRAWAL_NOTICE_ENDPOINT}"
  )
endif()

azzs_project_target(
  TARGET azzs_windows_adapter
  ROLE platform_adapter
  OPTIONAL
  PUBLIC_LINKS
    azzs::application
    azzs::application_settings_application
    azzs::installation_batch_runner_application
    azzs::software_optimization_batch_runner_application
    azzs::offline_package_cache_application
    azzs::software_selection_application
    azzs::software_optimization_catalog_application
  PRIVATE_LINKS
    azzs_cppwinrt_headers
    advapi32
    bcrypt
    ole32
    oleaut32
    rpcrt4
    shell32
    user32
    windowsapp
    wbemuuid
    winhttp
    wtsapi32
)
