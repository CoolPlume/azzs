add_executable(azzs_execution_log_contract
  main.cpp
)

target_include_directories(azzs_execution_log_contract
  PRIVATE
    ../support/include
)

target_link_libraries(azzs_execution_log_contract
  PRIVATE
    azzs::infrastructure_adapter
)

if(WIN32)
  target_link_libraries(azzs_execution_log_contract
    PRIVATE
      azzs::windows_adapter
  )
endif()

add_test(
  NAME execution-log.contract
  COMMAND azzs_execution_log_contract
)
