add_executable(azzs_sidebar_width_contract
  main.cpp
)

azzs_project_target(
  TARGET azzs_sidebar_width_contract
  ROLE test
  PRIVATE_LINKS
    azzs::application
)

azzs_register_test(
  NAME sidebar-width.contract
  TARGET azzs_sidebar_width_contract
  LABELS headless contract
)
