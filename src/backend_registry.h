#ifndef BACKEND_REGISTRY_H
#define BACKEND_REGISTRY_H

#include "bench.h"

typedef struct
{
    const char *name;
    backend_ctor open;
} backend_entry;

void backend_register(const char *name, backend_ctor open);

const backend_entry *backend_find(const char *name);

int backend_count(void);
const backend_entry *backend_at(int i);

#define KV_REGISTER_BACKEND(NAME, OPEN)                                \
    __attribute__((constructor)) static void kv_register__##OPEN(void) \
    {                                                                  \
        backend_register((NAME), (OPEN));                              \
    }

#endif
