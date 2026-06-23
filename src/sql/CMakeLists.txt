# sql: the tokenizer, parser, AST, and logical plan. It depends only on core-
# level types (absl for status) and knows nothing about crypto, storage, or
# backends. The planner (the next layer) turns a logical plan into a backend
# selection by reading the schema and capabilities.
add_library(opaquedb_sql
  ast.cpp
  tokenizer.cpp
  parser.cpp
  ddl.cpp
  logical_plan.cpp)

target_include_directories(opaquedb_sql PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

# core::Schema appears in the DDL parser's public API, so opaquedb_core is
# PUBLIC. The query path stays unaware of this; it only sees the schema.
target_link_libraries(opaquedb_sql
  PUBLIC
    opaquedb_core
    absl::status
    absl::statusor
  PRIVATE
    opaquedb_compile_options
    absl::strings)
