MODULE_big = pg_vortex
EXTENSION = pg_vortex
DATA = $(wildcard pg_vortex--*.sql)

OBJS = pg_vortex.o

REGRESS = basic
REGRESS_OPTS = --temp-instance=./tmp_check

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
