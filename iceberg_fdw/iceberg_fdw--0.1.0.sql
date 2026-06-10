/* iceberg_fdw extension */

CREATE FUNCTION iceberg_fdw_handler()
RETURNS fdw_handler
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION iceberg_fdw_validator(text[], oid)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FOREIGN DATA WRAPPER iceberg_fdw
  HANDLER iceberg_fdw_handler
  VALIDATOR iceberg_fdw_validator;
