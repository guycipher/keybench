#ifndef CONFIG_H
#define CONFIG_H

#include "bench.h"

typedef struct cfg_file cfg_file;

int cfg_load(cfg_file **c, const char *path);
void cfg_free(cfg_file *c);

const kv_options *cfg_section(const cfg_file *c, const char *name);

int kv_opt_all(const kv_options *o, const char *key, const char **out, int max);

long long cfg_parse_size(const char *s, long long dflt);

int cfg_nsections(const cfg_file *c);
const kv_options *cfg_section_at(const cfg_file *c, int i);

#endif
