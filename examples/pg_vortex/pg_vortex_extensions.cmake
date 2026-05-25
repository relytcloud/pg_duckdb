duckdb_extension_load(json)
duckdb_extension_load(icu)
duckdb_extension_load(httpfs
    GIT_URL https://github.com/duckdb/duckdb-httpfs
    GIT_TAG 9de3296f40ed03e8e063394887f0d6a46144e847
)
duckdb_extension_load(vortex
    GIT_URL https://github.com/vortex-data/duckdb-vortex
    GIT_TAG b5fc172130020adcb28b4fe78665cf4ed0069ad0
    SUBMODULES "vortex"
)
