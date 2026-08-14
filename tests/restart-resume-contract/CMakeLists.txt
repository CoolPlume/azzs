add_executable(azzs_restart_resume_contract
  main.cpp
)

azzs_project_target(
  TARGET azzs_restart_resume_contract
  ROLE test
  PRIVATE_LINKS
    azzs::application
    azzs::test_support
)

azzs_register_test(
  NAME restart-resume.contract
  TARGET azzs_restart_resume_contract
  LABELS headless contract
)
