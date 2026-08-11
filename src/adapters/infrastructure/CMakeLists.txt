add_library(azzs_infrastructure_adapter STATIC
  src/local_file_log_storage.cpp
  src/state_operation_occupancy_storage.cpp
  src/structured_execution_log.cpp
)

add_library(azzs::infrastructure_adapter ALIAS azzs_infrastructure_adapter)

target_include_directories(azzs_infrastructure_adapter
  PUBLIC
    include
)

target_link_libraries(azzs_infrastructure_adapter
  PUBLIC
    azzs::application
)

if(WIN32)
  target_link_libraries(azzs_infrastructure_adapter
    PRIVATE
      advapi32
  )
endif()
