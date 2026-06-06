#ifndef PROBE_H
#define PROBE_H

typedef void (*probe_info_cb)(void *arg, const char *key, const char *value);
typedef void (*probe_metric_cb)(void *arg, const char *name, double value);

typedef struct
{
    const char *name;
    void (*info)(probe_info_cb cb, void *arg);
    void (*sample)(probe_metric_cb cb, void *arg);
} probe;

void probe_set_data_dir(const char *dir);
const char *probe_data_dir(void);

void probe_register(const probe *p);
const probe *probe_find(const char *name);
int probe_count(void);
const probe *probe_at(int i);

void probe_select_all(void);
void probe_select_none(void);
int probe_select(const char *name);
int probe_selected_count(void);
const probe *probe_selected_at(int i);

#define KV_REGISTER_PROBE(PVAR)                                              \
    __attribute__((constructor)) static void kv_register_probe__##PVAR(void) \
    {                                                                        \
        probe_register(&PVAR);                                               \
    }

#endif
