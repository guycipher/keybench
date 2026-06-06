#include <string.h>

#include "backend_registry.h"

#define MAX_BACKENDS    32
#define DEFAULT_BACKEND "skiplist"

static backend_entry g_backends[MAX_BACKENDS];
static int g_n;

void backend_register(const char *name, backend_ctor open)
{
    if (g_n >= MAX_BACKENDS) return;
    g_backends[g_n].name = name;
    g_backends[g_n].open = open;
    g_n++;
}

const backend_entry *backend_find(const char *name)
{
    int want_default = (name == NULL);
    if (!name) name = DEFAULT_BACKEND;
    for (int i = 0; i < g_n; i++)
        if (!strcmp(g_backends[i].name, name)) return &g_backends[i];
    if (want_default && g_n > 0) return &g_backends[0];
    return NULL;
}

int backend_count(void)
{
    return g_n;
}
const backend_entry *backend_at(int i)
{
    return &g_backends[i];
}
