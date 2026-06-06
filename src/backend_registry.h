#ifndef BACKEND_REGISTRY_H
#define BACKEND_REGISTRY_H

#include "bench.h"

typedef struct
{
    const char *name;
    backend_ctor open;
    int persistent;
} backend_entry;

void backend_register(const char *name, backend_ctor open, int persistent);

const backend_entry *backend_find(const char *name);

int backend_count(void);
const backend_entry *backend_at(int i);

/* PERSISTENT is 1 for an engine that stores data on disk and so requires a
   --data-dir, 0 for an in memory engine that needs none. */
#define KV_REGISTER_BACKEND(NAME, OPEN, PERSISTENT)                    \
    __attribute__((constructor)) static void kv_register__##OPEN(void) \
    {                                                                  \
        backend_register((NAME), (OPEN), (PERSISTENT));                \
    }

#endif
