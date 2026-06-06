#include "probe.h"

#include <string.h>

#define MAX_PROBES 32

static const probe *g_all[MAX_PROBES];
int g_nall;
static const probe *g_sel[MAX_PROBES];
int g_nsel;
static const char *g_data_dir = "/tmp";

void probe_set_data_dir(const char *dir)
{
    if (dir && *dir) g_data_dir = dir;
}
const char *probe_data_dir(void)
{
    return g_data_dir;
}

void probe_register(const probe *p)
{
    if (g_nall < MAX_PROBES) g_all[g_nall++] = p;
}

const probe *probe_find(const char *name)
{
    for (int i = 0; i < g_nall; i++)
        if (!strcmp(g_all[i]->name, name)) return g_all[i];
    return NULL;
}

int probe_count(void)
{
    return g_nall;
}
const probe *probe_at(int i)
{
    return (i >= 0 && i < g_nall) ? g_all[i] : NULL;
}

void probe_select_all(void)
{
    for (int i = 0; i < g_nall; i++) g_sel[i] = g_all[i];
    g_nsel = g_nall;
}
void probe_select_none(void)
{
    g_nsel = 0;
}

int probe_select(const char *name)
{
    const probe *p = probe_find(name);
    if (!p) return -1;
    if (g_nsel < MAX_PROBES) g_sel[g_nsel++] = p;
    return 0;
}

int probe_selected_count(void)
{
    return g_nsel;
}
const probe *probe_selected_at(int i)
{
    return (i >= 0 && i < g_nsel) ? g_sel[i] : NULL;
}
