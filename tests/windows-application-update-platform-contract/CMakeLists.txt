if(NOT WIN32)
  return()
endif()

find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)

azzs_register_test(
  NAME windows-application-update-platform.contract
  LABELS contract
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/check_windows_application_update_platform.py"
    --repository-root
    "${PROJECT_SOURCE_DIR}"
)

add_executable(azzs_windows_application_update_platform_contract
  main.cpp
)

target_compile_definitions(azzs_windows_application_update_platform_contract
  PRIVATE
    AZZS_APPLICATION_UPDATE_EXPECTED_ENDPOINT="${AZZS_APPLICATION_UPDATE_ENDPOINT}"
)

azzs_project_target(
  TARGET azzs_windows_application_update_platform_contract
  ROLE test
  PRIVATE_LINKS
    azzs::windows_adapter
)

azzs_register_test(
  NAME windows-application-update-platform-runtime.contract
  TARGET azzs_windows_application_update_platform_contract
  LABELS headless contract
)
