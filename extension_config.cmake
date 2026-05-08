# This file is included by DuckDB's build system. It specifies which extension to load

# Any extra extensions that should be built
duckdb_extension_load(tpch)
duckdb_extension_load(parquet)
duckdb_extension_load(delta
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/third_party/duckdb-delta
    LOAD_TESTS
)

# Extension from this repo
duckdb_extension_load(lpts
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)
