# tidesdb - LSM engine, transactional and self-concurrent (https://tidesdb.com).
#
# Targets tidesdb 10, whose public surface is the single self-contained header
# <tidesdb/db.h>. Defaults target a system install under /usr/local. Override for
# a custom prefix:
#   make TIDESDB=1 TIDESDB_CFLAGS=-I/opt/tidesdb/include \
#                  TIDESDB_LIBS="-L/opt/tidesdb/lib -ltidesdb"
#
# db.h includes nothing from the package, so a stale tidesdb 9 tree left in the
# same prefix sits beside it harmlessly and is never pulled in.
# TIDESDB_PREFIX points the build at one self-contained install, which is what
# install_tidesdb.sh --prefix produces. This matters more for tidesdb than for
# rocksdb: every tidesdb build carries the same soname, so without the rpath the
# loader picks whichever libtidesdb.so.10 the cache knows about, which may not be
# the one these headers describe.
#   make TIDESDB=1 TIDESDB_PREFIX=/opt/engines/tidesdb-2026-09-02
TIDESDB_PREFIX ?=
ifneq ($(TIDESDB_PREFIX),)
  TIDESDB_CFLAGS ?= -I$(TIDESDB_PREFIX)/include
  TIDESDB_LIBS   ?= -L$(TIDESDB_PREFIX)/lib -Wl,-rpath,$(TIDESDB_PREFIX)/lib -ltidesdb
  ifeq ($(wildcard $(TIDESDB_PREFIX)/include/tidesdb/db.h),)
    $(error no tidesdb/db.h under $(TIDESDB_PREFIX)/include -- not a tidesdb prefix)
  endif
  ifeq ($(wildcard $(TIDESDB_PREFIX)/lib/libtidesdb.*),)
    $(error no libtidesdb under $(TIDESDB_PREFIX)/lib)
  endif
endif

TIDESDB_CFLAGS ?= -I/usr/local/include
TIDESDB_LIBS   ?= -L/usr/local/lib -ltidesdb

# fail early and clearly when the install predates 10, rather than at link time with
# a wall of missing symbols. tidesdb only exposes its version as a string, so probe
# for a config field that 10 introduced when the memtable became database level.
TIDESDB_IS_V10 := $(shell echo 'int main(void){tidesdb_config_t c=tidesdb_default_config();(void)c.memtable_write_buffer_size;return 0;}' | \
  $(CC) $(TIDESDB_CFLAGS) -include tidesdb/db.h -fsyntax-only -x c - >/dev/null 2>&1 && echo 1)
ifneq ($(TIDESDB_IS_V10),1)
  $(error tidesdb 10 headers not found via $(TIDESDB_CFLAGS). <tidesdb/db.h> with a memtable_write_buffer_size config field is required -- point TIDESDB_CFLAGS at a tidesdb 10 install)
endif

SRC    += backends/tidesdb/tidesdb.c
CFLAGS += $(TIDESDB_CFLAGS)
LDLIBS += $(TIDESDB_LIBS)
