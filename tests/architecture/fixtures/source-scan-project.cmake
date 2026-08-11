set(AZZS_GRAPH_NODES
  "azzs_domain|domain"
  "azzs_application|application"
  "azzs_windows_adapter|platform_adapter"
)
set(AZZS_GRAPH_EDGES
  "azzs_application|azzs_domain"
  "azzs_windows_adapter|azzs_application"
)
get_filename_component(AZZS_GRAPH_SOURCE_ROOT
  "${CMAKE_CURRENT_LIST_DIR}/source-scan-project" ABSOLUTE)
set(AZZS_GRAPH_SCAN_PROJECT TRUE)
