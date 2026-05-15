\echo Use "CREATE EXTENSION pg_vortex" to load this file. \quit

CREATE FUNCTION vortex_version()
RETURNS text
AS 'MODULE_PATHNAME', 'pg_vortex_version'
LANGUAGE C STRICT;

-- read_vortex is a marker UDF: all calls are intercepted by pg_vortex's
-- planner_hook and offloaded to DuckDB's read_vortex table function. The
-- C symbol is a stub that ereports if reached directly (i.e., when the
-- offload check missed).
CREATE FUNCTION read_vortex(path text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'duckdb_only_function'
LANGUAGE C STRICT;
