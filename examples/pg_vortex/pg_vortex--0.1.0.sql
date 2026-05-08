\echo Use "CREATE EXTENSION pg_vortex" to load this file. \quit

CREATE FUNCTION vortex_version()
RETURNS text
AS 'MODULE_PATHNAME', 'vortex_version'
LANGUAGE C STRICT;
