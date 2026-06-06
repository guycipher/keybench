#ifndef BENCH_H
#define BENCH_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline int kv_keycmp(const char *a, size_t alen, const char *b, size_t blen)
{
    size_t n = alen < blen ? alen : blen;
    int c = memcmp(a, b, n);
    if (c) return c;
    return (alen > blen) - (alen < blen);
}

#define KV_RELAXED_ADD(x, d) __atomic_fetch_add(&(x), (d), __ATOMIC_RELAXED)
#define KV_RELAXED_SET(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELAXED)
#define KV_RELAXED_GET(x)    __atomic_load_n(&(x), __ATOMIC_RELAXED)

typedef int (*kv_range_cb)(void *arg, const char *k, size_t klen, const char *v, size_t vlen);

typedef void (*kv_stat_cb)(void *arg, const char *name, double value);
typedef struct kv_backend
{
    const char *name;
    void *ctx;

    int (*put)(void *ctx, const char *k, size_t klen, const char *v, size_t vlen);
    int (*get)(void *ctx, const char *k, size_t klen, const char **vp, size_t *vlen);
    int (*del)(void *ctx, const char *k, size_t klen);
    int (*range)(void *ctx, const char *lo, size_t lolen, const char *hi, size_t hilen, int limit,
                 kv_range_cb cb, void *cbarg);
    void (*close)(void *ctx);

    const char *(*version)(void *ctx);

    void (*stats)(void *ctx, kv_stat_cb cb, void *arg);

    const char *(*datadir)(void *ctx);
} kv_backend;

typedef struct kv_options kv_options;
const char *kv_opt_str(const kv_options *o, const char *key, const char *dflt);

long long kv_opt_int(const kv_options *o, const char *key, long long dflt);

const char *kv_opt_name(const kv_options *o);
int kv_opt_count(const kv_options *o);
const char *kv_opt_key_at(const kv_options *o, int i);
const char *kv_opt_val_at(const kv_options *o, int i);

typedef kv_backend *(*backend_ctor)(unsigned seed, const char *data_dir, const kv_options *opts);

uint64_t now_ns(void);

#endif
