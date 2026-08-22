add_executable(azzs_core_smoke
  main.cpp
)

azzs_project_target(
  TARGET azzs_core_smoke
  ROLE test
  PRIVATE_LINKS
    azzs::application
    azzs::test_support
)

azzs_register_test(
  NAME core.smoke
  TARGET azzs_core_smoke
  LABELS headless contract
)
