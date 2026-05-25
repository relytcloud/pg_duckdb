# DUCKDB_EXTENSION_CONFIGS file consumed by the root Makefile's duckdb build.
# Bakes the ducklake DuckDB extension (and prerequisites) into libduckdb_bundle.a
# so pg_ducklake.so can LoadStaticExtension<DucklakeExtension>() at runtime
# without a separate .a force-load step.

duckdb_extension_load(json)
duckdb_extension_load(icu)
duckdb_extension_load(ducklake
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/third_party/ducklake
)
