#define _POSIX_C_SOURCE 200809L
#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
    {
        fprintf(stderr, "keybench: out of memory\n");
        abort();
    }
    return p;
}

typedef struct
{
    char *key;
    char *val;
} cfg_kv;

struct kv_options
{
    char name[64];
    cfg_kv *kv;
    int n, cap;
};

struct cfg_file
{
    kv_options **s;
    int n, cap;
};

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s) return s;
    char *e = s + strlen(s) - 1;
    while (e > s && isspace((unsigned char)*e)) *e-- = '\0';
    return s;
}

static kv_options *section_new(const char *name)
{
    kv_options *o = xmalloc(sizeof *o);
    snprintf(o->name, sizeof o->name, "%s", name);
    o->kv = NULL;
    o->n = o->cap = 0;
    return o;
}

static void section_add(kv_options *o, const char *k, const char *v)
{
    if (o->n == o->cap)
    {
        o->cap = o->cap ? o->cap * 2 : 8;
        o->kv = realloc(o->kv, (size_t)o->cap * sizeof *o->kv);
        if (!o->kv)
        {
            fprintf(stderr, "keybench: out of memory\n");
            abort();
        }
    }
    o->kv[o->n].key = strdup(k);
    o->kv[o->n].val = strdup(v);
    o->n++;
}

static kv_options *file_section(cfg_file *c, const char *name)
{
    for (int i = 0; i < c->n; i++)
        if (!strcmp(c->s[i]->name, name)) return c->s[i];
    if (c->n == c->cap)
    {
        c->cap = c->cap ? c->cap * 2 : 8;
        c->s = realloc(c->s, (size_t)c->cap * sizeof *c->s);
        if (!c->s)
        {
            fprintf(stderr, "keybench: out of memory\n");
            abort();
        }
    }
    c->s[c->n] = section_new(name);
    return c->s[c->n++];
}

int cfg_load(cfg_file **out, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
    {
        fprintf(stderr, "cannot open config '%s'\n", path);
        return -1;
    }

    cfg_file *c = xmalloc(sizeof *c);
    c->s = NULL;
    c->n = c->cap = 0;
    kv_options *cur = file_section(c, "bench");

    char line[1024];
    while (fgets(line, sizeof line, f))
    {
        /* A comment starts only at line start or after whitespace, so a value
           such as a rocksdb nested option string keeps its inline semicolons. */
        for (char *p = line; *p; p++)
        {
            if ((*p == '#' || *p == ';') && (p == line || isspace((unsigned char)p[-1])))
            {
                *p = '\0';
                break;
            }
        }
        char *s = trim(line);
        if (!*s) continue;
        if (*s == '[')
        {
            char *e = strchr(s, ']');
            if (e)
            {
                *e = '\0';
                cur = file_section(c, trim(s + 1));
            }
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        section_add(cur, trim(s), trim(eq + 1));
    }
    fclose(f);
    *out = c;
    return 0;
}

void cfg_free(cfg_file *c)
{
    if (!c) return;
    for (int i = 0; i < c->n; i++)
    {
        kv_options *o = c->s[i];
        for (int j = 0; j < o->n; j++)
        {
            free(o->kv[j].key);
            free(o->kv[j].val);
        }
        free(o->kv);
        free(o);
    }
    free(c->s);
    free(c);
}

const kv_options *cfg_section(const cfg_file *c, const char *name)
{
    if (!c) return NULL;
    for (int i = 0; i < c->n; i++)
        if (!strcmp(c->s[i]->name, name)) return c->s[i];
    return NULL;
}

long long cfg_parse_size(const char *s, long long dflt)
{
    if (!s || !*s) return dflt;
    char *end;
    double v = strtod(s, &end);
    while (*end && isspace((unsigned char)*end)) end++;
    long long mult = 1;
    switch (toupper((unsigned char)*end))
    {
        case 'K':
            mult = 1024LL;
            break;
        case 'M':
            mult = 1024LL * 1024;
            break;
        case 'G':
            mult = 1024LL * 1024 * 1024;
            break;
        case 'T':
            mult = 1024LL * 1024 * 1024 * 1024;
            break;
        case '\0':
            mult = 1;
            break;
        default:
            return dflt;
    }
    return (long long)(v * (double)mult);
}

const char *kv_opt_str(const kv_options *o, const char *key, const char *dflt)
{
    if (!o) return dflt;
    for (int i = 0; i < o->n; i++)
        if (!strcmp(o->kv[i].key, key)) return o->kv[i].val;
    return dflt;
}

long long kv_opt_int(const kv_options *o, const char *key, long long dflt)
{
    const char *v = kv_opt_str(o, key, NULL);
    return v ? cfg_parse_size(v, dflt) : dflt;
}

int kv_opt_all(const kv_options *o, const char *key, const char **out, int max)
{
    int n = 0;
    if (!o) return 0;
    for (int i = 0; i < o->n && n < max; i++)
        if (!strcmp(o->kv[i].key, key)) out[n++] = o->kv[i].val;
    return n;
}

const char *kv_opt_name(const kv_options *o)
{
    return o ? o->name : "";
}
int kv_opt_count(const kv_options *o)
{
    return o ? o->n : 0;
}
const char *kv_opt_key_at(const kv_options *o, int i)
{
    return (o && i < o->n) ? o->kv[i].key : NULL;
}
const char *kv_opt_val_at(const kv_options *o, int i)
{
    return (o && i < o->n) ? o->kv[i].val : NULL;
}

int cfg_nsections(const cfg_file *c)
{
    return c ? c->n : 0;
}
const kv_options *cfg_section_at(const cfg_file *c, int i)
{
    return (c && i < c->n) ? c->s[i] : NULL;
}
