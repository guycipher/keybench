# tidesdb - LSM engine, transactional and self-concurrent (https://tidesdb.com).
#
# Defaults target a system install under /usr/local. Override for a custom prefix:
#   make TIDESDB=1 TIDESDB_CFLAGS=-I/opt/tidesdb/include \
#                  TIDESDB_LIBS="-L/opt/tidesdb/lib -ltidesdb"
TIDESDB_CFLAGS ?= -I/usr/local/include
TIDESDB_LIBS   ?= -L/usr/local/lib -ltidesdb

SRC    += backends/tidesdb/tidesdb.c
CFLAGS += $(TIDESDB_CFLAGS)
LDLIBS += $(TIDESDB_LIBS)
