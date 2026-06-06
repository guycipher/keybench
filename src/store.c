#include "store.h"

#include <stdio.h>
#include <stdlib.h>

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

struct kv_store
{
    kv_backend *be;
};

kv_store *store_open(backend_ctor inner, unsigned seed, const char *data_dir,
                     const kv_options *opts)
{
    kv_store *s = xmalloc(sizeof *s);
    s->be = inner(seed, data_dir, opts);
    return s;
}

void store_close(kv_store *s)
{
    s->be->close(s->be->ctx);
    free(s->be);
    free(s);
}

const char *store_version(const kv_store *s)
{
    return s->be->version ? s->be->version(s->be->ctx) : s->be->name;
}

const char *store_datadir(const kv_store *s)
{
    return s->be->datadir ? s->be->datadir(s->be->ctx) : NULL;
}

void store_stats(kv_store *s, kv_stat_cb cb, void *arg)
{
    if (s->be->stats) s->be->stats(s->be->ctx, cb, arg);
}

int store_put(kv_store *s, const char *k, size_t klen, const char *v, size_t vlen)
{
    return s->be->put(s->be->ctx, k, klen, v, vlen);
}

int store_putbatch(kv_store *s, const char *const *keys, const size_t *klens,
                   const char *const *vals, const size_t *vlens, int n)
{
    if (s->be->putbatch) return s->be->putbatch(s->be->ctx, keys, klens, vals, vlens, n);
    int rc = 0;
    for (int i = 0; i < n; i++)
        if (s->be->put(s->be->ctx, keys[i], klens[i], vals[i], vlens[i]) != 0) rc = -1;
    return rc;
}

int store_get(kv_store *s, const char *k, size_t klen, char **vp, size_t *vlen)
{
    const char *bv;
    size_t bl;
    int found = s->be->get(s->be->ctx, k, klen, &bv, &bl);
    if (found)
    {
        *vp = (char *)bv;
        *vlen = bl;
    }
    return found;
}

int store_del(kv_store *s, const char *k, size_t klen)
{
    return s->be->del(s->be->ctx, k, klen);
}

int store_delbatch(kv_store *s, const char *const *keys, const size_t *klens, int n)
{
    if (s->be->delbatch) return s->be->delbatch(s->be->ctx, keys, klens, n);
    int rc = 0;
    for (int i = 0; i < n; i++)
        if (s->be->del(s->be->ctx, keys[i], klens[i]) < 0) rc = -1;
    return rc;
}

int store_range(kv_store *s, const char *lo, size_t lolen, const char *hi, size_t hilen, int limit,
                kv_range_cb cb, void *arg)
{
    return s->be->range(s->be->ctx, lo, lolen, hi, hilen, limit, cb, arg);
}
