MODULE_big = iceberg_fdw
OBJS = src/iceberg_fdw.o

EXTENSION = iceberg_fdw
DATA = iceberg_fdw--0.1.0.sql

PG_CPPFLAGS += -I$(srcdir)/include

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
