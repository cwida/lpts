cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME lpts)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})
include_directories(src/include)
include_directories(third_party/ducklake/src/include)
include_directories(third_party/duckdb-delta/src/include)
include_directories("${CMAKE_BINARY_DIR}/codegen/include")

set(EXTENSION_SOURCES
    src/lpts_extension.cpp
    src/cte_nodes.cpp
    src/lpts_helpers.cpp
    src/lpts_ast.cpp
    src/lpts_ast_renderer.cpp
    src/lpts_pipeline.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

if(TARGET delta_kernel)
    add_dependencies(${EXTENSION_NAME} delta_kernel)
    add_dependencies(${LOADABLE_EXTENSION_NAME} delta_kernel)
endif()

target_compile_definitions(${EXTENSION_NAME} PRIVATE DEFINE_DEFAULT_ENGINE DEFINE_DEFAULT_ENGINE_BASE)
target_compile_definitions(${LOADABLE_EXTENSION_NAME} PRIVATE DEFINE_DEFAULT_ENGINE DEFINE_DEFAULT_ENGINE_BASE)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

set(LPTS_BENCHMARK_LINK_LIBS
    duckdb_static
    duckdb_generated_extension_loader
    ${EXTENSION_NAME}
    tpch_extension
    core_functions_extension
    parquet_extension
    delta_extension
    duckdb_static)

if(DUCKDB_EXTENSION_JEMALLOC_SHOULD_LINK)
    list(APPEND LPTS_BENCHMARK_LINK_LIBS jemalloc_extension)
endif()

if(NOT EMSCRIPTEN AND NOT WIN32)
    add_executable(lpts_sqlstorm_benchmark benchmark/sqlstorm/lpts_sqlstorm_benchmark.cpp)
    target_link_libraries(lpts_sqlstorm_benchmark ${LPTS_BENCHMARK_LINK_LIBS})
endif()
