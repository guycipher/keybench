# rocksdb - real persistent engine (self-concurrent).
#
# Defaults target the system v11 install. -I/usr/include pins the v11 header
# ahead of any /usr/local/include shadow, and -l:librocksdb.so.11 pins the
# matching lib. Override for a custom install, e.g.:
#   make ROCKSDB=1 ROCKSDB_CFLAGS=-I/opt/rocksdb/include \
#                  ROCKSDB_LIBS="-L/opt/rocksdb/lib -lrocksdb" \
#                  ROCKSDB_VERSION_H=/opt/rocksdb/include/rocksdb/version.h
ROCKSDB_CFLAGS    ?= -I/usr/include
ROCKSDB_LIBS      ?= -l:librocksdb.so.11
ROCKSDB_VERSION_H ?= /usr/include/rocksdb/version.h

# <rocksdb/version.h> is a C++ header, so extract the version macros at build
# time and pass them as a string define (used by rdb_version()).
ROCKSDB_VER := $(shell awk '/define ROCKSDB_MAJOR/{a=$$3}/define ROCKSDB_MINOR/{b=$$3}/define ROCKSDB_PATCH/{c=$$3}END{if(a!="")print a"."b"."c}' $(ROCKSDB_VERSION_H) 2>/dev/null)

SRC    += backends/rocksdb/rocksdb.c
CFLAGS += $(ROCKSDB_CFLAGS)
ifneq ($(ROCKSDB_VER),)
  CFLAGS += -DKB_ROCKSDB_VERSION='"$(ROCKSDB_VER)"'
endif
LDLIBS += $(ROCKSDB_LIBS)
