MODULE_big = iceberg_fdw
OBJS = src/iceberg_fdw.o src/iceberg_ddl.o src/catalog_adapter.o src/type_adapter.o src/operator_adapter.o src/sdk_scan_adapter.o src/delta_scan_adapter.o src/metadata_txn.o

EXTENSION = iceberg_fdw
DATA = iceberg_fdw--0.1.0.sql
REGRESS = managed_ddl managed_scan

PG_CPPFLAGS += -I$(srcdir)/include
CXXFLAGS += -fPIC

OPENGAUSS_SRC_INCLUDE ?= $(firstword $(wildcard ../openGauss-server/src/include /tmp/openGauss-src-include))
ifneq ($(OPENGAUSS_SRC_INCLUDE),)
PG_CPPFLAGS += -I$(OPENGAUSS_SRC_INCLUDE)
endif

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

exclude_option = -fPIE
override CPPFLAGS := $(filter-out $(exclude_option),$(CPPFLAGS))
override CFLAGS := $(filter-out $(exclude_option),$(CFLAGS))
override CXXFLAGS := $(filter-out $(exclude_option),$(CXXFLAGS))
