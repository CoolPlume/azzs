set(AZZS_GRAPH_NODES
  "azzs_domain|domain"
  "azzs_application|application"
  "azzs_architecture_selection_application|application"
  "azzs_software_catalog_application|application"
  "azzs_software_selection_application|application"
  "azzs_windows_adapter|platform_adapter"
  "azzs_infrastructure_adapter|infrastructure_adapter"
  "azzs_architecture_selection_domain|domain"
  "azzs_software_catalog_domain|domain"
  "azzs_software_selection_domain|domain"
)
set(AZZS_GRAPH_EDGES
  "azzs_application|azzs_domain"
  "azzs_architecture_selection_application|azzs_application"
  "azzs_architecture_selection_application|azzs_architecture_selection_domain"
  "azzs_software_catalog_application|azzs_application"
  "azzs_software_catalog_application|azzs_software_catalog_domain"
  "azzs_software_selection_application|azzs_application"
  "azzs_software_selection_application|azzs_software_catalog_application"
  "azzs_software_selection_application|azzs_software_selection_domain"
  "azzs_windows_adapter|azzs_application"
  "azzs_infrastructure_adapter|azzs_application"
)
get_filename_component(AZZS_GRAPH_SOURCE_ROOT
  "${CMAKE_CURRENT_LIST_DIR}/msbuild-compile-input" ABSOLUTE)
set(AZZS_GRAPH_SCAN_PROJECT TRUE)
