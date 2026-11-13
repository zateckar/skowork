# client/knowledge-graph (add-knowledge-graph). wb_knowledge_core is portable (file kinds, decoding, chunkers, globs,
# vector quantisation, the extraction parser and the index worker's requests) so the fuzz job and the worker build it;
# wb_knowledge is the Windows part (kg.db, the service, retrieval and the tools).
add_library(wb_knowledge_core STATIC core.cpp index_request.cpp analysis.cpp reconcile.cpp)
target_link_libraries(wb_knowledge_core PUBLIC wb_base)
skowork_warnings(wb_knowledge_core)

if(WIN32)
  add_library(wb_knowledge STATIC kg_store.cpp knowledge_service.cpp knowledge_tools.cpp)
  target_link_libraries(wb_knowledge PUBLIC wb_knowledge_core wb_runtime wb_model wb_storage wb_platform wb_base)
  skowork_warnings(wb_knowledge)
endif()
